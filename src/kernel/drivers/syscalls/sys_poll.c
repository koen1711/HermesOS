#include "sys.h"

#include "fd.h"
#include "uaccess.h"
#include <drivers/fs/vfs/vfs.h>
#include <drivers/net/core/netisr.h>
#include <drivers/scheduler/process.h>
#include <drivers/timer/callout.h>
#include <drivers/timer/clock.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

#define POLL_MAX_FDS 1024
#define POLL_RECHECK_MS 10
#define POLLRDNORM 0x040
#define POLLWRNORM 0x100

struct pollfd_k {
    int fd;
    int16_t events;
    int16_t revents;
};

struct pselect_sigmask {
    const uint64_t* ss;
    size_t ss_len;
};

struct pollfd_set {
    struct pollfd_k* fds;
    uint64_t nfds;
};

int poll_file(struct file_s* f)
{
    int ready = f->node->fops && f->node->fops->poll ? f->node->fops->poll(f) : VFS_POLLIN | VFS_POLLOUT;
    if (ready & VFS_POLLIN)
        ready |= POLLRDNORM;
    if (ready & VFS_POLLOUT)
        ready |= POLLWRNORM;
    return ready;
}

static int16_t poll_one(const struct pollfd_k* p)
{
    if (p->fd < 0)
        return 0;
    struct file_s* f = fd_get(p->fd);
    if (f == nullptr)
        return VFS_POLLNVAL;
    return (int16_t)(poll_file(f) & (p->events | VFS_POLLERR | VFS_POLLHUP));
}

static long scan_pollfds(void* ctx)
{
    const struct pollfd_set* set = ctx;
    long count = 0;
    for (uint64_t i = 0; i < set->nfds; i++) {
        set->fds[i].revents = poll_one(&set->fds[i]);
        if (set->fds[i].revents)
            count++;
    }
    return count;
}

long poll_wait(const poll_scan_t scan, void* ctx, const int64_t timeout_ns)
{
    const uint64_t start = clock_monotonic_ns();
    for (;;) {
        netisr_poll();
        const long count = scan(ctx);
        if (count || timeout_ns == 0)
            return count;

        uint64_t wait = ticks_from_ms(POLL_RECHECK_MS);
        if (timeout_ns > 0) {
            const uint64_t elapsed = clock_monotonic_ns() - start;
            if (elapsed >= (uint64_t)timeout_ns)
                return 0;
            const uint64_t left = clock_ticks_from_ns((uint64_t)timeout_ns - elapsed);
            if (left < wait)
                wait = left;
        }

        const int err = process_sleep_timeout(WAIT_CHANNEL_POLL, wait);
        if (err == -EINTR)
            return -EINTR;
    }
}

long poll_wait_sigmask(const uint64_t* umask, const size_t sigsetsize, const poll_scan_t scan, void* ctx,
                       const int64_t timeout_ns)
{
    process_t* current = scheduler_get()->current;
    uint64_t mask = 0;
    if (umask) {
        if (sigsetsize != sizeof(uint64_t))
            return -EINVAL;
        if (copy_from_user(&mask, umask, sizeof(mask)) < 0)
            return -EFAULT;
    }
    const uint64_t saved = current->sig_blocked;
    if (umask)
        current->sig_blocked = mask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

    const long r = poll_wait(scan, ctx, timeout_ns);
    if (umask) {
        if (r == -EINTR) {
            current->sig_saved_blocked = saved;
            current->sig_restore_mask = true;
        } else {
            current->sig_blocked = saved;
        }
    }
    return r;
}

static long with_sigmask(const uint64_t* umask, const size_t sigsetsize, struct pollfd_k* fds, const uint64_t nfds,
                         const int64_t timeout_ns)
{
    struct pollfd_set set = { fds, nfds };
    return poll_wait_sigmask(umask, sigsetsize, scan_pollfds, &set, timeout_ns);
}

long poll_timespec_ns(const struct timespec_k* uts, int64_t* ns)
{
    if (uts == nullptr) {
        *ns = -1;
        return 0;
    }
    struct timespec_k ts;
    if (copy_from_user(&ts, uts, sizeof(ts)) < 0)
        return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
        return -EINVAL;
    *ns = ts.tv_sec > 1000000000 ? 1000000000LL * 1000000000LL : ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return 0;
}

static long poll_user(void* ufds, const uint64_t nfds, const int64_t timeout_ns, const uint64_t* sigmask,
                      const size_t sigsetsize)
{
    if (nfds > POLL_MAX_FDS)
        return -EINVAL;
    struct pollfd_k* fds = nullptr;
    if (nfds) {
        fds = malloc(nfds * sizeof(struct pollfd_k));
        if (fds == nullptr)
            return -ENOMEM;
        if (copy_from_user(fds, ufds, nfds * sizeof(struct pollfd_k)) < 0) {
            free(fds);
            return -EFAULT;
        }
    }
    long r = with_sigmask(sigmask, sigsetsize, fds, nfds, timeout_ns);
    if (r >= 0 && nfds && copy_to_user(ufds, fds, nfds * sizeof(struct pollfd_k)) < 0)
        r = -EFAULT;
    free(fds);
    return r;
}

