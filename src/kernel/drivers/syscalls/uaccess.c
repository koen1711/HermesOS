#include "uaccess.h"

#include <hardware/memory/alloc.h>
#include <hardware/memory/mmu.h>
#include <scheduler/process.h>
#include <os/errno.h>

bool uaccess_ok(const void* ptr, const size_t len)
{
    if (scheduler_get()->current == nullptr)
        return ptr != nullptr || len == 0;
    const uintptr_t start = (uintptr_t)ptr;
    constexpr uintptr_t end = USER_VIRTUAL_BASE + USER_MEMORY_SIZE;
    return start >= USER_VIRTUAL_BASE && start <= end && len <= end - start;
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
    if (!uaccess_ok(user_dst, len))
        return -EFAULT;
    memcpy(user_dst, src, len);
    return 0;
}
