#include "sys.h"

#include "fd.h"
#include "uaccess.h"
#include <drivers/fs/vfs/vfs.h>
#include <drivers/scheduler/process.h>
#include <drivers/timer/clock.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/pmm.h>
#include <hardware/smp/smp.h>
#include <utils/random/random.h>
#include <utils/str/str.h>
#include <os/errno.h>

#define EXEC_MAX_ARGS 4096
#define EXEC_STRINGS_MAX (128 * 1024)
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define RLIM_INFINITY (~0ULL)
#define GETRANDOM_MAX (32 * 1024 * 1024)
#define SCHED_RESET_ON_FORK 0x40000000
#define WNOHANG 1
#define WSTOPPED 2
#define WEXITED 4
#define WCONTINUED 8
#define WNOWAIT 0x01000000

enum sched_policy {
    SCHED_OTHER = 0,
    SCHED_FIFO = 1,
    SCHED_RR = 2,
    SCHED_BATCH = 3,
    SCHED_IDLE = 5
};

enum waitid_type {
    P_ALL = 0,
    P_PID = 1,
    P_PGID = 2,
    P_PIDFD = 3
};

enum cld_code {
    CLD_EXITED = 1,
    CLD_KILLED = 2,
    CLD_DUMPED = 3
};

enum prctl_option {
    PR_SET_PDEATHSIG = 1,
    PR_GET_DUMPABLE = 3,
    PR_SET_DUMPABLE = 4,
    PR_SET_NAME = 15,
    PR_GET_NAME = 16
};

enum rlimit_resource {
    RLIMIT_STACK = 3,
    RLIMIT_NOFILE = 7,
    RLIMIT_NLIMITS = 16
};