long sys_poll(void* fds, const uint64_t nfds, const int timeout_ms)
{
    return poll_user(fds, nfds, timeout_ms < 0 ? -1 : (int64_t)timeout_ms * 1000000, nullptr, 0);
}

long sys_ppoll(void* fds, const uint64_t nfds, const struct timespec_k* timeout, const uint64_t* sigmask,
               const size_t sigsetsize)
{
    int64_t ns;
    const long err = poll_timespec_ns(timeout, &ns);
    return err ? err : poll_user(fds, nfds, ns, sigmask, sigsetsize);
}

static bool fd_isset(const uint64_t* set, const int fd)
{
    return set && (set[fd / 64] >> (fd % 64)) & 1;
}

static long select_core(const int nfds, uint64_t* urfds, uint64_t* uwfds, uint64_t* uefds, const int64_t timeout_ns,
                        const uint64_t* sigmask, const size_t sigsetsize)
{
    if (nfds < 0 || nfds > POLL_MAX_FDS)
        return -EINVAL;
    const size_t words = ((size_t)nfds + 63) / 64;
    uint64_t sets[3][POLL_MAX_FDS / 64] = {{0}};
    uint64_t* user[3] = { urfds, uwfds, uefds };
    for (int s = 0; s < 3; s++) {
        if (user[s] && copy_from_user(sets[s], user[s], words * sizeof(uint64_t)) < 0)
            return -EFAULT;
    }

    struct pollfd_k* fds = malloc((nfds ? nfds : 1) * sizeof(struct pollfd_k));
    if (fds == nullptr)
        return -ENOMEM;
    uint64_t n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        int16_t events = 0;
        if (fd_isset(user[0] ? sets[0] : nullptr, fd))
            events |= VFS_POLLIN;
        if (fd_isset(user[1] ? sets[1] : nullptr, fd))
            events |= VFS_POLLOUT;
        if (fd_isset(user[2] ? sets[2] : nullptr, fd))
            events |= VFS_POLLPRI;
        if (events) {
            if (fd_get(fd) == nullptr) {
                free(fds);
                return -EBADF;
            }
            fds[n++] = (struct pollfd_k){ .fd = fd, .events = events };
        }
    }

    long r = with_sigmask(sigmask, sigsetsize, fds, n, timeout_ns);
    if (r >= 0) {
        uint64_t out[3][POLL_MAX_FDS / 64] = {{0}};
        long count = 0;
        for (uint64_t i = 0; i < n; i++) {
            const int fd = fds[i].fd;
            const int16_t rev = fds[i].revents;
            const int16_t ev = fds[i].events;
            const bool hit[3] = {
                (ev & VFS_POLLIN) && (rev & (VFS_POLLIN | VFS_POLLHUP | VFS_POLLERR)),
                (ev & VFS_POLLOUT) && (rev & (VFS_POLLOUT | VFS_POLLERR)),
                (ev & VFS_POLLPRI) && (rev & VFS_POLLPRI)
            };
            for (int s = 0; s < 3; s++) {
                if (hit[s]) {
                    out[s][fd / 64] |= 1ULL << (fd % 64);
                    count++;
                }
            }
        }
        for (int s = 0; s < 3 && r >= 0; s++) {
            if (user[s] && copy_to_user(user[s], out[s], words * sizeof(uint64_t)) < 0)
                r = -EFAULT;
        }
        if (r >= 0)
            r = count;
    }
    free(fds);
    return r;
}

long sys_select(const int nfds, uint64_t* rfds, uint64_t* wfds, uint64_t* efds, struct timeval_k* utimeout)
{
    int64_t ns = -1;
    if (utimeout) {
        struct timeval_k tv;
        if (copy_from_user(&tv, utimeout, sizeof(tv)) < 0)
            return -EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0)
            return -EINVAL;
        ns = tv.tv_sec > 1000000000 ? 1000000000LL * 1000000000LL : tv.tv_sec * 1000000000LL + tv.tv_usec * 1000;
    }
    return select_core(nfds, rfds, wfds, efds, ns, nullptr, 0);
}

long sys_pselect6(const int nfds, uint64_t* rfds, uint64_t* wfds, uint64_t* efds, const struct timespec_k* timeout,
                  const void* usigmask)
{
    int64_t ns;
    const long err = poll_timespec_ns(timeout, &ns);
    if (err)
        return err;
    struct pselect_sigmask sm = { nullptr, 0 };
    if (usigmask && copy_from_user(&sm, usigmask, sizeof(sm)) < 0)
        return -EFAULT;
    return select_core(nfds, rfds, wfds, efds, ns, sm.ss, sm.ss_len);
}
