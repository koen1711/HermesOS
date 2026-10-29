#ifndef OS_SYSCALLS_SYS_H
#define OS_SYSCALLS_SYS_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/types.h>

struct timespec_k;
struct timeval_k;

long sys_read(int fd, void* buf, size_t len);
long sys_write(int fd, const void* buf, size_t len);
long sys_openat(int dirfd, const char* path, int flags, uint32_t mode);
long sys_close(int fd);

long sys_readv(int fd, const void* iov, int iov_cnt);
long sys_writev(int fd, const void* iov, int iov_cnt);
long sys_preadv(int fd, const void* iov, int iov_cnt, int64_t offset);
long sys_pwritev(int fd, const void* iov, int iov_cnt, int64_t offset);
long sys_preadv2(int fd, const void* iov, int iov_cnt, int64_t offset, int flags);
long sys_pwritev2(int fd, const void* iov, int iov_cnt, int64_t offset, int flags);

long sys_pread64(int fd, void* buf, size_t len, int64_t offset);
long sys_pwrite64(int fd, const void* buf, size_t len, int64_t offset);
long sys_lseek(int fd, int64_t offset, int whence);
long sys_fstat(int fd, void* st);
long sys_newfstatat(int dirfd, const char* path, void* st, int flags);
long sys_statfs(const char* path, void* buf);
long sys_fstatfs(int fd, void* buf);
long sys_getdents64(int fd, void* dirp, size_t count);
long sys_getcwd(char* buf, size_t size);
long sys_chdir(const char* path);
long sys_fchdir(int fd);
long sys_dup(int fd);
long sys_dup3(int oldfd, int newfd, int flags);
long sys_dup2(int oldfd, int newfd);
long sys_fcntl(int fd, int cmd, uint64_t arg);
long sys_ioctl(int fd, unsigned long request, void* arg);
long sys_faccessat(int dirfd, const char* path, int mode, int flags);
long sys_readlinkat(int dirfd, const char* path, char* buf, size_t size);
long sys_mkdirat(int dirfd, const char* path, uint32_t mode);
long sys_unlinkat(int dirfd, const char* path, int flags);
long sys_renameat(int olddirfd, const char* old_path, int newdirfd, const char* new_path, unsigned int flags);
long sys_truncate(const char* path, int64_t length);
long sys_ftruncate(int fd, int64_t length);
long sys_path_exists(int dirfd, const char* path);
long sys_umask(uint32_t mask);
long sys_fsync(int fd);
long sys_pipe2(int* fds, int flags);
long sys_sendfile(int out_fd, int in_fd, int64_t* offset, size_t count);
long sys_memfd_create(const char* name, unsigned int flags);
long sys_chroot(const char* path);

long sys_mknodat(int dirfd, const char* path, uint32_t mode, uint32_t dev);
long sys_fallocate(int fd, int mode, int64_t offset, int64_t len);

long sys_splice(int fd_in, int64_t* off_in, int fd_out, int64_t* off_out, size_t len, unsigned int flags);
long sys_copy_file_range(int fd_in, int64_t* off_in, int fd_out, int64_t* off_out, size_t len, uint32_t flags);

long sys_close_range(unsigned int min_fd, unsigned int max_fd, unsigned int flags);

long sys_poll(void* fds, uint64_t nfds, int timeout_ms);
long sys_ppoll(void* fds, uint64_t nfds, const struct timespec_k* timeout, const uint64_t* sigmask, size_t sigsetsize);
long sys_select(int nfds, uint64_t* rfds, uint64_t* wfds, uint64_t* efds, struct timeval_k* timeout);
long sys_pselect6(int nfds, uint64_t* rfds, uint64_t* wfds, uint64_t* efds, const struct timespec_k* timeout,
                  const void* sigmask);

struct file_s;
typedef long (*poll_scan_t)(void* ctx);
int poll_file(struct file_s* f);
long poll_wait(poll_scan_t scan, void* ctx, int64_t timeout_ns);
long poll_wait_sigmask(const uint64_t* umask, size_t sigsetsize, poll_scan_t scan, void* ctx, int64_t timeout_ns);
long poll_timespec_ns(const struct timespec_k* uts, int64_t* ns);

long sys_epoll_create1(int flags);
long sys_epoll_ctl(int epfd, int op, int fd, const void* event);
long sys_epoll_pwait(int epfd, void* events, int maxevents, int64_t timeout_ns, const uint64_t* sigmask,
                     size_t sigsetsize);
long sys_epoll_pwait2(int epfd, void* events, int maxevents, const struct timespec_k* timeout,
                      const uint64_t* sigmask, size_t sigsetsize);
long sys_eventfd2(unsigned int initval, int flags);
long sys_pidfd_open(int pid, unsigned int flags);
int pidfd_pid(const struct file_s* f);

long sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, int64_t offset);
long sys_msync(uint64_t addr, size_t len, int flags);
long sys_munmap(uint64_t addr, size_t len);
long sys_mprotect(uint64_t addr, size_t len, int prot);
long sys_brk(uint64_t addr);
long sys_madvise(uint64_t addr, size_t len, int advice);
long sys_mremap(uint64_t old_addr, size_t old_len, size_t new_len, int flags,
    uint64_t new_addr);

long sys_exit(int code);
long sys_exit_group(int code);
long sys_wait4(int pid, int* status, int options, void* rusage);
long sys_execve(const char* path, char* const* argv, char* const* envp);
long sys_set_tid_address(int* tidptr);
long sys_arch_prctl(int code, uint64_t addr);
long sys_prctl(int option, uint64_t arg2);
long sys_uname(void* buf);
long sys_sysinfo(void* info);
long sys_prlimit64(int pid, int resource, const void* new_limit, void* old_limit);
long sys_getrusage(int who, void* usage);
long sys_sched_getaffinity(int pid, size_t len, void* mask);
long sys_sched_setaffinity(int pid, size_t len, const void* mask);
long sys_sched_setparam(int pid, const void* param);
long sys_sched_setscheduler(int pid, int policy, const void* param);
long sys_sched_rr_get_interval(int pid, struct timespec_k* ts);
long sys_getrandom(void* buf, size_t len, unsigned int flags);
long sys_getgroups(int size, uint32_t* list);
long sys_setgroups(int size, const uint32_t* list);
long sys_waitid(int idtype, int id, void* info, int options, void* rusage);

long sys_clock_gettime(int clock, struct timespec_k* ts);
long sys_clock_getres(int clock, struct timespec_k* ts);
long sys_clock_settime(int clock, const struct timespec_k* ts);
long sys_gettimeofday(struct timeval_k* tv, void* tz);
long sys_time(int64_t* t);
long sys_nanosleep(const struct timespec_k* req, struct timespec_k* rem);
long sys_clock_nanosleep(int clock, int flags, const struct timespec_k* req, struct timespec_k* rem);
long sys_alarm(unsigned int seconds);
long sys_setitimer(int which, const void* new_value, void* old_value);
long sys_getitimer(int which, void* value);
long sys_times(void* buf);

#endif //OS_SYSCALLS_SYS_H