struct utsname_k {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct sysinfo_k {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char reserved[4];
};

struct rlimit_k {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct rusage_k {
    struct timeval_k ru_utime;
    struct timeval_k ru_stime;
    int64_t fields[14];
};

struct waitid_info_k {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t pad;
    int32_t si_pid;
    uint32_t si_uid;
    int32_t si_status;
    int32_t pad2;
    int64_t si_utime;
    int64_t si_stime;
    uint8_t rest[80];
};

static process_t* current()
{
    return scheduler_get()->current;
}

long sys_exit(const int code)
{
    process_exit_status((code & 0xFF) << 8);
    return 0;
}

long sys_exit_group(const int code)
{
    process_exit_group((code & 0xFF) << 8);
    return 0;
}

long sys_wait4(const int pid, int* ustatus, const int options, void* rusage)
{
    if (ustatus && !uaccess_ok_write(ustatus, sizeof(int)))
        return -EFAULT;
    int status = 0;
    const long r = process_wait4(pid, &status, options);
    if (r > 0 && ustatus && copy_to_user(ustatus, &status, sizeof(status)) < 0)
        return -EFAULT;
    if (r > 0 && rusage) {
        const struct rusage_k ru = {0};
        if (copy_to_user(rusage, &ru, sizeof(ru)) < 0)
            return -EFAULT;
    }
    return r;
}

static long copy_string_array(char* const* uarray, char*** out, size_t* budget)
{
    *out = nullptr;
    size_t count = 0;
    if (uarray) {
        for (;; count++) {
            if (count >= EXEC_MAX_ARGS)
                return -E2BIG;
            const char* p;
            if (copy_from_user(&p, &uarray[count], sizeof(p)) < 0)
                return -EFAULT;
            if (p == nullptr)
                break;
        }
    }
    char** array = malloc((count + 1) * sizeof(char*) + *budget);
    if (array == nullptr)
        return -ENOMEM;
    char* strings = (char*)(array + count + 1);
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        const char* p;
        copy_from_user(&p, &uarray[i], sizeof(p));
        const long len = copy_string_from_user(strings + used, p, *budget - used);
        if (len < 0) {
            free(array);
            return len == -ENAMETOOLONG ? -E2BIG : len;
        }
        array[i] = strings + used;
        used += (size_t)len + 1;
    }
    array[count] = nullptr;
    *budget -= used;
    *out = array;
    return 0;
}

long sys_waitid(const int idtype, const int id, void* uinfo, const int options, void* rusage)
{
    if (options & ~(WNOHANG | WSTOPPED | WEXITED | WCONTINUED | WNOWAIT))
        return -EINVAL;
    if (!(options & WEXITED))
        return -EINVAL;

    int pid;
    switch (idtype) {
    case P_ALL:
        pid = -1;
        break;
    case P_PID:
    case P_PGID:
        if (id < 0 || (idtype == P_PID && id == 0))
            return -EINVAL;
        pid = id ? id : current()->tgid;
        break;
    case P_PIDFD: {
        const struct file_s* f = fd_get(id);
        pid = f ? pidfd_pid(f) : -EBADF;
        if (pid < 0)
            return pid;
        break;
    }
    default:
        return -EINVAL;
    }

    int status = 0;
    const long r = process_wait4(pid, &status, options & (WNOHANG | WNOWAIT));
    if (r < 0)
        return r;

    struct waitid_info_k info = {0};
    if (r > 0) {
        info.si_signo = SIGCHLD;
        info.si_pid = (int32_t)r;
        if ((status & 0x7F) == 0) {
            info.si_code = CLD_EXITED;
            info.si_status = (status >> 8) & 0xFF;
        } else {
            info.si_code = status & 0x80 ? CLD_DUMPED : CLD_KILLED;
            info.si_status = status & 0x7F;
        }
    }
    if (uinfo && copy_to_user(uinfo, &info, sizeof(info)) < 0)
        return -EFAULT;
    if (rusage) {
        const struct rusage_k ru = {0};
        if (copy_to_user(rusage, &ru, sizeof(ru)) < 0)
            return -EFAULT;
    }
    return 0;
}

long sys_execve(const char* upath, char* const* uargv, char* const* uenvp)
{
    char raw[VFS_PATH_MAX];
    char path[VFS_PATH_MAX];
    long err = copy_string_from_user(raw, upath, sizeof(raw));
    if (err < 0)
        return err;
    if (err == 0)
        return -ENOENT;
    err = vfs_resolve_root(current()->files->root, current()->files->cwd, raw, path);
    if (err)
        return err;

    size_t budget = EXEC_STRINGS_MAX;
    char** argv;
    char** envp = nullptr;
    err = copy_string_array(uargv, &argv, &budget);
    if (err == 0)
        err = copy_string_array(uenvp, &envp, &budget);
    if (err == 0)
        err = process_exec(current(), path, argv, envp);
    free(argv);
    free(envp);
    return err;
}

long sys_set_tid_address(int* tidptr)
{
    current()->clear_child_tid = (uint64_t)tidptr;
    return current()->pid;
}

long sys_arch_prctl(const int code, const uint64_t addr)
{
    switch (code) {
    case ARCH_SET_FS:
        if (addr >= USER_ADDR_LIMIT)
            return -EPERM;
        process_set_fs_base(current(), addr);
        return 0;
    case ARCH_GET_FS:
        return copy_to_user((void*)addr, &current()->fs_base, sizeof(uint64_t));
    default:
        return -EINVAL;
    }
}

long sys_prctl(const int option, const uint64_t arg2)
{
    switch (option) {
    case PR_SET_NAME:
    case PR_SET_PDEATHSIG:
    case PR_SET_DUMPABLE:
        return 0;
    case PR_GET_DUMPABLE:
        return 1;
    case PR_GET_NAME: {
        const char name[16] = "hermes";
        return copy_to_user((void*)arg2, name, sizeof(name));
    }
    default:
        return -EINVAL;
    }
}

static void set_field(char* field, const char* value)
{
    strncpy(field, value, 64);
}

long sys_uname(void* buf)
{
    struct utsname_k u = {0};
    set_field(u.sysname, "Hermes");
    set_field(u.nodename, "hermes");
    set_field(u.release, "0.1.0");
    set_field(u.version, "#1");
    set_field(u.machine, "x86_64");
    set_field(u.domainname, "(none)");
    return copy_to_user(buf, &u, sizeof(u));
}

long sys_sysinfo(void* info)
{
    struct sysinfo_k si = {0};
    si.uptime = (int64_t)(clock_monotonic_ns() / 1000000000ULL);
    si.totalram = pmm_total_pages() * PAGE_SIZE;
    si.freeram = pmm_free_pages() * PAGE_SIZE;
    si.procs = (uint16_t)process_count();
    si.mem_unit = 1;
    return copy_to_user(info, &si, sizeof(si));
}

static struct rlimit_k rlimit_of(const int resource)
{
    switch (resource) {
    case RLIMIT_STACK:
        return (struct rlimit_k){ USER_STACK_LIMIT, USER_STACK_LIMIT };
    case RLIMIT_NOFILE:
        return (struct rlimit_k){ PROC_MAX_FILE_DESCRIPTORS, PROC_MAX_FILE_DESCRIPTORS };
    default:
        return (struct rlimit_k){ RLIM_INFINITY, RLIM_INFINITY };
    }
}

long sys_prlimit64(const int pid, const int resource, const void* new_limit, void* old_limit)
{
    if (pid != 0 && pid != current()->tgid)
        return process_find(pid) ? -EPERM : -ESRCH;
    if (resource < 0 || resource >= RLIMIT_NLIMITS)
        return -EINVAL;
    const struct rlimit_k limit = rlimit_of(resource);
    if (old_limit && copy_to_user(old_limit, &limit, sizeof(limit)) < 0)
        return -EFAULT;
    if (new_limit) {
        struct rlimit_k next;
        if (copy_from_user(&next, new_limit, sizeof(next)) < 0)
            return -EFAULT;

        if (next.rlim_cur > next.rlim_max || next.rlim_max > limit.rlim_max)
            return -EPERM;
    }
    return 0;
}

long sys_getrusage(const int who, void* usage)
{
    (void)who;
    const struct rusage_k ru = {0};
    return copy_to_user(usage, &ru, sizeof(ru));
}

static uint64_t online_cpus()
{
    return cpu_count() >= 64 ? ~0ULL : (1ULL << cpu_count()) - 1;
}

static process_t* sched_target(const int pid)
{
    return pid == 0 ? current() : process_find(pid);
}

long sys_sched_getaffinity(const int pid, const size_t len, void* mask)
{
    if (pid < 0)
        return -EINVAL;
    const process_t* proc = sched_target(pid);
    if (proc == nullptr)
        return -ESRCH;
    if (len < sizeof(uint64_t))
        return -EINVAL;
    const uint64_t cpus = proc->affinity ? proc->affinity : online_cpus();
    if (copy_to_user(mask, &cpus, sizeof(cpus)) < 0)
        return -EFAULT;
    return sizeof(uint64_t);
}

long sys_sched_setaffinity(const int pid, const size_t len, const void* mask)
{
    if (pid < 0)
        return -EINVAL;
    process_t* proc = sched_target(pid);
    if (proc == nullptr)
        return -ESRCH;
    uint64_t cpus = 0;
    if (copy_from_user(&cpus, mask, len < sizeof(cpus) ? len : sizeof(cpus)) < 0)
        return -EFAULT;
    cpus &= online_cpus();
    if (cpus == 0)
        return -EINVAL;
    proc->affinity = cpus;
    return 0;
}

static long sched_param_in(const void* uparam, int* priority)
{
    if (uparam == nullptr)
        return -EINVAL;
    return copy_from_user(priority, uparam, sizeof(*priority)) < 0 ? -EFAULT : 0;
}

long sys_sched_setparam(const int pid, const void* param)
{
    if (pid < 0)
        return -EINVAL;
    if (sched_target(pid) == nullptr)
        return -ESRCH;
    int priority;
    const long err = sched_param_in(param, &priority);
    if (err)
        return err;
    return priority == 0 ? 0 : -EINVAL;
}

long sys_sched_setscheduler(const int pid, const int policy, const void* param)
{
    if (pid < 0)
        return -EINVAL;
    if (sched_target(pid) == nullptr)
        return -ESRCH;
    int priority;
    const long err = sched_param_in(param, &priority);
    if (err)
        return err;
    switch (policy & ~SCHED_RESET_ON_FORK) {
    case SCHED_OTHER:
        return priority == 0 ? 0 : -EINVAL;
    case SCHED_BATCH:
    case SCHED_IDLE:
        return priority == 0 ? -EPERM : -EINVAL;
    case SCHED_FIFO:
    case SCHED_RR:
        return priority >= 1 && priority <= 99 ? -EPERM : -EINVAL;
    default:
        return -EINVAL;
    }
}

long sys_sched_rr_get_interval(const int pid, struct timespec_k* uts)
{
    if (pid < 0)
        return -EINVAL;
    if (sched_target(pid) == nullptr)
        return -ESRCH;
    const struct timespec_k ts = { .tv_sec = 0, .tv_nsec = 1000000000LL / ticks_hz() };
    return copy_to_user(uts, &ts, sizeof(ts));
}

long sys_getrandom(void* buf, size_t len, const unsigned int flags)
{
    (void)flags;
    if (len > GETRANDOM_MAX)
        len = GETRANDOM_MAX;
    if (!uaccess_ok_write(buf, len))
        return -EFAULT;
    random_fill(buf, len);
    return (long)len;
}

long sys_getgroups(const int size, uint32_t* list)
{
    const process_t* proc = current();
    if (size < 0)
        return -EINVAL;
    if (size == 0)
        return proc->group_count;
    if (size < proc->group_count)
        return -EINVAL;
    if (copy_to_user(list, proc->groups, proc->group_count * sizeof(uint32_t)) < 0)
        return -EFAULT;
    return proc->group_count;
}

long sys_setgroups(const int size, const uint32_t* list)
{
    if (size < 0 || size > PROC_MAX_GROUPS)
        return -EINVAL;
    uint32_t groups[PROC_MAX_GROUPS];
    if (size && copy_from_user(groups, list, size * sizeof(uint32_t)) < 0)
        return -EFAULT;
    process_t* proc = current();
    memcpy(proc->groups, groups, size * sizeof(uint32_t));
    proc->group_count = size;
    return 0;
}
