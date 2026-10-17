// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace BbThreads {

/// Mach port of the calling thread (macOS; 0 elsewhere): another thread may read its CPU times
/// with thread_info (the frame log, frame_stats.cpp). pthread_mach_thread_np adds no reference.
inline std::uint32_t CurrentThreadPort() {
#ifdef __APPLE__
    return static_cast<std::uint32_t>(pthread_mach_thread_np(pthread_self()));
#else
    return 0;
#endif
}

/// Kernel thread id of the calling thread (diagnostics, write log, draw pipe).
inline unsigned long HostTid() {
#ifdef __APPLE__
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<unsigned long>(id);
#else
    return static_cast<unsigned long>(gettid());
#endif
}

/// Hardware threads available to the process.
inline unsigned Available() {
#ifdef _WIN32
    return bb_win32_available_cpus();
#elif defined(__APPLE__)
    // bbport (macOS): no affinity masks; every core is available.
    return std::max(1u, std::thread::hardware_concurrency());
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
#endif
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
#ifdef _WIN32
    bb_win32_background_thread();
#elif defined(__APPLE__)
    // bbport (macOS): no SCHED_IDLE; the utility QoS class keeps helpers below the game's threads.
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
#endif
}

#ifdef __APPLE__
/// CPU time of the calling thread in microseconds (macOS has no RUSAGE_THREAD).
/// Defined in bbport_thread_cpu_mac.cpp so Mach headers stay out of shared headers.
bool CurrentThreadCpuUs(std::uint64_t& user_us, std::uint64_t& sys_us);
/// The same for another thread, by its Mach port (CurrentThreadPort). False for port 0 or a
/// thread that has exited.
bool ThreadCpuUs(std::uint32_t port, std::uint64_t& user_us, std::uint64_t& sys_us);
/// BB_FRAME_STATS: CPU time of every thread of the process since the last call, by thread name
/// and by owner (this port, the game, the system): where a native GPU process would help.
std::string ReportProcessThreads(double window_s);
#endif

/// Helper threads of the GPU side that the GPU command thread's progress depends on (Vulkan
/// recording threads, guest memory copy threads): a fault there must never wait for that thread
/// (a read trap's copy back goes through it for the game's threads). Set once per thread.
inline thread_local bool gpu_side_helper = false;

} // namespace BbThreads
