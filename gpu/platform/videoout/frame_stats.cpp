// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_FRAME_STATS=1: every 5 seconds, the flip rate, frame time spread and what the GPU
// side spent its time on; long frames (over 40 ms) get a line of their own. BB_FRAME_LOG=path:
// one CSV line per flip.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "bbport_cpu.h"
#include "bbport_heap_sites.h"
#include "bbport_threads.h"
#include "bbport_toggles.h"
#include "bbport_wait_trace.h"
#include "core/emulator_settings.h"
#include "platform/kernel/event_queue.h"
#include "platform/videoout/frame_stats.h"
#include "render/vk_breadcrumbs.h"
#include "memory/fault_sites.h"
#include "vulkan/instance.h"
#include "render/scheduler.h"
#if __has_include("remote/bb_remote.h")
#include "remote/bb_remote.h" // the native line: whether this process is bb-gpu (Back)
#else
// The main (in-process) line has no bb-gpu process: the frame-stats extras print without it.
namespace BbRemote {
inline bool BackActive() { return false; }
namespace Back {
inline void GameStats(double, double) {}
struct GamePhaseTotals {
    u64 fault_ns, faults, call_ns, calls, file_reads, file_bytes, file_ns, cpu_us, majflt;
};
inline bool GamePhases(GamePhaseTotals&) { return false; }
} // namespace Back
} // namespace BbRemote
#endif

extern "C" void runtime_sleep_stats(uint64_t* calls, uint64_t* ns);
extern "C" void runtime_wait_report(double frames);

namespace Vulkan {
extern std::atomic<u64> g_bb_compile_ns;
extern std::atomic<u32> g_bb_compiles;
extern std::atomic<u64> g_bb_compile_port_ns;
extern std::atomic<u64> g_bb_compile_driver_ns;
} // namespace Vulkan

namespace BbVideoOut {

bool FrameStatsEnabled() {
    static const bool enabled = EmulatorSettingsImpl::Flag("BB_FRAME_STATS", false);
    return enabled;
}

/// bbport: memory per statistics window: the kernel's count for the game (VRAM and GTT of its
/// DRM clients, RSS) and the parts we know of. GTT holds the game's direct memory (fixed).
static void PrintMemory() {
    u64 vram_kib = 0, gtt_kib = 0;
    std::vector<u64> clients;
    if (DIR* dir = opendir("/proc/self/fdinfo")) {
        while (const dirent* entry = readdir(dir)) {
            char path[64];
            std::snprintf(path, sizeof(path), "/proc/self/fdinfo/%s", entry->d_name);
            FILE* file = std::fopen(path, "r");
            if (!file) {
                continue;
            }
            char line[160];
            bool drm = false;
            unsigned long long client = 0, vram = 0, gtt = 0;
            while (std::fgets(line, sizeof(line), file)) {
                drm |= std::strncmp(line, "drm-driver:", 11) == 0;
                std::sscanf(line, "drm-client-id: %llu", &client);
                std::sscanf(line, "drm-memory-vram: %llu", &vram);
                std::sscanf(line, "drm-memory-gtt: %llu", &gtt);
            }
            std::fclose(file);
            // Duplicated descriptors share a client.
            if (drm && std::find(clients.begin(), clients.end(), client) == clients.end()) {
                clients.push_back(client);
                vram_kib += vram;
                gtt_kib += gtt;
            }
        }
        closedir(dir);
    }
    unsigned long long size_pages = 0, rss_pages = 0;
    if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(statm, "%llu %llu", &size_pages, &rss_pages) != 2) {
            rss_pages = 0;
        }
        std::fclose(statm);
    }
    u64 rss = u64(rss_pages) * u64(sysconf(_SC_PAGESIZE));
#ifdef __APPLE__
    // bbport (macOS): no /proc; resident size from Mach (VRAM/GTT stay 0: unified memory).
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        rss = info.resident_size;
    }
#endif
    const u64 device = BbStats::device_alloc_bytes.load() - BbStats::device_free_bytes.load();
    u64 vma_blocks = 0, vma_used = 0;
    Vulkan::VmaDeviceUsage(vma_blocks, vma_used);
    std::printf("Memory: VRAM %llu MiB, GTT %llu MiB, RSS %llu MiB; Vulkan memory %llu MiB "
                "allocated (VMA blocks %llu MiB, %llu MiB of them used); Vulkan images %llu MiB; "
                "cached images %llu MiB in %llu; guest blocks in VRAM %llu MiB allocated, "
                "%llu MiB of it unused (%llu MiB moved back idle, %llu MiB unmapped, %llu MiB freed; %llu chunks, %llu emptied, blocks kept: %llu GPU-written, %llu not direct memory); texture collector: usage %llu MiB, starts at %llu MiB, %llu images freed, %llu GPU-written kept\n",
                (unsigned long long)(vram_kib >> 10), (unsigned long long)(gtt_kib >> 10),
                (unsigned long long)(rss >> 20), (unsigned long long)(device >> 20),
                (unsigned long long)(vma_blocks >> 20), (unsigned long long)(vma_used >> 20),
                (unsigned long long)(BbStats::vk_image_bytes.load() >> 20),
                (unsigned long long)(BbStats::live_image_bytes.load() >> 20),
                (unsigned long long)BbStats::live_images.load(),
                (unsigned long long)(BbStats::residency_alloc_bytes.load() >> 20),
                (unsigned long long)(BbStats::residency_unused_bytes.load() >> 20),
                (unsigned long long)(BbStats::vram_idle_bytes.exchange(0) >> 20),
                (unsigned long long)(BbStats::vram_unmapped_bytes.exchange(0) >> 20),
                (unsigned long long)(BbStats::vram_chunks_freed_bytes.exchange(0) >> 20),
                (unsigned long long)BbStats::residency_chunk_count.load(),
                (unsigned long long)BbStats::evacuations.exchange(0),
                (unsigned long long)BbStats::evac_kept_gpu.exchange(0),
                (unsigned long long)BbStats::evac_kept_other.exchange(0),
                (unsigned long long)(BbStats::gc_used_bytes.load() >> 20),
                (unsigned long long)(BbStats::gc_trigger_bytes.load() >> 20),
                (unsigned long long)BbStats::gc_freed_images.exchange(0),
                (unsigned long long)BbStats::gc_kept_images.exchange(0));
}

