#pragma once
// The main line has no bb-gpu process: this header is the no-op twin of the native line's
// gpu/shim/remote/bb_remote.h, so files synced from the native line compile verbatim.
// Do not add IPC here; every function is a no-op and both process flags stay false.
#include <cstdint>
using u64 = std::uint64_t; using u32 = std::uint32_t;

namespace BbRemote {

inline bool front_active = false;
inline bool back_active = false;

inline bool FrontActive() { return front_active; }
inline bool BackActive() { return back_active; }
inline bool Requested() { return false; }

namespace Back {
inline void GameStats(double, double) {}
struct GamePhaseTotals {
    u64 fault_ns, faults, call_ns, calls, file_reads, file_bytes, file_ns, cpu_us, majflt;
};
inline bool GamePhases(GamePhaseTotals&) { return false; }
inline void WindowModeKept(int) {}
inline void OutputResKept(std::uint32_t) {}
} // namespace Back

} // namespace BbRemote
