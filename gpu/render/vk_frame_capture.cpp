// SPDX-License-Identifier: GPL-2.0-or-later
#include "render/vk_frame_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <mutex>

#include "bbport_toggles.h"
#include "vulkan/vk_common.h"
#include "textures/image_info.h"

namespace Vulkan {

namespace {

struct Target {
    VAddr address{};
    vk::Format format{};
    u32 width{}, height{};
    bool operator==(const Target&) const = default;
};

struct Entry {
    bool compute{};
    std::vector<Target> colors;
    Target depth;
    u32 draws{};
    u64 indices{};
    std::vector<u64> shaders; ///< vs/ps pairs (graphics) or cs (compute), first few
    std::vector<std::pair<Target, bool>> sampled; ///< textures (false) and storage images (true)
    std::string note;
    std::vector<std::string> buffers; ///< dumped constants, per draw
};

// GPU thread only.
std::vector<Entry> entries;
std::vector<std::pair<Target, bool>> pending_sampled;
std::vector<std::string> pending_buffers;

void AddBuffers(Entry& entry) {
    // Constants of full-screen passes (post-processing: camera matrices) and of the first draw
    // of each geometry pass.
    if (entry.draws <= 4) {
        for (auto& b : pending_buffers) {
            entry.buffers.push_back(std::move(b));
        }
    }
    pending_buffers.clear();
}
bool pass_open = false;
std::mutex display_mutex;
std::vector<VAddr> display_buffers;
// bbport: read on every draw; the game registers a handful of display buffers once.
std::array<std::atomic<VAddr>, 16> display_list{};
std::atomic<u32> display_count{0};

Target ToTarget(const VideoCore::ImageInfo& info) {
    return {info.guest_address, info.pixel_format, info.size.width, info.size.height};
}

std::string Describe(const Target& t) {
    if (!t.address) {
        return "-";
    }
    return std::format("{:#x} {} {}x{}", t.address, vk::to_string(t.format), t.width, t.height);
}

void AddSampled(Entry& entry) {
    for (const auto& s : pending_sampled) {
        if (entry.sampled.size() < 32 &&
            std::ranges::find(entry.sampled, s) == entry.sampled.end()) {
            entry.sampled.push_back(s);
        }
    }
    pending_sampled.clear();
}

void AddShader(Entry& entry, u64 hash) {
    if (entry.shaders.size() < 8 && std::ranges::find(entry.shaders, hash) == entry.shaders.end()) {
        entry.shaders.push_back(hash);
    }
}

void Write(VAddr presented) {
    const char* dir = std::getenv("BB_CAPTURE_DIR");
    const std::string path =
        std::format("{}/frame_{}.txt", dir ? dir : ".", static_cast<long long>(std::time(nullptr)));
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::printf("Frame capture: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "presented buffer %#llx\n", static_cast<unsigned long long>(presented));
    u32 index = 0;
    for (const auto& e : entries) {
        if (e.compute) {
            std::fprintf(f, "\n#%u COMPUTE dispatches %u, cs", index++, e.draws);
        } else {
            std::fprintf(f, "\n#%u PASS draws %u (indices %llu), vs/ps", index++, e.draws,
                         static_cast<unsigned long long>(e.indices));
        }
        for (const u64 h : e.shaders) {
            std::fprintf(f, " %016llx", static_cast<unsigned long long>(h));
        }
        std::fprintf(f, "\n");
        for (size_t i = 0; i < e.colors.size(); ++i) {
            std::fprintf(f, "  color%zu %s\n", i, Describe(e.colors[i]).c_str());
        }
        if (!e.compute) {
            std::fprintf(f, "  depth  %s\n", Describe(e.depth).c_str());
        }
        for (const auto& [t, storage] : e.sampled) {
            std::fprintf(f, "  %s %s\n", storage ? "writes " : "samples", Describe(t).c_str());
        }
        if (!e.note.empty()) {
            std::fprintf(f, "  note: %s\n", e.note.c_str());
        }
        for (const auto& b : e.buffers) {
            std::fprintf(f, "%s\n", b.c_str());
        }
    }
    std::fclose(f);
    std::printf("Frame capture: %zu passes written to %s\n", entries.size(), path.c_str());
}

} // namespace

namespace {

// ---- BB_PASS_TRACE (bench/vertex-passes) -------------------------------------------------------
// One line per render encoder, so a Metal System Trace's per-encoder vertex time can be joined to
// the identity the trace does not carry. Off unless BB_PASS_TRACE names a file; even then it only
// writes inside a window (see PassTraceFlip).
struct PassLine {
    u64 frame{};
    u64 ordinal{};      ///< render encoders since this frame began
    u32 width{}, height{}, num_colors{};
    std::string color0, color1, depth;   ///< vk::Format names, "-" when absent
    u32 draws{}, samplers{};
    u64 indices{}, instances{};
    std::vector<u64> vs, ps;
    u64 t0{};           ///< steady-clock ns at the encoder's begin, relative to the window's first
};

bool pass_trace_enabled = false;
std::string pass_trace_path, pass_trace_trigger;
u64 pass_trace_frames = 200;
u64 pass_trace_min_draws = 400;   ///< the trigger only arms on a frame that draws the game
u64 pass_trace_draws_at_flip = 0;
bool pass_trace_armed = false;   ///< the trigger fired; the next encoder opens the file
bool pass_trace_open = false;
FILE* pass_trace_file = nullptr;
u64 pass_trace_window_frame0 = 0;
u64 pass_trace_last_frame = ~0ull;
u64 pass_trace_ordinal = 0;
u64 pass_trace_t_base = 0;
bool pass_trace_in_pass = false;
PassLine pass_trace_line;
// The attachments of the pass the rasterizer is about to begin (stashed by PassTraceTargets, taken
// by the first encoder that actually begins).
bool pass_trace_have_targets = false;
std::string pass_trace_c0, pass_trace_c1, pass_trace_d;
u32 pass_trace_ncolors = 0;
std::atomic<u64> pass_trace_flips{0}, pass_trace_flips_at_arm{0};

const char* FormatName(vk::Format format) {
    static thread_local std::string text;
    text = vk::to_string(format);
    return text.c_str();
}

void PassTraceInit() {
    static const bool once = [] {
        const char* path = std::getenv("BB_PASS_TRACE");
        pass_trace_enabled = path && path[0];
        if (!pass_trace_enabled) {
            return true;
        }
        pass_trace_path = path;
        if (const char* trigger = std::getenv("BB_PASS_TRACE_TRIGGER")) {
            pass_trace_trigger = trigger;
        }
        if (const char* frames = std::getenv("BB_PASS_TRACE_FRAMES")) {
            pass_trace_frames = std::strtoull(frames, nullptr, 10);
            if (pass_trace_frames == 0) {
                pass_trace_frames = 200;
            }
        }
        // No trigger file: record from the start (short runs), otherwise wait for the flip.
        pass_trace_armed = pass_trace_trigger.empty();
        std::printf("Pass trace: BB_PASS_TRACE=%s frames=%llu trigger=%s\n", pass_trace_path.c_str(),
                    static_cast<unsigned long long>(pass_trace_frames),
                    pass_trace_trigger.empty() ? "-" : pass_trace_trigger.c_str());
        std::fflush(stdout);
        return true;
    }();
    (void)once;
}

void PassTraceOpen(u64 frame) {
    pass_trace_file = std::fopen(pass_trace_path.c_str(), "w");
    if (!pass_trace_file) {
        std::printf("Pass trace: cannot write %s\n", pass_trace_path.c_str());
        pass_trace_enabled = false;
        return;
    }
    pass_trace_open = true;
    pass_trace_window_frame0 = frame;
    pass_trace_last_frame = ~0ull;
    pass_trace_ordinal = 0;
    pass_trace_t_base = 0;
    // One header, so a reader can tell a truncated window from a whole one.
    std::fprintf(pass_trace_file,
                 "# BB_PASS_TRACE frame0=%llu frames=%llu one line per render encoder: "
                 "f frame, i encoder index in the frame, t target, nc colour attachments, "
                 "c0/c1/d attachment formats, dr draws, idx indices (x instances), "
                 "vs/ps shader hashes\n",
                 static_cast<unsigned long long>(frame),
                 static_cast<unsigned long long>(pass_trace_frames));
    std::fflush(pass_trace_file);
    std::printf("Pass trace: window from frame %llu to %llu\n",
                static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(frame + pass_trace_frames));
    std::fflush(stdout);
}

void PassTraceFlush() {
    if (!pass_trace_in_pass || !pass_trace_open) {
        pass_trace_in_pass = false;
        return;
    }
    pass_trace_in_pass = false;
    const auto& l = pass_trace_line;
    std::fprintf(pass_trace_file, "PT %llu %llu %ux%u nc=%u c0=%s c1=%s d=%s dr=%u idx=%llu inst=%llu vs=",
                 static_cast<unsigned long long>(l.frame),
                 static_cast<unsigned long long>(l.ordinal), l.width, l.height, l.num_colors,
                 l.color0.c_str(), l.color1.c_str(), l.depth.c_str(), l.draws,
                 static_cast<unsigned long long>(l.indices),
                 static_cast<unsigned long long>(l.instances));
    for (size_t i = 0; i < l.vs.size(); ++i) {
        std::fprintf(pass_trace_file, "%s%016llx", i ? "," : "",
                     static_cast<unsigned long long>(l.vs[i]));
    }
    if (l.vs.empty()) {
        std::fprintf(pass_trace_file, "-");
    }
    std::fprintf(pass_trace_file, " ps=");
    for (size_t i = 0; i < l.ps.size(); ++i) {
        std::fprintf(pass_trace_file, "%s%016llx", i ? "," : "",
                     static_cast<unsigned long long>(l.ps[i]));
    }
    if (l.ps.empty()) {
        std::fprintf(pass_trace_file, "-");
    }
    std::fprintf(pass_trace_file, " t=%llu\n", static_cast<unsigned long long>(l.t0));
    std::fflush(pass_trace_file);
}

} // namespace

bool FrameCapture::PassTraceEnabled() {
    PassTraceInit();
    return pass_trace_enabled && pass_trace_open;
}

void FrameCapture::PassTraceFlip() {
    PassTraceInit();
    if (!pass_trace_enabled) {
        return;
    }
    const u64 flip = pass_trace_flips.fetch_add(1, std::memory_order_relaxed) + 1;
    if (pass_trace_armed || pass_trace_open) {
        return;
    }
    if (pass_trace_trigger.empty()) {
        return;
    }
    if (!std::filesystem::exists(pass_trace_trigger)) {
        return;
    }
    // Only a frame that actually draws the game arms the window: the trigger can land while the
    // title still owns the screen (a boot frame has 4 encoders, a gameplay frame ~190).
    const u64 draws = BbStats::draws.load(std::memory_order_relaxed);
    const u64 since = draws - pass_trace_draws_at_flip;
    pass_trace_draws_at_flip = draws;
    if (since < pass_trace_min_draws) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(pass_trace_trigger, ec);
    pass_trace_armed = true;
    pass_trace_flips_at_arm.store(flip, std::memory_order_relaxed);
    std::printf("Pass trace: armed at flip %llu (%llu draws since the last flip)\n",
                static_cast<unsigned long long>(flip), static_cast<unsigned long long>(since));
    std::fflush(stdout);
}

void FrameCapture::PassTraceTargets(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                                    const VideoCore::ImageInfo* depth) {
    PassTraceInit();
    if (!pass_trace_enabled) {
        return;
    }
    pass_trace_ncolors = num_colors;
    pass_trace_c0 = num_colors > 0 && colors[0] ? FormatName(colors[0]->pixel_format) : "-";
    pass_trace_c1 = num_colors > 1 && colors[1] ? FormatName(colors[1]->pixel_format) : "-";
    pass_trace_d = depth ? FormatName(depth->pixel_format) : "-";
    pass_trace_have_targets = true;
}

void FrameCapture::PassTraceBegin(u32 width, u32 height, u32 num_colors) {
    PassTraceInit();
    if (!pass_trace_enabled) {
        return;
    }
    // A new encoder always ends the previous one first; EndRendering does that, but a split path
    // that reopens without an end must not lose the line.
    PassTraceFlush();
    const u64 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    if (!pass_trace_open) {
        if (!pass_trace_armed || frame == 0) {
            return;
        }
        PassTraceOpen(frame);
    } else if (frame >= pass_trace_window_frame0 + pass_trace_frames) {
        std::fclose(pass_trace_file);
        pass_trace_file = nullptr;
        pass_trace_open = false;
        pass_trace_enabled = false;
        std::printf("Pass trace: window closed at frame %llu\n",
                    static_cast<unsigned long long>(frame));
        std::fflush(stdout);
        return;
    }
    if (frame != pass_trace_last_frame) {
        pass_trace_last_frame = frame;
        pass_trace_ordinal = 0;
    }
    auto& l = pass_trace_line;
    l = PassLine{};
    l.frame = frame;
    l.ordinal = pass_trace_ordinal++;
    l.width = width;
    l.height = height;
    l.num_colors = pass_trace_have_targets ? pass_trace_ncolors : num_colors;
    l.color0 = pass_trace_have_targets ? pass_trace_c0 : "-";
    l.color1 = pass_trace_have_targets ? pass_trace_c1 : "-";
    l.depth = pass_trace_have_targets ? pass_trace_d : "-";
    pass_trace_have_targets = false;
    const auto now = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
    if (pass_trace_t_base == 0) {
        pass_trace_t_base = now;
    }
    l.t0 = now - pass_trace_t_base;
    pass_trace_in_pass = true;
}

void FrameCapture::PassTraceDraw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances) {
    if (!pass_trace_enabled || !pass_trace_open || !pass_trace_in_pass) {
        return;
    }
    auto& l = pass_trace_line;
    ++l.draws;
    l.indices += u64(num_indices) * std::max(num_instances, 1u);
    l.instances += std::max(num_instances, 1u);
    if (l.vs.size() < 4 && std::ranges::find(l.vs, vs_hash) == l.vs.end()) {
        l.vs.push_back(vs_hash);
    }
    if (ps_hash && l.ps.size() < 4 && std::ranges::find(l.ps, ps_hash) == l.ps.end()) {
        l.ps.push_back(ps_hash);
    }
}

void FrameCapture::PassTraceEnd() {
    if (!pass_trace_enabled || !pass_trace_open || !pass_trace_in_pass) {
        return;
    }
    PassTraceFlush();
}

void FrameCapture::PassTraceExternal(u32 width, u32 height, u32 draws, const char* what) {
    if (!pass_trace_enabled || !pass_trace_open) {
        return;
    }
    const u64 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    std::fprintf(pass_trace_file, "PT %llu %llu %ux%u nc=0 c0=- c1=- d=- dr=%u idx=0 inst=0 vs=- ps=- t=0 %s\n",
                 static_cast<unsigned long long>(frame),
                 static_cast<unsigned long long>(pass_trace_ordinal++), width, height, draws, what);
    std::fflush(pass_trace_file);
}

void FrameCapture::OnFlip(VAddr presented_address) {
    last_presented.store(presented_address, std::memory_order_relaxed);
    flips.fetch_add(1, std::memory_order_release);
    PassTraceFlip();
    static const char* trigger = std::getenv("BB_CAPTURE_TRIGGER");
    if (trigger && state.load(std::memory_order_relaxed) == Idle &&
        std::filesystem::exists(trigger)) {
        std::error_code ec;
        std::filesystem::remove(trigger, ec);
        state.store(Armed, std::memory_order_release);
        std::printf("Frame capture: armed\n");
    }
}

void FrameCapture::Poll() {}

bool FrameCapture::IsDisplayBuffer(VAddr address) {
    const u32 count = display_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        if (display_list[i].load(std::memory_order_relaxed) == address) {
            return true;
        }
    }
    if (count < display_list.size()) {
        return false;
    }
    std::scoped_lock lk{display_mutex};
    return std::ranges::find(display_buffers, address) != display_buffers.end();
}