/// bbport: the frame log (BB_FRAME_LOG), opened once. One stream for the log and for the capture
/// hotkey's marker: the log's own file offset advances with its writes, so a second stream's
/// marker (written at the end of the file) would be written over by the log's next flush.
static FILE* OpenFrameLog() {
    static FILE* const log = [] () -> FILE* {
        const char* path = std::getenv("BB_FRAME_LOG");
        FILE* file = path && *path ? std::fopen(path, "w") : nullptr;
        if (file) {
            std::fprintf(file, "t_s,frame_ms,present_wait_ms,tick_wait_ms,stage_a_cpu_ms,"
                               "images,image_mb,buffer_mb,protect_ms,resident_ms,create_ms,"
                               "refresh_ms,write_fault_ms,read_faults,guest_copy_mb,draws,"
                               "dispatches,submissions,compiles,preupload_mb,kernel_ms,alloc_mb,"
                               "free_mb,"
                               // The frame phases (FramePhases): which one owns a long frame.
                               "translate_ms,translates,pipeline_sync_ms,pipelines_sync,"
                               "async_skips,descriptor_ms,descriptor_batches,ipc_ms,ipc_calls,"
                               "gpu_idle_ms,game_fault_ms,game_faults,game_call_ms,game_calls,"
                               "game_read_ms,game_read_mb,game_reads,game_cpu_ms,game_majflt,"
                               "fault_handler_ms,fault_handlers,fault_drain_ms,fault_drains,"
                               // The steady-state probe (FrameProbe below).
                               "ga_wall_ms,ga_user_ms,ga_sys_ms,ga_drain_ms,ga_drains,ga_yields,"
                               "b_busy_ms,b_user_ms,b_sys_ms,cat_state_ms,cat_pipeline_ms,"
                               "cat_bind_ms,bind_copy_mb,cat_emit_ms\n");
        }
        return file;
    }();
    return log;
}

/// bbport: the frame phases (BbStats::t_translate and the rest, and the game process's
/// SharedState totals in the native split) as per-frame deltas: what a long frame that has no
/// compiles spent its time on. Taken once per flip; the totals only count with BB_FRAME_STATS.
struct FramePhases {
    double translate_ms, pipeline_sync_ms, descriptor_ms, ipc_ms, gpu_idle_ms;
    u64 translates, pipelines_sync, async_skips, descriptor_batches, ipc_calls;
    double fault_handler_ms, fault_drain_ms; ///< bb-gpu's side of the game's page faults
    u64 fault_handlers, fault_drains;
    bool game; ///< the game process's columns are valid (native split)
    double game_fault_ms, game_call_ms, game_read_ms, game_read_mb, game_cpu_ms;
    u64 game_faults, game_calls, game_reads, game_majflt;
};
static FramePhases TakeFramePhases() {
    static u64 last[14]{};
    static BbRemote::Back::GamePhaseTotals last_game{};
    static bool have_game = false;
    const u64 now[14] = {BbStats::t_translate.load(),        BbStats::translates.load(),
                         BbStats::t_pipeline_sync.load(),    BbStats::pipelines_sync.load(),
                         BbStats::async_skips.load(),        BbStats::t_descriptors.load(),
                         BbStats::descriptor_batches.load(), BbStats::t_ipc_call.load(),
                         BbStats::ipc_calls.load(),          BbStats::gpu_idle_total_ns.load(),
                         BbStats::t_fault_handler.load(),    BbStats::fault_handlers.load(),
                         BbStats::t_fault_drain.load(),      BbStats::fault_drains.load()};
    u64 d[14];
    for (int i = 0; i < 14; ++i) {
        d[i] = now[i] - last[i];
        last[i] = now[i];
    }
    FramePhases p{};
    p.translate_ms = d[0] / 1e6;
    p.translates = d[1];
    p.pipeline_sync_ms = d[2] / 1e6;
    p.pipelines_sync = d[3];
    p.async_skips = d[4];
    p.descriptor_ms = d[5] / 1e6;
    p.descriptor_batches = d[6];
    p.ipc_ms = d[7] / 1e6;
    p.ipc_calls = d[8];
    p.gpu_idle_ms = d[9] / 1e6;
    p.fault_handler_ms = d[10] / 1e6;
    p.fault_handlers = d[11];
    p.fault_drain_ms = d[12] / 1e6;
    p.fault_drains = d[13];
    BbRemote::Back::GamePhaseTotals game{};
    if (BbRemote::BackActive() && BbRemote::Back::GamePhases(game)) {
        if (have_game) {
            p.game = true;
            p.game_fault_ms = (game.fault_ns - last_game.fault_ns) / 1e6;
            p.game_faults = game.faults - last_game.faults;
            p.game_call_ms = (game.call_ns - last_game.call_ns) / 1e6;
            p.game_calls = game.calls - last_game.calls;
            p.game_read_ms = (game.file_ns - last_game.file_ns) / 1e6;
            p.game_read_mb = (game.file_bytes - last_game.file_bytes) / 1e6;
            p.game_reads = game.file_reads - last_game.file_reads;
            p.game_cpu_ms = (game.cpu_us - last_game.cpu_us) / 1e3;
            p.game_majflt = game.majflt - last_game.majflt;
        }
        last_game = game;
        have_game = true;
    }
    return p;
}

