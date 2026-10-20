// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): BB_PROBE_CYCLE — the live-setting churn probe (see bbport_memprobe.h).

#include "bbport_memprobe.h"

#include "bbport_settings.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/task_info.h>
#endif

namespace BbMemProbe {
namespace {

/// One state of the churn: the three settings the owner's menu play-test changes live.
struct State {
    int aspect;
    int preset;
    int upscaler;
};

// One dimension at a time, in this order and repeating: his current settings first (bbport.ini
// aspect_ratio=21:9, preset=4 UltraPerformance, upscaler=fsr3), then the aspect ratio, then the
// preset, then the upscaler. Every lap comes back to his settings — that line is the control:
// nothing is re-created on it, so growth seen there is time (or the area), not a change.
constexpr State States[] = {
    {BbSettings::Aspect21x9, BbSettings::UltraPerformance, BbSettings::UpscalerFsr3},
    {BbSettings::Aspect16x9, BbSettings::UltraPerformance, BbSettings::UpscalerFsr3},
    {BbSettings::Aspect21x9, BbSettings::Quality, BbSettings::UpscalerFsr3},
    {BbSettings::Aspect21x9, BbSettings::UltraPerformance, BbSettings::UpscalerOff},
};
constexpr int StateCount = int(sizeof(States) / sizeof(States[0]));

// The run is at the title for ~45 s (warm_route.sh's TITLE_WAIT) and in the area's load for ~30
// more, so a fixed 90 s from the renderer's first frame is past both, with the title presses'
// retries as margin. Deliberately not a gameplay detector: the probe must not depend on what the
// game is doing, only on the clock.
constexpr double BootSeconds = 90.0;

// Read once, on the render thread (rasterizer.cpp's display pass calls OnFrameStart once a frame).
bool gate_read = false;
bool on = false;
double period = 8.0;

// The values found when the cycle began; put back when the process exits.
struct Original {
    int aspect = 0;
    std::uint32_t output_res = 0;
    int preset = 0;
    int upscaler = 0;
    bool taken = false;
} original;

/// The probe writes settings; it never saves them. BbSettings::Save() (bbport.ini) is not called
/// from here at all — that file is the owner's, and a measurement must not edit it.
void Restore() {
    if (!original.taken) return;
    auto& v = BbSettings::Get();
    v.aspect.store(original.aspect);
    v.output_res.store(original.output_res);
    v.preset.store(original.preset);
    v.upscaler.store(original.upscaler);
}

/// The edit the menu makes, at the same level: the aspect ratio drives the output resolution and
/// keeps its height (BbSettings::SetAspect — the F1 overlay's Aspect row and the game's Display
/// page both call it), and preset/upscaler go to the atomics the renderer polls. Storing the
/// aspect number alone would test an aspect/output pair the menu can never produce.
void Apply(const State& s) {
    auto& v = BbSettings::Get();
    BbSettings::SetAspect(s.aspect);
    // Not the overlay's Store() helper: that marks the settings dirty and the menu then saves
    // them. The renderer's own BB_PRESET_FILE scripted-change path stores these two directly.
    v.preset.store(s.preset);
    v.upscaler.store(s.upscaler);
}

/// The two numbers vulkan/instance.cpp's UnifiedMemoryBudget() samples, read the same way (one
/// TASK_VM_INFO, the same ledger sum). Its formula and its 250 ms cache are left untouched: this
/// only reads, so the budget behaves exactly as it does in the owner's sessions.
struct Memory {
    std::uint64_t footprint = 0; ///< phys_footprint — what jetsam and the watchdog look at
    std::uint64_t ledger = 0;    ///< the graphics ledger: the renderer's share + the driver's own
};

Memory Sample() {
    Memory m;
#ifdef __APPLE__
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
            KERN_SUCCESS ||
        count < TASK_VM_INFO_REV3_COUNT) {
        return m;
    }
    m.footprint = info.phys_footprint;
    const std::int64_t graphics =
        std::int64_t(info.ledger_tag_graphics_footprint) +
        std::int64_t(info.ledger_tag_graphics_footprint_compressed);
    m.ledger = graphics > 0 ? std::uint64_t(graphics) : 0;
#endif
    return m;
}

/// The one line per cycle, in the shape the sampler greps. flushed: through run.sh's tee the log is
/// block-buffered, and a line still in the buffer is lost when the watchdog's TERM ends the run —
/// which is exactly the end this probe exists for.
void PrintLine(int index, const State& s, std::uint64_t renderer_bytes) {
    const Memory m = Sample();
    constexpr std::uint64_t MiB = 1u << 20;
    std::printf("PROBE CYCLE %d: aspect %s preset %d upscaler %s footprint %llu MiB ledger %llu MiB "
                "renderer %llu MiB\n",
                index, BbSettings::AspectKey(s.aspect), s.preset,
                BbSettings::UpscalerName(s.upscaler), (unsigned long long)(m.footprint / MiB),
                (unsigned long long)(m.ledger / MiB), (unsigned long long)(renderer_bytes / MiB));
    std::fflush(stdout);
}

} // namespace

bool Enabled() {
    if (!gate_read) {
        const char* gate = std::getenv("BB_PROBE_CYCLE");
        // Nonzero turns it on; the number is not a count — the list repeats for the whole run.
        on = gate && std::atoi(gate) > 0;
        if (const char* secs = std::getenv("BB_PROBE_CYCLE_SECS")) {
            const double value = std::atof(secs);
            if (value >= 1.0) period = value;
        }
        gate_read = true;
        if (on) std::printf("PROBE: BB_PROBE_CYCLE on, %g s per state\n", period);
        if (on) std::fflush(stdout);
    }
    return on;
}

void Tick(std::uint64_t renderer_bytes) {
    if (!Enabled()) return;
    using Clock = std::chrono::steady_clock;
    // Anchored at the renderer's first frame, which is within a second of the process start; the
    // 90 s boot offset carries far more margin than that.
    static const Clock::time_point start = Clock::now();
    static bool began = false;
    static int cycle = 0; ///< the i of the next line; the state printed is States[i % StateCount]
    static Clock::time_point next{};

    if (!began) {
        if (std::chrono::duration<double>(Clock::now() - start).count() < BootSeconds) return;
        auto& v = BbSettings::Get();
        original.aspect = v.aspect.load();
        original.output_res = v.output_res.load();
        original.preset = v.preset.load();
        original.upscaler = v.upscaler.load();
        original.taken = true;
        std::atexit(Restore);
        began = true;
        // The baseline (i = 0): the settings this run loaded, in effect since the boot. Taken
        // before the first write, so the boot phase's own allocations land on this line rather
        // than on top of a change.
        PrintLine(0, States[0], renderer_bytes);
        cycle = 1;
        Apply(States[cycle % StateCount]);
        next = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                  std::chrono::duration<double>(period));
        return;
    }
    if (Clock::now() < next) return;
    // The state stored one period ago has been in effect since: these are its settled numbers,
    // not the pre-re-create snapshot a line printed at the instant of the store would give.
    PrintLine(cycle, States[cycle % StateCount], renderer_bytes);
    ++cycle;
    Apply(States[cycle % StateCount]);
    next = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double>(period));
}

} // namespace BbMemProbe
