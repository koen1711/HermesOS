#include "uaccess.h"

#include <hardware/memory/alloc.h>
#include <hardware/memory/vm.h>
#include <drivers/scheduler/process.h>
#include <os/errno.h>

static bool uaccess_check(const void* ptr, const size_t len, const bool write)
{
    const process_t* current = scheduler_get()->current;
    if (current == nullptr)
        return ptr != nullptr || len == 0;
    if (len == 0)
        return true;

    return current->vm != nullptr && vm_populate(current->vm, (uint64_t)ptr, len, write);
}

bool uaccess_ok(const void* ptr, const size_t len)
{
    return uaccess_check(ptr, len, false);
}

bool uaccess_ok_write(const void* ptr, const size_t len)
{
    return uaccess_check(ptr, len, true);
}

int copy_from_user(void* dst, const void* user_src, const size_t len)
{
    if (!uaccess_ok(user_src, len))
        return -EFAULT;
    memcpy(dst, user_src, len);
    return 0;
}

int copy_to_user(void* user_dst, const void* src, const size_t len)
{
    if (!uaccess_ok_write(user_dst, len))
        return -EFAULT;
    memcpy(user_dst, src, len);
    return 0;
}

long copy_string_from_user(char* dst, const char* user_src, const size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if ((i == 0 || ((uintptr_t)(user_src + i) & (PAGE_SIZE - 1)) == 0) && !uaccess_ok(user_src + i, 1))
            return -EFAULT;
        dst[i] = user_src[i];
        if (dst[i] == '\0')
            return (long)i;
    }
    return -ENAMETOOLONG;
}
