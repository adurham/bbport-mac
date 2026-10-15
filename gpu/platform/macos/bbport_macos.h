// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): the Linux functions the GPU library calls that macOS lacks, force-included in
// every source on macOS (gpu/CMakeLists.txt), as platform/win32 does for Windows. Kept to what is
// a drop-in replacement; anything with different semantics (MAP_FIXED_NOREPLACE, userfaultfd) is
// handled where it is used.
#pragma once
#ifdef __APPLE__

#include <pthread.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The calling thread's system-wide ID (Linux's gettid; pthread_threadid_np here).
static inline pid_t gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (pid_t)id;
}

/// Reads from this process only (the library's sole use): fails instead of faulting on unmapped
/// memory, through Mach (platform/macos/posix_macos.cpp).
ssize_t process_vm_readv(pid_t pid, const struct iovec* local, unsigned long local_count,
                         const struct iovec* remote, unsigned long remote_count,
                         unsigned long flags);

/// getrusage with Linux's RUSAGE_THREAD (the calling thread's CPU times and context switches,
/// from Mach); RUSAGE_SELF and RUSAGE_CHILDREN go to the system's.
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1
int bbport_getrusage(int who, struct rusage* usage);
#define getrusage bbport_getrusage
#endif

#ifdef __cplusplus
}
#endif

#endif // __APPLE__
