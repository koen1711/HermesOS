#ifndef OS_STR_H
#define OS_STR_H
#include <stdarg.h>

#include "hardware/memory/alloc.h"

/* Hardware text mode color constants. */

static size_t strlen(const char* str)
{
    size_t len = 0;
    while (str[len])
        len++;
    return len;
}

static const char* str_concat_variadic(int count, ...)
{
    va_list args;
    va_start(args, count);

    // First pass: calculate total length needed
    size_t total_len = 0;
    va_list args_copy;
    va_copy(args_copy, args);

    for (int i = 0; i < count; i++) {
        const char* str = va_arg(args_copy, const char*);
        total_len += strlen(str);
    }

    va_end(args_copy);

    // Allocate memory for the result
    char* result = malloc(total_len + 1);
    if (result == NULL) {
        va_end(args);
        return NULL;
    }

    // Second pass: copy strings
    size_t current_pos = 0;
    for (int i = 0; i < count; i++) {
        const char* str = va_arg(args, const char*);
        size_t len = strlen(str);
        memcpy(result + current_pos, str, len);
        current_pos += len;
    }

    // Add null terminator
    result[total_len] = '\0';

    va_end(args);
    return result;
}

static const char* str_concat(const char* str1, const char* str2)
{
    const size_t len1 = strlen(str1);
    const size_t len2 = strlen(str2);
    char* result = malloc(len1 + len2 + 1);
    for (size_t i = 0; i < len1; i++)
        result[i] = str1[i];
    for (size_t i = 0; i < len2; i++)
        result[len1 + i] = str2[i];
    result[len1 + len2] = '\0';
    return result;
}

static char* str_u32(uint32_t n)
{
    char* buffer = malloc(11); // 10 digits + null terminator
    if (buffer == NULL) return NULL; // Check for allocation failure

    buffer[10] = '\0'; // Add null terminator
    for (int i = 0; i < 10; i++) {
        buffer[9 - i] = '0' + n % 10;
        n /= 10;
    }
    return buffer;
}

static char* str_u16(uint16_t n)
{
    char* buffer = malloc(6); // 5 digits + null terminator
    if (buffer == NULL) return NULL;

    buffer[5] = '\0';
    for (int i = 0; i < 5; i++) {
        buffer[4 - i] = '0' + n % 10;
        n /= 10;
    }
    return buffer;
}

static char* str_u8(uint8_t n)
{
    char* buffer = malloc(4); // 3 digits + null terminator
    if (buffer == NULL) return NULL;

    buffer[3] = '\0';
    for (int i = 0; i < 3; i++) {
        buffer[2 - i] = '0' + n % 10;
        n /= 10;
    }
    return buffer;
}

static char* str_u32_hex(uint32_t n)
{
    char* buffer = malloc(9); // 8 digits + null terminator
    if (buffer == NULL) return NULL;

    buffer[8] = '\0';
    for (int i = 0; i < 8; i++) {
        uint8_t digit = n & 0xF;
        buffer[7 - i] = digit < 10 ? '0' + digit : 'A' + digit - 10;
        n >>= 4;
    }
    return buffer;
}

static char* str_u16_hex(uint16_t n)
{
    char* buffer = malloc(5); // 4 digits + null terminator
    if (buffer == NULL) return NULL;

    buffer[4] = '\0';
    for (int i = 0; i < 4; i++) {
        uint8_t digit = n & 0xF;
        buffer[3 - i] = digit < 10 ? '0' + digit : 'A' + digit - 10;
        n >>= 4;
    }
    return buffer;
}

static char* str_u8_hex(uint8_t n)
{
    char* buffer = malloc(3); // 2 digits + null terminator
    if (buffer == NULL) return NULL;

    buffer[2] = '\0';
    for (int i = 0; i < 2; i++) {
        uint8_t digit = n & 0xF;
        buffer[1 - i] = digit < 10 ? '0' + digit : 'A' + digit - 10;
        n >>= 4;
    }
    return buffer;
}

#endif //OS_STR_H
