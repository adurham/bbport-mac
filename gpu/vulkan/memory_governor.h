// SPDX-License-Identifier: GPL-2.0-or-later
// bbport vulkan: what the GPU memory budget governor (instance.cpp, GovernedBudget) and the texture
// collector (texture_cache_memory.cpp) tell each other.
//
// The governor is on only when the owner asks for it: renderer_share_cap (bbport.ini, MiB, 0 = off;
// BB_RENDERER_SHARE_CAP is the env alias) or BB_MEM_BUDGET_RESERVE=1. With both off nothing here is
// read on any hot path except one cached bool, and the budget is the port's own, unchanged.
//
// The texture cache publishes, once a second, the bytes of the images it holds and of the images
// referenced in the last three frames (lru_touched_tick at or after the gc_tick of the start of the
// frame before last). The governor floors its budget so the collector's critical mark sits above
// those bytes plus the renderer's non-image usage: an image in use cannot be the next thing the
// collector needs to free. The collector also refuses outright to evict an image referenced in the
// last two frames while the governor is on, and counts both what it refused and what it evicted
// anyway (the second must read 0 in every arm).
#pragma once

#include <atomic>

#include "common/types.h"

namespace Vulkan::MemoryGovernor {

/// Whether a budget gate is on (renderer_share_cap > 0 or BB_MEM_BUDGET_RESERVE=1). Defined in
/// instance.cpp; cached after the first call (the settings are loaded before the device exists).
bool On();

/// Published by the texture cache at most once a second (host image sizes, VkImage allocations).
inline std::atomic<u64> image_bytes{0};            ///< every registered image
inline std::atomic<u64> referenced_image_bytes{0}; ///< those referenced in the last 3 frames
inline std::atomic<u64> referenced_images{0};
inline std::atomic<bool> referenced_published{false};

/// Counted by the texture collector (all runs; printed when On()).
inline std::atomic<u64> evicted_referenced{0}; ///< evicted while used in the last 2 frames: must be 0
inline std::atomic<u64> refused_referenced{0}; ///< evictions the guard refused for that reason
inline std::atomic<u64> evicted_total{0};

} // namespace Vulkan::MemoryGovernor
