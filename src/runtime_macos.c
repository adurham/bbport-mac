/* bbport (macOS): Linux functions the runtime calls that macOS lacks and that need Mach, kept
 * out of platform.h (Mach's names, such as thread_create, clash with the runtime's). */
#ifdef __APPLE__
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

ssize_t process_vm_readv(pid_t pid, const struct iovec *local, unsigned long local_count,
                         const struct iovec *remote, unsigned long remote_count, unsigned long flags) {
    if (pid != getpid() || flags || local_count != 1 || remote_count != 1) { errno = EINVAL; return -1; }
    const size_t n = local[0].iov_len < remote[0].iov_len ? local[0].iov_len : remote[0].iov_len;
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)remote[0].iov_base, n,
                               (mach_vm_address_t)(uintptr_t)local[0].iov_base, &got) != KERN_SUCCESS) {
        errno = EFAULT;
        return -1;
    }
    return (ssize_t)got;
}
#endif
