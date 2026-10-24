#ifndef OS_SYSCALLS_UACCESS_H
#define OS_SYSCALLS_UACCESS_H

#include <os/stddef.h>
#include <os/stdbool.h>

bool uaccess_ok(const void* ptr, size_t len);
bool uaccess_ok_write(const void* ptr, size_t len);
int copy_from_user(void* dst, const void* user_src, size_t len);
int copy_to_user(void* user_dst, const void* src, size_t len);
long copy_string_from_user(char* dst, const char* user_src, size_t size);

#endif //OS_SYSCALLS_UACCESS_H
