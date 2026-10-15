// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (macOS): the Linux functions declared in bbport_macos.h.
#include <cerrno>
#include <cstring>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_act.h>
#include <unistd.h>

#include "platform/macos/bbport_macos.h"

#undef getrusage

extern "C" ssize_t process_vm_readv(pid_t pid, const struct iovec* local,
                                    unsigned long local_count, const struct iovec* remote,
                                    unsigned long remote_count, unsigned long flags) {
    if (pid != getpid() || flags != 0) {
        errno = EPERM;
        return -1;
    }
    // Copies remote into local in order, as far as both reach (Linux's semantics: a partial
    // read stops at the first unreadable byte).
    ssize_t done = 0;
    unsigned long li = 0, ri = 0;
    size_t loff = 0, roff = 0;
    while (li < local_count && ri < remote_count) {
        const size_t lleft = local[li].iov_len - loff, rleft = remote[ri].iov_len - roff;
        if (lleft == 0) {
            ++li, loff = 0;
            continue;
        }
        if (rleft == 0) {
            ++ri, roff = 0;
            continue;
        }
        const uintptr_t from = reinterpret_cast<uintptr_t>(remote[ri].iov_base) + roff;
        // Within one page at a time, so an unmapped page ends the read where it starts.
        const size_t page = size_t(vm_page_size);
        const size_t in_page = page - (from & (page - 1));
        size_t n = lleft < rleft ? lleft : rleft;
        n = n < in_page ? n : in_page;
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), from, n,
                                   reinterpret_cast<uintptr_t>(local[li].iov_base) + loff,
                                   &got) != KERN_SUCCESS ||
            got == 0) {
            if (done == 0) {
                errno = EFAULT;
                return -1;
            }
            return done;
        }
        done += ssize_t(got);
        loff += size_t(got);
        roff += size_t(got);
    }
    return done;
}

extern "C" int bbport_getrusage(int who, struct rusage* usage) {
    if (who != RUSAGE_THREAD) {
        return getrusage(who, usage);
    }
    std::memset(usage, 0, sizeof(*usage));
    const mach_port_t thread = mach_thread_self();
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    const kern_return_t result =
        thread_info(thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count);
    mach_port_deallocate(mach_task_self(), thread);
    if (result != KERN_SUCCESS) {
        errno = EINVAL;
        return -1;
    }
    usage->ru_utime.tv_sec = info.user_time.seconds;
    usage->ru_utime.tv_usec = info.user_time.microseconds;
    usage->ru_stime.tv_sec = info.system_time.seconds;
    usage->ru_stime.tv_usec = info.system_time.microseconds;
    return 0;
}
