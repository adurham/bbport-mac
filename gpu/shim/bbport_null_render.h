// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (bench/null-render): BB_NULL_RENDER=1, default off. A measurement gate that asks "how
// fast does the GAME pace when the renderer costs nothing?". With it on, bb-gpu still decodes
// every submission (labels, EOP fences, flips and interrupts reach the guest as before) but
// records no draw and no dispatch, and every flip presents the blank frame instead of encoding
// the game's display buffer. Occlusion counters fall back to the fixed "everything visible"
// sequence, so the guest's visibility logic sees what it sees with the real renderer's counters at
// their most permissive. Not a renderer mode: the screen is black.
//
// With the gate off, Enabled() is one cached false and every caller runs its old path; no line is
// printed (the control run's log carries zero "Null render" lines).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace BbNullRender {

/// 0 off (default), 1 draws + dispatches dropped, 2 draws dropped and dispatches kept (the
/// fallback if dropping the guest's compute work changes what the game does).
inline int Level() {
    static const int level = [] {
        const char* env = std::getenv("BB_NULL_RENDER");
        const int v = env && (env[0] == '1' || env[0] == '2') ? env[0] - '0' : 0;
        if (v) {
            std::printf("Null render: BB_NULL_RENDER=%d - draws%s are decoded and dropped, flips "
                        "present a blank frame (a measurement, not a renderer)\n",
                        v, v == 1 ? " and dispatches" : "");
            std::fflush(stdout);
        }
        return v;
    }();
    return level;
}
inline bool Enabled() {
    return Level() != 0;
}
inline bool DropsDispatches() {
    return Level() == 1;
}

/// Work dropped since the last stats window (frame_stats.cpp prints and resets them).
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, flips{0};

} // namespace BbNullRender