/// bbport (fix/perf-stage-a): the steady-state probe, per flip. The two threads every frame goes
/// through (BbStats::gpu_thread_port, record_thread_port), their user and system CPU time read
/// from Mach at the flip itself (stage_a_cpu_ms is sampled at the last graphics submission). Stage
/// A's busy wall (the frame minus its wait for guest work) against its CPU time: user ~ wall is
/// work to optimize, a large sys share is syscalls (its drains' sched_yield fallbacks, counted),
/// wall >> user+sys is waiting off-CPU. Stage B's busy time, and the four categories of draw work.
struct FrameProbe {
    double ga_wall_ms, ga_user_ms, ga_sys_ms, ga_drain_ms, b_busy_ms, b_user_ms, b_sys_ms;
    double cat_ms[BbStats::CatCount], bind_copy_mb;
    u64 ga_drains, ga_yields;
};
static FrameProbe TakeFrameProbe(double frame_ms, double gpu_idle_ms) {
    static u64 last_ga[2], last_b[2], last_counts[3], last_cat[BbStats::CatCount], last_bytes;
    static const double ticks_per_ms = double(BbCpu::TicksFromUs(1000000)) / 1000.0;
    FrameProbe p{};
    const auto cpu = [](std::atomic<std::uint32_t>& port, u64 (&last)[2], double& user_ms,
                        double& sys_ms) {
        u64 user = 0, sys = 0;
#ifdef __APPLE__
        if (!BbThreads::ThreadCpuUs(port.load(std::memory_order_acquire), user, sys)) {
            return;
        }
#else
        (void)port;
        return;
#endif
        if (last[0] || last[1]) {
            user_ms = (user - last[0]) / 1e3;
            sys_ms = (sys - last[1]) / 1e3;
        }
        last[0] = user;
        last[1] = sys;
    };
    cpu(BbStats::gpu_thread_port, last_ga, p.ga_user_ms, p.ga_sys_ms);
    cpu(BbStats::record_thread_port, last_b, p.b_user_ms, p.b_sys_ms);
    p.ga_wall_ms = std::max(0.0, frame_ms - gpu_idle_ms);
    const u64 counts[3] = {BbStats::ga_drain_ticks.load(), BbStats::ga_drains.load(),
                           BbStats::ga_yields.load()};
    p.ga_drain_ms = (counts[0] - last_counts[0]) / ticks_per_ms;
    p.ga_drains = counts[1] - last_counts[1];
    p.ga_yields = counts[2] - last_counts[2];
    std::copy(std::begin(counts), std::end(counts), std::begin(last_counts));
    static u64 last_busy;
    const u64 busy = BbStats::b_busy_ticks.load();
    p.b_busy_ms = (busy - last_busy) / ticks_per_ms;
    last_busy = busy;
    for (u32 c = 0; c < BbStats::CatCount; ++c) {
        const u64 now = BbStats::cat_ticks[c].load(std::memory_order_relaxed);
        p.cat_ms[c] = (now - last_cat[c]) / ticks_per_ms;
        last_cat[c] = now;
    }
    const u64 bytes = BbStats::bind_copy_bytes.load();
    p.bind_copy_mb = (bytes - last_bytes) / 1e6;
    last_bytes = bytes;
    return p;
}