void FrameCapture::AddDisplayBuffer(VAddr address) {
    std::scoped_lock lk{display_mutex};
    if (std::ranges::find(display_buffers, address) == display_buffers.end()) {
        display_buffers.push_back(address);
        const u32 count = display_count.load(std::memory_order_relaxed);
        if (count < display_list.size()) {
            display_list[count].store(address, std::memory_order_relaxed);
            display_count.store(count + 1, std::memory_order_release);
        }
    }
}

void FrameCapture::BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                             const VideoCore::ImageInfo* depth) {
    std::vector<Target> targets;
    bool display = false;
    {
        std::scoped_lock lk{display_mutex};
        for (u32 i = 0; i < num_colors; ++i) {
            targets.push_back(colors[i] ? ToTarget(*colors[i]) : Target{});
            display |= colors[i] && std::ranges::find(display_buffers, colors[i]->guest_address) !=
                                        display_buffers.end();
        }
    }
    // Frame boundaries in the command stream: the pass that writes a display buffer.
    const bool new_pass = !(pass_open && !entries.empty() && !entries.back().compute &&
                            entries.back().colors == targets);
    if (display && new_pass) {
        if (state.load(std::memory_order_relaxed) == Recording) {
            Write(last_presented.load(std::memory_order_relaxed));
            entries.clear();
            pending_sampled.clear();
            pending_buffers.clear();
            state.store(Idle, std::memory_order_release);
            return;
        }
        entries.clear();
        pending_sampled.clear();
        pending_buffers.clear();
        pass_open = false;
        state.store(Recording, std::memory_order_release);
    }
    if (state.load(std::memory_order_relaxed) != Recording) {
        pending_sampled.clear();
        pending_buffers.clear();
        return;
    }
    const Target d = depth ? ToTarget(*depth) : Target{};
    if (pass_open && !entries.empty() && !entries.back().compute &&
        entries.back().colors == targets && entries.back().depth == d) {
        return;
    }
    entries.push_back({.compute = false, .colors = std::move(targets), .depth = d});
    pass_open = true;
}

