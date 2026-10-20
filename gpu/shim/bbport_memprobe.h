// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): BB_PROBE_CYCLE — a measurement, not a feature.
//
// The owner's own sessions push bb-gpu to 12-16 GiB resident over 30-40 minutes while a hidden
// warm run stays at ~3 GiB, and the difference between them is that he play-tests repeated live
// aspect / preset / upscaler changes from the game's menu. This module churns exactly those three
// settings on a fixed clock so one bounded hidden run can say where the memory goes: a step per
// change (a leak in the re-create path), a flat resident set under a climbing graphics ledger
// (evictable churn, budget-bound), or flat resident with KosmicKrisp's own allocators stepping
// (driver-side retention).
//
// Off unless BB_PROBE_CYCLE is set, so a normal run behaves exactly as before. It writes the
// settings atomics and NEVER BbSettings::Save(): the owner's bbport.ini is never touched, and the
// values it found are restored when the process exits.
//
//   BB_PROBE_CYCLE=<n>        states to churn (0 = off, the default); <n> is the number of cycle
//                             lines after the baseline the run will print, the list repeating
//   BB_PROBE_CYCLE_SECS=<n>   seconds in each state (default 8)

#pragma once
#include <cstdint>

namespace BbMemProbe {

/// Whether BB_PROBE_CYCLE asks for the cycle (read once, cached).
bool Enabled();

/// Called once per frame by the renderer, in the process that reads BbSettings (bb-gpu).
/// `renderer_bytes` is Instance::GetDeviceMemoryUsage(): the renderer's own allocations, the
/// second half of the graphics ledger the budget in vulkan/instance.cpp already computes.
/// Prints one "PROBE CYCLE <i>: ..." line per state and returns.
void Tick(std::uint64_t renderer_bytes);

} // namespace BbMemProbe
