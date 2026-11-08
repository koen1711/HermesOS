#ifndef OS_SYSCALLS_UACCESS_H
#define OS_SYSCALLS_UACCESS_H

#include <os/stddef.h>
#include <os/stdbool.h>
#include <os/errno.h>
#include <hardware/memory/alloc.h>

bool uaccess_check(const void* ptr, size_t len, bool write);

static bool uaccess_ok(const void* ptr, const size_t len)
{
    return uaccess_check(ptr, len, false);
}

static bool uaccess_ok_write(const void* ptr, const size_t len)
{
    return uaccess_check(ptr, len, true);
}

static int copy_from_user(void* dst, const void* user_src, const size_t len)
{
    if (!uaccess_ok(user_src, len))
        return -EFAULT;
    memcpy(dst, user_src, len);
    return 0;
}

static int copy_to_user(void* user_dst, const void* src, const size_t len)
{
    if (!uaccess_ok_write(user_dst, len))
        return -EFAULT;
    memcpy(user_dst, src, len);
    return 0;
}

long copy_string_from_user(char* dst, const char* user_src, size_t size);

#endif //OS_SYSCALLS_UACCESS_H