void FrameCapture::Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances) {
    if (state.load(std::memory_order_relaxed) != Recording || entries.empty() ||
        entries.back().compute) {
        return;
    }
    auto& e = entries.back();
    ++e.draws;
    e.indices += u64(num_indices) * std::max(num_instances, 1u);
    AddShader(e, vs_hash);
    AddShader(e, ps_hash);
    AddSampled(e);
    AddBuffers(e);
}

void FrameCapture::Dispatch(u64 cs_hash, u32 x, u32 y, u32 z) {
    if (state.load(std::memory_order_relaxed) != Recording) {
        pending_sampled.clear();
        pending_buffers.clear();
        return;
    }
    if (!entries.empty() && entries.back().compute && !entries.back().shaders.empty() &&
        entries.back().shaders[0] == cs_hash) {
        ++entries.back().draws;
    } else {
        entries.push_back({.compute = true, .draws = 1, .shaders = {cs_hash}});
        entries.back().note = std::format("groups {}x{}x{}", x, y, z);
    }
    AddSampled(entries.back());
    AddBuffers(entries.back());
    pass_open = false;
}

void FrameCapture::Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size) {
    const u64 bytes = std::min<u64>(size, 1024) & ~u64(3);
    std::string out = std::format("  buffer stage {:016x} slot {} at {:#x} size {}:", stage_hash,
                                  slot, address, size);
    const auto* words = static_cast<const float*>(data);
    for (u64 i = 0; i < bytes / 4; ++i) {
        if (i % 8 == 0) {
            out += std::format("\n    [{:3}]", i);
        }
        out += std::format(" {:12.6g}", words[i]);
    }
    pending_buffers.push_back(std::move(out));
}

void FrameCapture::Sampled(const VideoCore::ImageInfo& info, bool storage) {
    if (pending_sampled.size() < 64) {
        pending_sampled.emplace_back(ToTarget(info), storage);
    }
}

void FrameCapture::Note(const char* text) {
    if (!entries.empty()) {
        entries.back().note += text;
    }
}

} // namespace Vulkan
