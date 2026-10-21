// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (bench/protect-async): BB_PROTECT_ASYNC=1, default off. In the native split bb-gpu's page
// protections stop being one synchronous round trip each to the game process
// (remote_back.cpp ForwardProtect): write-restores are sent with no reply, write-revokes are
// batched and sent at the next flush point, and every place that READS guest memory after a revoke
// (the upload copies, the backing reads, an image refresh, a Vulkan submission whose commands copy
// straight from guest memory) first waits until the game process has applied every overlapping
// revoke decided before it. The hooks stay null with the gate off: every call below is then a
// single null check and the old synchronous path runs unchanged.
#pragma once

#include <cstdint>

namespace BbProtectAsync {

using RangeHook = void (*)(std::uint64_t address, std::uint64_t size);
using Hook = void (*)();

/// Set once by bb-gpu (remote_back.cpp) before the GPU core starts, only with BB_PROTECT_ASYNC=1.
inline RangeHook barrier = nullptr; ///< guest memory [address, address + size) is about to be read
inline Hook barrier_all = nullptr;  ///< a submission: every revoke decided so far must be applied
inline Hook flush = nullptr;        ///< a point where recorded work is handed on: send the batch

inline void Barrier(std::uint64_t address, std::uint64_t size) {
    if (const RangeHook hook = barrier) {
        hook(address, size);
    }
}
inline void BarrierAll() {
    if (const Hook hook = barrier_all) {
        hook();
    }
}
inline void Flush() {
    if (const Hook hook = flush) {
        hook();
    }
}

} // namespace BbProtectAsync