void OnFlipStats() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point window_start = Clock::now(), last = window_start;
    static u32 frames;
    static double worst_ms;
    const auto now = Clock::now();
    const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count();
    worst_ms = std::max(worst_ms, frame_ms);
    static std::vector<double> intervals;
    intervals.push_back(frame_ms);
    last = now;
    // Stall diagnostics: what happened during a long frame.
    static u64 last_gpu_ns, last_images, last_image_bytes, last_buffer_bytes;
    static u64 last_t[6], last_minflt, last_sigf, last_pc, last_pp, last_rc, last_rp;
    const u64 pc = BbStats::protect_calls.load(), pp = BbStats::protect_pages.load(),
              rc = BbStats::protect_revoke_calls.load(), rp = BbStats::protect_revoke_pages.load();
    const u64 minflt = BbStats::gpu_minor_faults.load(), sigf = BbStats::gpu_signal_faults.load();
    static u64 last_copy_cpu, last_copy_sys, last_copy_flt;
    const u64 copy_cpu = BbStats::t_copy_cpu.load(), copy_sys = BbStats::copy_sys_us.load(),
              copy_flt = BbStats::copy_minflt.load();
    static u64 last_proc_flt, last_copy_ns, last_copy_bytes, last_rf, last_trf, last_twf;
    const u64 rf = BbStats::read_faults.load(), trf = BbStats::t_read_faults.load(),
              twf = BbStats::t_write_faults.load();
    if (frame_ms > 40.0 && last_gpu_ns != 0) {
        std::printf("       fault handlers: %llu read faults %.1f ms, write faults %.1f ms\n",
                    static_cast<unsigned long long>(rf - last_rf), (trf - last_trf) / 1e6,
                    (twf - last_twf) / 1e6);
    }
    last_rf = rf;
    last_trf = trf;
    last_twf = twf;
    const FramePhases phases = TakeFramePhases();
    const u64 copy_ns = BbStats::t_copy.load(), copy_bytes = BbStats::copy_bytes.load();
    u64 proc_flt = 0;
    if (rusage usage{}; getrusage(RUSAGE_SELF, &usage) == 0) {
        proc_flt = usage.ru_minflt;
    }
    const u64 t_now[6] = {BbStats::t_resident.load(), BbStats::t_protect.load(),
                          BbStats::t_image_create.load(), BbStats::t_refresh.load(),
                          BbStats::t_staging.load(), BbStats::t_host_wait.load()};
    static u64 last_draws, last_dispatches, last_subs, last_sys, last_user, last_invol, last_vol;
    const u64 draws = BbStats::draws.load(), dispatches = BbStats::dispatches.load(),
              subs = BbStats::submissions.load(), sys_us = BbStats::gpu_sys_us.load(),
              user_us = BbStats::gpu_user_us.load(), invol = BbStats::gpu_invol_switches.load(),
              vol = BbStats::gpu_vol_switches.load();
    u64 gpu_ns = 0;
    if (const int clock = BbStats::gpu_thread_clock.load(); clock != -1) {
        timespec ts{};
        clock_gettime(static_cast<clockid_t>(clock), &ts);
        gpu_ns = u64(ts.tv_sec) * 1000000000ull + u64(ts.tv_nsec);
    } else {
        // bbport (macOS): no CPU clock for another thread; the GPU thread records its own CPU
        // time after each submission (getrusage RUSAGE_THREAD, platform/macos), so use that.
        gpu_ns = (BbStats::gpu_user_us.load() + BbStats::gpu_sys_us.load()) * 1000ull;
    }
    const u64 images = BbStats::images_registered.load();
    const u64 image_bytes = BbStats::image_upload_bytes.load();
    const u64 buffer_bytes = BbStats::buffer_upload_bytes.load();
    if (frame_ms > 40.0 && last_gpu_ns != 0) {
        std::printf("Stall: %.1f ms frame; GPU thread on CPU %.1f ms; %llu images registered, "
                    "%.1f MB image uploads, %.1f MB buffer uploads\n",
                    frame_ms, (gpu_ns - last_gpu_ns) / 1e6,
                    static_cast<unsigned long long>(images - last_images),
                    (image_bytes - last_image_bytes) / 1e6,
                    (buffer_bytes - last_buffer_bytes) / 1e6);
        std::printf("       %llu draws, %llu dispatches, %llu submissions; GPU thread user %.1f ms, "
                    "kernel %.1f ms, %llu preempted, %llu waits\n",
                    static_cast<unsigned long long>(draws - last_draws),
                    static_cast<unsigned long long>(dispatches - last_dispatches),
                    static_cast<unsigned long long>(subs - last_subs), (user_us - last_user) / 1e3,
                    (sys_us - last_sys) / 1e3, static_cast<unsigned long long>(invol - last_invol),
                    static_cast<unsigned long long>(vol - last_vol));
    }
    if (frame_ms > 40.0 && last_gpu_ns != 0) {
        std::printf("       ms in: resident %.1f, protect %.1f, image create %.1f, "
                    "image refresh %.1f, staging %.1f, waiting for host copies %.1f\n",
                    (t_now[0] - last_t[0]) / 1e6, (t_now[1] - last_t[1]) / 1e6,
                    (t_now[2] - last_t[2]) / 1e6, (t_now[3] - last_t[3]) / 1e6,
                    (t_now[4] - last_t[4]) / 1e6, (t_now[5] - last_t[5]) / 1e6);
    }
    if (frame_ms > 40.0 && last_gpu_ns != 0) {
        std::printf("       phases ms: translate %.1f (%llu), pipelines on the GPU thread %.1f (%llu), "
                    "%llu draws without their pipeline (async), descriptors %.1f (%llu batches), "
                    "calls into the game process %.1f (%llu), GPU thread waiting for the game "
                    "%.1f, page faults handled %.1f (%llu; draw pipe drains in them %.1f)",
                    phases.translate_ms, (unsigned long long)phases.translates,
                    phases.pipeline_sync_ms, (unsigned long long)phases.pipelines_sync,
                    (unsigned long long)phases.async_skips, phases.descriptor_ms,
                    (unsigned long long)phases.descriptor_batches, phases.ipc_ms,
                    (unsigned long long)phases.ipc_calls, phases.gpu_idle_ms,
                    phases.fault_handler_ms, (unsigned long long)phases.fault_handlers,
                    phases.fault_drain_ms);
        if (phases.game) {
            std::printf("; game process: faults to bb-gpu %.1f (%llu), calls %.1f (%llu), file "
                        "reads %.1f (%llu, %.1f MB), CPU %.1f, page-ins %llu",
                        phases.game_fault_ms, (unsigned long long)phases.game_faults,
                        phases.game_call_ms, (unsigned long long)phases.game_calls,
                        phases.game_read_ms, (unsigned long long)phases.game_reads,
                        phases.game_read_mb, phases.game_cpu_ms,
                        (unsigned long long)phases.game_majflt);
        }
        std::printf("\n");
    }
    static std::array<u64, 12> last_layer{};
    static u64 last_mprotects = 0, last_entries = 0;
    std::array<u64, 12> layer_now{};
    for (size_t i = 0; i < layer_now.size(); ++i) {
        layer_now[i] = BbStats::t_layer[i].load(std::memory_order_relaxed);
    }
    const u64 mprotects = BbStats::layer_mprotects.load(std::memory_order_relaxed);
    const u64 entries = BbStats::layer_entries_written.load(std::memory_order_relaxed);
    if (frame_ms > 40.0 && last_gpu_ns != 0 && layer_now != last_layer) {
        const auto ms = [&](size_t i) { return (layer_now[i] - last_layer[i]) / 1e6; };
        std::printf("       layer memory ms: traps set/cleared %.1f (%llu mprotect), "
                    "to VRAM %.1f, back in place %.1f, trap hits %.1f, yield %.1f, "
                    "volatile %.1f, republish %.1f (%llu entries: staging %.1f, guest view %.1f, "
                    "mirrors %.1f, copy %.1f), paged uploads %.1f\n",
                    ms(0), static_cast<unsigned long long>(mprotects - last_mprotects), ms(1),
                    ms(2), ms(3), ms(4), ms(5), ms(6),
                    static_cast<unsigned long long>(entries - last_entries), ms(8), ms(9),
                    ms(10), ms(11), ms(7));
    }
    last_layer = layer_now;
    last_mprotects = mprotects;
    last_entries = entries;
    if (frame_ms > 40.0 && last_gpu_ns != 0) {
        std::printf("       GPU thread page faults %llu (process %llu), protection faults %llu; "
                    "protect calls %llu (%llu pages), of which write-revoking %llu (%llu pages)\n",
                    static_cast<unsigned long long>(minflt - last_minflt),
                    static_cast<unsigned long long>(proc_flt - last_proc_flt),
                    static_cast<unsigned long long>(sigf - last_sigf),
                    static_cast<unsigned long long>(pc - last_pc),
                    static_cast<unsigned long long>(pp - last_pp),
                    static_cast<unsigned long long>(rc - last_rc),
                    static_cast<unsigned long long>(rp - last_rp));
    }
    last_pc = pc;
    last_pp = pp;
    last_rc = rc;
    last_rp = rp;
    last_minflt = minflt;
    if (frame_ms > 40.0 && last_gpu_ns != 0 && copy_ns > last_copy_ns) {
        std::printf("       guest copies %.1f MB in %.1f thread-ms, %.1f ms on CPU (kernel %.1f ms, "
                    "%llu page faults in large copies) (%.2f GB/s per thread)\n",
                    (copy_bytes - last_copy_bytes) / 1e6, (copy_ns - last_copy_ns) / 1e6,
                    (copy_cpu - last_copy_cpu) / 1e6, (copy_sys - last_copy_sys) / 1e3,
                    static_cast<unsigned long long>(copy_flt - last_copy_flt),
                    double(copy_bytes - last_copy_bytes) / double(copy_ns - last_copy_ns));
    }
    // bbport: BB_FRAME_LOG=path (with BB_FRAME_STATS): one line per flip, to find what the
    // long frames (stutter) have in common.
    static FILE* const frame_log = OpenFrameLog();
    if (frame_log) {
        static const auto log_start = now;
        static u64 log_twf, log_tick, log_present, log_compiles, log_rf, log_preupload;
        static u64 log_alloc, log_free;
        const u64 tick = BbStats::tick_wait_ns.load(), present = BbStats::present_wait_ns.load();
        const u64 compiles_now = Vulkan::g_bb_compiles.load();
        // tick_wait_ns and the compile count are reset every stats window.
        const FrameProbe probe = TakeFrameProbe(frame_ms, phases.gpu_idle_ms);
        const auto delta = [](u64 now_value, u64& last_value) {
            const u64 d = now_value >= last_value ? now_value - last_value : now_value;
            last_value = now_value;
            return d;
        };
        std::fprintf(frame_log,
                     "%.4f,%.2f,%.2f,%.2f,%.2f,%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%llu,"
                     "%.2f,%llu,%llu,%llu,%llu,%.2f,%.2f,%.1f,%.1f,"
                     "%.2f,%llu,%.2f,%llu,%llu,%.2f,%llu,%.2f,%llu,%.2f,"
                     "%.2f,%llu,%.2f,%llu,%.2f,%.2f,%llu,%.2f,%llu,%.2f,%llu,%.2f,%llu,"
                     "%.2f,%.2f,%.2f,%.2f,%llu,%llu,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                     std::chrono::duration<double>(now - log_start).count(), frame_ms,
                     delta(present, log_present) / 1e6, delta(tick, log_tick) / 1e6,
                     last_gpu_ns ? (gpu_ns - last_gpu_ns) / 1e6 : 0.0,
                     static_cast<unsigned long long>(images - last_images),
                     (image_bytes - last_image_bytes) / 1e6,
                     (buffer_bytes - last_buffer_bytes) / 1e6, (t_now[1] - last_t[1]) / 1e6,
                     (t_now[0] - last_t[0]) / 1e6, (t_now[2] - last_t[2]) / 1e6,
                     (t_now[3] - last_t[3]) / 1e6, delta(twf, log_twf) / 1e6,
                     static_cast<unsigned long long>(delta(rf, log_rf)),
                     (copy_bytes - last_copy_bytes) / 1e6,
                     static_cast<unsigned long long>(draws - last_draws),
                     static_cast<unsigned long long>(dispatches - last_dispatches),
                     static_cast<unsigned long long>(subs - last_subs),
                     static_cast<unsigned long long>(delta(compiles_now, log_compiles)),
                     delta(BbStats::preupload_bytes.load(), log_preupload) / 1e6,
                     last_gpu_ns ? (sys_us - last_sys) / 1e3 : 0.0,
                     delta(BbStats::device_alloc_bytes.load(), log_alloc) / 1e6,
                     delta(BbStats::device_free_bytes.load(), log_free) / 1e6,
                     phases.translate_ms, (unsigned long long)phases.translates,
                     phases.pipeline_sync_ms, (unsigned long long)phases.pipelines_sync,
                     (unsigned long long)phases.async_skips, phases.descriptor_ms,
                     (unsigned long long)phases.descriptor_batches, phases.ipc_ms,
                     (unsigned long long)phases.ipc_calls, phases.gpu_idle_ms,
                     phases.game_fault_ms, (unsigned long long)phases.game_faults,
                     phases.game_call_ms, (unsigned long long)phases.game_calls,
                     phases.game_read_ms, phases.game_read_mb,
                     (unsigned long long)phases.game_reads, phases.game_cpu_ms,
                     (unsigned long long)phases.game_majflt, phases.fault_handler_ms,
                     (unsigned long long)phases.fault_handlers, phases.fault_drain_ms,
                     (unsigned long long)phases.fault_drains, probe.ga_wall_ms, probe.ga_user_ms,
                     probe.ga_sys_ms, probe.ga_drain_ms, (unsigned long long)probe.ga_drains,
                     (unsigned long long)probe.ga_yields, probe.b_busy_ms, probe.b_user_ms,
                     probe.b_sys_ms, probe.cat_ms[BbStats::CatState],
                     probe.cat_ms[BbStats::CatPipeline], probe.cat_ms[BbStats::CatBind],
                     probe.bind_copy_mb, probe.cat_ms[BbStats::CatEmit]);
        static u32 unflushed = 0;
        if (++unflushed >= 120) {
            std::fflush(frame_log);
            unflushed = 0;
        }
#if __has_include("remote/bb_remote.h")
        if (BbRemote::BackActive()) {
            BbRemote::Back::ProtectProbeFlip(frame_ms); // BB_PROTECT_PROBE=1 only
        }
#endif
    }
    last_copy_ns = copy_ns;
    last_copy_cpu = copy_cpu;
    last_copy_sys = copy_sys;
    last_copy_flt = copy_flt;
    last_copy_bytes = copy_bytes;
    last_proc_flt = proc_flt;
    last_sigf = sigf;
    std::copy(std::begin(t_now), std::end(t_now), std::begin(last_t));
    last_draws = draws;
    last_dispatches = dispatches;
    last_subs = subs;
    last_sys = sys_us;
    last_user = user_us;
    last_invol = invol;
    last_vol = vol;
    last_gpu_ns = gpu_ns;
    last_images = images;
    last_image_bytes = image_bytes;
    last_buffer_bytes = buffer_bytes;
    ++frames;
    const double window = std::chrono::duration<double>(now - window_start).count();
    if (window >= 5.0) {
        const u32 compiles = Vulkan::g_bb_compiles.exchange(0);
        const u64 compile_ns = Vulkan::g_bb_compile_ns.exchange(0);
        const u64 compile_port_ns = Vulkan::g_bb_compile_port_ns.exchange(0);
        const u64 compile_driver_ns = Vulkan::g_bb_compile_driver_ns.exchange(0);
        const u64 direct = Vulkan::Scheduler::direct_recordings.exchange(0);
        const u64 faults = BbStats::tracker_faults.exchange(0);
        // GPU command thread CPU time per draw: comparable between builds even when the scene
        // (and so the frame rate) differs a little.
        static u64 window_draws = 0, window_gpu_us = 0;
        const u64 all_draws = BbStats::draws.load();
        const u64 gpu_us = BbStats::gpu_user_us.load() + BbStats::gpu_sys_us.load();
        const double us_per_draw = all_draws > window_draws
                                       ? double(gpu_us - window_gpu_us) / (all_draws - window_draws)
                                       : 0.0;
        const double draws_per_frame = frames ? double(all_draws - window_draws) / frames : 0.0;
        window_draws = all_draws;
        window_gpu_us = gpu_us;
        std::printf("Frame stats: %.1f FPS, worst frame %.1f ms (vblank %u Hz); "
                    "%u shader/pipeline compiles, %.1f ms (%.1f translating, %.1f in the driver); "
                    "%llu recorder syncs; "
                    "%.0f write faults/s, %lld hot pages; GPU thread %.2f us/draw, "
                    "%.0f draws/frame, idle %.1f%%; blocked: recorder %.1f%%, host copies "
                    "%.1f%% (%.0f/frame, %.0f/frame not needed), copy threads %.1f%%, GPU ticks %.1f%%; "
                    "reduced-size draws %.0f/frame of %.0f in the scene\n",
                    frames / window, worst_ms, EmulatorSettings.GetVblankFrequency(), compiles,
                    compile_ns / 1e6, compile_port_ns / 1e6, compile_driver_ns / 1e6,
                    static_cast<unsigned long long>(direct),
                    faults / window, static_cast<long long>(BbStats::hot_pages.load()),
                    us_per_draw, draws_per_frame,
                    BbStats::gpu_idle_ns.exchange(0) / (window * 1e7),
                    BbStats::sync_recording_ns.exchange(0) / (window * 1e7),
                    BbStats::host_copies_wait_ns.exchange(0) / (window * 1e7),
                    frames ? double(BbStats::host_copy_waits.exchange(0)) / frames : 0.0,
                    frames ? double(BbStats::host_copy_waits_skipped.exchange(0)) / frames : 0.0,
                    BbStats::copy_threads_wait_ns.exchange(0) / (window * 1e7),
                    BbStats::tick_wait_ns.exchange(0) / (window * 1e7),
                    frames ? double(BbStats::reduced_draws.exchange(0)) / frames : 0.0,
                    frames ? double(BbStats::scene_draws.exchange(0)) / frames : 0.0);
        BbMem::FaultSites::Report();
        if (const u64 notes = BbStats::cpu_write_notes.exchange(0)) {
            std::printf("Write notices: %.0f/s, %.2f MB/s (libc copies over pages the GPU side "
                        "watches)\n",
                        notes / window, BbStats::cpu_write_note_bytes.exchange(0) / (window * 1e6));
        }
        Vulkan::Breadcrumbs::PrintProgress();
        PrintMemory();
        if (const u64 in_place = BbStats::bound_in_place_bytes.exchange(0)) {
            std::printf("Buffer bindings: %.1f MB/frame in place (%.1f GPU-written), %.1f MB/frame "
                        "VRAM copies\n",
                        in_place / (frames * 1e6),
                        BbStats::bound_in_place_written_bytes.exchange(0) / (frames * 1e6),
                        BbStats::bound_vram_bytes.exchange(0) / (frames * 1e6));
        }
        {
            const u64 updates = BbStats::gpu_data_updates.exchange(0);
            const u64 copied_back = BbStats::copied_back_blocks.exchange(0);
            const u64 readbacks = BbStats::readbacks.exchange(0);
            const u64 late = BbStats::late_vram_writes.exchange(0);
            if (updates + copied_back + readbacks + late != 0) {
                std::printf("GPU data in VRAM: %.1f command writes/s put into it, %.1f exact writes/s "
                            "(labels, CPU bytes beside GPU data), %.1f blocks/s copied back, "
                            "%.1f readbacks/s\n",
                            updates / window, late / window, copied_back / window,
                            readbacks / window);
            }
        }
        BbKernel::ReportEqueueWaits(frames);
        if (static int heap_reports = 0; ++heap_reports % 6 == 1) {
            BbHeapSites::Report();
        }
        runtime_wait_report(frames);
        for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
            if (const u64 a = BbStats::range_allocators[i].load()) {
                std::printf("%s %#llx: %llu live, %.1f MB", i ? ";" : "GPU range allocators:",
                            (unsigned long long)a, (unsigned long long)BbStats::range_live[i].load(),
                            BbStats::range_bytes[i].load() / 1e6);
            }
        }
        if (BbStats::range_allocators[0].load()) {
            std::printf("\n");
        }
        // Native GPU process: the guest's Gnm calls are counted in the game process
        // (MsgGameStats prints them there).
        if (!BbRemote::BackActive()) {
            std::printf("Gnm: %.1f sceGnmSubmitDone/frame, %.1f sceGnmAreSubmitsAllowed/frame, %.1f refused/frame; "
                        "guest waited for the previous frame %.1fx %.2f ms/frame; %.1f compute queue submissions/frame\n",
                        BbStats::submit_done_calls.exchange(0) / double(frames),
                        BbStats::submits_allowed_queries.exchange(0) / double(frames),
                        BbStats::submits_refused.exchange(0) / double(frames),
                        BbStats::gnm_frame_waits.exchange(0) / double(frames),
                        BbStats::gnm_frame_wait_ns.exchange(0) / (1e6 * double(frames)),
                        BbStats::asc_submits.exchange(0) / double(frames));
        } else {
            std::printf("Gnm: %.1f compute queue submissions/frame\n",
                        BbStats::asc_submits.exchange(0) / double(frames));
        }
        std::printf("EOP fences: %llu decoded, %llu labels written, %lld pending; %.1f labels/frame "
                    "written by the GPU\n",
                    (unsigned long long)BbStats::eop_decoded.load(),
                    (unsigned long long)BbStats::eop_written.load(),
                    (long long)(BbStats::eop_decoded.load() - BbStats::eop_written.load()),
                    BbStats::gpu_labels.exchange(0) / double(frames));
        const u64 early = BbStats::idle_flushes.exchange(0);
        const u64 draining = BbStats::drain_flushes.exchange(0);
        if (early || draining) {
            std::printf("Honest labels: %.1f submissions/frame sent early (GPU idle), %.1f as the "
                        "GPU reached the last one\n",
                        early / double(frames), draining / double(frames));
        }
        {
            uint64_t sleeps = 0, sleep_ns = 0;
            runtime_sleep_stats(&sleeps, &sleep_ns);
            if (sleeps != 0 && frames != 0) {
                std::printf("Guest sleeps per frame: %.1f, %.2f ms\n", sleeps / double(frames),
                            sleep_ns / (frames * 1e6));
            }
        }
        if (const u64 copies = BbStats::shadow_copies.exchange(0)) {
            std::printf("Shadow copies: %.0f/frame, %.1f MB/frame (in-place data the CPU writes, "
                        "copied into VRAM for reading)\n",
                        copies / double(frames), BbStats::shadow_bytes.exchange(0) / (frames * 1e6));
        }
        if (const u64 allocs = BbStats::gpu_range_allocs.exchange(0)) {
            std::printf("Guest GPU memory: %.0f ranges/s handed out, %.2f MB/s\n", allocs / window,
                        BbStats::gpu_range_alloc_bytes.exchange(0) / (window * 1e6));
        }
        // bbport: parallel Vulkan recording (vk_scheduler.h): command buffer segments.
        if (const u64 submits = Vulkan::Scheduler::recorded_submissions.exchange(0)) {
            const u64 segments = Vulkan::Scheduler::recorded_segments.exchange(0);
            std::printf("Recording: %.1f submissions/frame, %.2f command buffers each\n",
                        frames ? double(submits) / frames : 0.0, double(segments) / submits);
        }
        BbWaitTrace::Report(window);
        // Frame pacing: spread of the guest flip intervals (judder that the mean hides).
        if (intervals.size() > 2) {
            std::vector<double> sorted = intervals;
            std::sort(sorted.begin(), sorted.end());
            const double median = sorted[sorted.size() / 2];
            double sum = 0, sq = 0;
            u32 spikes = 0;
            for (const double ms : intervals) {
                sum += ms;
                sq += ms * ms;
                spikes += ms > 1.5 * median;
            }
            const double mean = sum / intervals.size();
            std::printf("Frame pacing: median %.2f ms, stddev %.2f ms, p99 %.2f ms, "
                        "%u frames over 1.5x median\n",
                        median, std::sqrt(std::max(0.0, sq / intervals.size() - mean * mean)),
                        sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)], spikes);
        }
#ifdef __APPLE__
        // bbport (macOS): CPU time of every thread, by name and by owner (bbport_thread_cpu_mac).
        if (const std::string threads = BbThreads::ReportProcessThreads(window); !threads.empty()) {
            std::printf("%s%s\n", BbRemote::BackActive() ? "GPU process: " : "", threads.c_str());
        }
#endif
        if (BbRemote::BackActive()) {
            BbRemote::Back::GameStats(window, frames);
        }
        intervals.clear();
        window_start = now;
        frames = 0;
        worst_ms = 0;
    }
}

/// The capture hotkey's marker in the frame log (window.cpp, Ctrl+Shift+C). Written through the
/// log's own stream, so the log's next flush keeps the marker where it is; a "#" line is not a
/// data row, so a reader that expects one line per flip skips it (grep -v '^#').
extern "C" void bb_frame_log_marker(const char* text) {
    if (FILE* log = OpenFrameLog(); log && text) {
        std::fprintf(log, "# CAPTURE %s\n", text);
        std::fflush(log); // the marker is there even if the session dies right after
    }
}

} // namespace BbVideoOut
