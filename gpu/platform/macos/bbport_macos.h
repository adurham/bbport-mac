// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): the Linux functions the GPU library calls that macOS lacks, force-included in
// every source on macOS (gpu/CMakeLists.txt), as platform/win32 does for Windows. Kept to drop-in
// replacements; anything without one (userfaultfd, tgkill) is handled where it is used.
#pragma once
#ifdef __APPLE__

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
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

/// Linux's MAP_FIXED_NOREPLACE: the address is taken as a hint and a mapping placed elsewhere is
/// undone (EEXIST), as Linux fails when the range is in use. mmap is bbport_mmap from here on.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x40000000
static inline void* bbport_mmap(void* address, size_t size, int prot, int flags, int fd,
                                off_t offset) {
    if (!(flags & MAP_FIXED_NOREPLACE)) {
        return mmap(address, size, prot, flags, fd, offset);
    }
    void* p = mmap(address, size, prot, flags & ~MAP_FIXED_NOREPLACE, fd, offset);
    if (p != MAP_FAILED && p != address) {
        munmap(p, size);
        errno = EEXIST;
        return MAP_FAILED;
    }
    return p;
}
#define mmap bbport_mmap
#endif

#ifdef __cplusplus
}
#endif

#endif // __APPLE__
