// SPDX-License-Identifier: GPL-2.0-or-later
// bbport vulkan: is the engine mid-play, for gates that must not do expensive work there.
//
// Why it exists: BB_KK_TRIM_POOL on a fixed clock (docs/KK_ALLOCATOR_TRIM_2026-10-20.md) buys
// ~1.7 GiB of phys_footprint but puts a 73-88 ms frame in most 5 s windows, because destroying a
// batch of command buffers and letting the pool re-create them lands in the middle of play. The
// same bytes are free at a moment when the engine is not playing a frame: the title, a menu, or a
// loading transition — and a load is exactly when this pool balloons (the CPU records hundreds of
// frames while the GPU is behind), so it is also when the most is reclaimable.
//
// The signal is the port's own frame statistics, not a new one (OnFlipStats already has the frame
// time and the draw count): a frame is *play* when it was not a long frame and it drew a gameplay
// number of draws. Both thresholds are ones the port already uses — 40 ms is the frame stats' own
// "long frame" line, 200 draws is the warm route's gameplay threshold (the title draws ~24).
//
// Only published when a gate asks for it (Enabled()), so with every gate off the hot path is one
// cached env read per flip and nothing else: no default behaviour changes.
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "common/types.h"

namespace Vulkan::PlayState {

namespace detail {
/// A frame at or over this many ms is a stall, not play (the frame stats' own long-frame line).
inline constexpr double LongFrameMs = 40.0;
/// At or above this many draws a frame the engine is drawing gameplay (the title draws ~24).
inline constexpr u64 PlayDraws = 200;
inline constexpr u64 Unpublished = ~u64{0};

inline std::atomic<bool> gate_read{false};
inline std::atomic<bool> gate_on{false};
/// The last flip's frame time in microseconds (Unpublished until the first flip is reported).
inline std::atomic<u64> last_frame_us{Unpublished};
/// What that flip drew.
inline std::atomic<u64> last_draws{0};
} // namespace detail

/// Whether anything wants the play state (BB_KK_TRIM_LOADONLY=1 today).
inline bool Enabled() {
    if (!detail::gate_read.load(std::memory_order_relaxed)) {
        const char* value = std::getenv("BB_KK_TRIM_LOADONLY");
        const bool on = value != nullptr && *value != '\0' && !(value[0] == '0' && value[1] == '\0');
        detail::gate_on.store(on, std::memory_order_relaxed);
        detail::gate_read.store(true, std::memory_order_relaxed);
        if (on) {
            std::printf("PROBE PLAY: BB_KK_TRIM_LOADONLY on, trims wait for a frame that is not "
                        "play (>= %g ms or < %llu draws, or the title)\n",
                        detail::LongFrameMs, (unsigned long long)detail::PlayDraws);
            std::fflush(stdout);
        }
        return on;
    }
    return detail::gate_on.load(std::memory_order_relaxed);
}

/// Called once per reported flip by the frame statistics. Cheap no-op unless a gate is on.
inline void PublishFrame(double frame_ms, u64 draws_this_frame) {
    if (!Enabled()) {
        return;
    }
    const u64 us = frame_ms > 0.0 ? u64(frame_ms * 1000.0) : 0;
    detail::last_frame_us.store(us, std::memory_order_relaxed);
    detail::last_draws.store(draws_this_frame, std::memory_order_relaxed);
}

/// The last reported frame time in ms, or LongFrameMs when nothing has been reported yet (before
/// the first flip: the pool's growth during startup is not play, and nothing is drawn yet). With
/// the frame statistics off (BB_FRAME_STATS unset) nothing is ever published, so MidPlay() is
/// always false and a load-only trim degrades to the plain clock — which is why the arms run with
/// BB_FRAME_STATS=1.
inline double LastFrameMs() {
    const u64 us = detail::last_frame_us.load(std::memory_order_relaxed);
    return us == detail::Unpublished ? detail::LongFrameMs : double(us) / 1000.0;
}

inline u64 LastDraws() {
    return detail::last_draws.load(std::memory_order_relaxed);
}

/// True while the engine is playing: the last frame was short and drew gameplay. False at the
/// title, in menus, during a load or a shader-compile burst — the moments a trim can be paid for
/// without turning a good frame into a hitch.
inline bool MidPlay() {
    return LastFrameMs() < detail::LongFrameMs && LastDraws() >= detail::PlayDraws;
}

} // namespace Vulkan::PlayState
