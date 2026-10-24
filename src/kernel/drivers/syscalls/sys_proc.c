#include "sys.h"

#include "uaccess.h"
#include <drivers/fs/vfs/vfs.h>
#include <drivers/scheduler/process.h>
#include <drivers/timer/clock.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/pmm.h>
#include <utils/random/random.h>
#include <utils/str/str.h>
#include <os/errno.h>

#define EXEC_MAX_ARGS 4096
#define EXEC_STRINGS_MAX (128 * 1024)
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define RLIM_INFINITY (~0ULL)
#define GETRANDOM_MAX (32 * 1024 * 1024)

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

long sys_execve(const char* upath, char* const* uargv, char* const* uenvp)
{
    char raw[VFS_PATH_MAX];
    char path[VFS_PATH_MAX];
    long err = copy_string_from_user(raw, upath, sizeof(raw));
    if (err < 0)
        return err;
    if (err == 0)
        return -ENOENT;
    err = vfs_normalize_path(current()->files->cwd, raw, path);
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
    struct utsname_k u;
    memset(&u, 0, sizeof(u));
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
    struct sysinfo_k si;
    memset(&si, 0, sizeof(si));
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

long sys_sched_getaffinity(const int pid, const size_t len, void* mask)
{
    if (pid != 0 && process_find(pid) == nullptr)
        return -ESRCH;
    if (len < sizeof(uint64_t))
        return -EINVAL;
    const uint64_t one_cpu = 1;
    if (copy_to_user(mask, &one_cpu, sizeof(one_cpu)) < 0)
        return -EFAULT;
    return sizeof(uint64_t);
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
    (void)list;
    return size < 0 ? -EINVAL : 0;
}
