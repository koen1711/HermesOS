#ifndef OS_STR_H
#define OS_STR_H
#include <stdarg.h>

#include "hardware/memory/alloc.h"

typedef struct
{
    char* str;
    size_t size;
} string;

/* Hardware text mode color constants. */

static size_t strlen(const char* str)
{
    size_t len = 0;
    while (str[len])
        len++;
    return len;
}

static char* strcmp(const char* str1, const char* str2)
{
    size_t i = 0;
    while (str1[i] && str2[i]) {
        if (str1[i] != str2[i])
            return (char*)(str1 + i);
        i++;
    }
    if (str1[i] != str2[i])
        return (char*)(str1 + i);
    return NULL;
}

static char* strchr(const char* str, char c)
{
    while (*str != '\0' && *str != c)
        str++;
    return *str == c ? (char*)str : NULL;
}

static char* strtok(char* str, const char* delim)
{
    static char* next_token = NULL;

    if (str != NULL) {
        next_token = str;
    } else if (next_token == NULL) {
        return NULL;
    }

    // Skip leading delimiters
    while (*next_token && strchr(delim, *next_token)) {
        next_token++;
    }

    if (*next_token == '\0') {
        next_token = NULL;
        return NULL;
    }

    char* token_start = next_token;

    // Find the end of the token
    while (*next_token && !strchr(delim, *next_token)) {
        next_token++;
    }

    if (*next_token) {
        *next_token = '\0'; // Null-terminate the token
        next_token++; // Move to the start of the next token
    } else {
        next_token = NULL; // No more tokens
    }

    return token_start;
}

static char* strcpy(char* dest, const char* src)
{
    size_t i;
    for (i = 0; src[i]; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
    return dest;
}

static char* strdup(const char* str)
{
    size_t len = strlen(str) + 1;
    char* copy = malloc(len);
    if (copy == NULL) return NULL;
    memcpy(copy, str, len);
    return copy;
}

static const char* str_concat_variadic(const int count, ...)
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
        const size_t len = strlen(str);
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

static char* str_u64(uint64_t n)
{
    char temp[21]; // 20 digits + null terminator
    int pos = 0;

    // Special case for 0
    if (n == 0) {
        char* buffer = malloc(2);
        if (buffer == NULL) return NULL;
        buffer[0] = '0';
        buffer[1] = '\0';
        return buffer;
    }

    // Convert digits in reverse order
    while (n > 0) {
        temp[pos++] = '0' + (n % 10);
        n /= 10;
    }

    // Allocate exact space needed
    char* buffer = malloc(pos + 1); // digits + null terminator
    if (buffer == NULL) return NULL;

    // Reverse the digits and copy to final buffer
    for (int i = 0; i < pos; i++) {
        buffer[i] = temp[pos - 1 - i];
    }
    buffer[pos] = '\0';

    return buffer;
}

static char* str_u32(uint32_t n)
{
    char temp[11]; // 10 digits + null terminator
    int pos = 0;

    // Special case for 0
    if (n == 0) {
        char* buffer = malloc(2);
        if (buffer == NULL) return NULL;
        buffer[0] = '0';
        buffer[1] = '\0';
        return buffer;
    }

    // Convert digits in reverse order
    while (n > 0) {
        temp[pos++] = '0' + (n % 10);
        n /= 10;
    }

    // Allocate exact space needed
    char* buffer = malloc(pos + 1); // digits + null terminator
    if (buffer == NULL) return NULL;

    // Reverse the digits and copy to final buffer
    for (int i = 0; i < pos; i++) {
        buffer[i] = temp[pos - 1 - i];
    }
    buffer[pos] = '\0';

    return buffer;
}

static char* str_u16(uint16_t n)
{
    char temp[6]; // 5 digits + null terminator
    int pos = 0;

    // Special case for 0
    if (n == 0) {
        char* buffer = malloc(2);
        if (buffer == NULL) return NULL;
        buffer[0] = '0';
        buffer[1] = '\0';
        return buffer;
    }

    // Convert digits in reverse order
    while (n > 0) {
        temp[pos++] = '0' + (n % 10);
        n /= 10;
    }

    // Allocate exact space needed
    char* buffer = malloc(pos + 1); // digits + null terminator
    if (buffer == NULL) return NULL;

    // Reverse the digits and copy to final buffer
    for (int i = 0; i < pos; i++) {
        buffer[i] = temp[pos - 1 - i];
    }
    buffer[pos] = '\0';

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
        const uint8_t digit = n & 0xF;
        buffer[1 - i] = digit < 10 ? '0' + digit : 'A' + digit - 10;
        n >>= 4;
    }
    return buffer;
}

static char* str_utf16_to_utf8(const uint16_t* utf16)
{
    // First, count UTF-16 code units
    size_t len = 0;
    while (utf16[len])
        len++;

    // Allocate enough space for worst case (4 bytes per UTF-16 code unit plus null terminator)
    char* utf8 = malloc(len * 4 + 1);
    if (utf8 == NULL) return NULL;

    size_t utf8_pos = 0;
    size_t i = 0;

    while (i < len) {
        uint32_t c = utf16[i++];

        // Check for surrogate pairs
        if (c >= 0xD800 && c <= 0xDBFF && i < len) {
            const uint32_t c2 = utf16[i];
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                // Valid surrogate pair - compute the actual code point
                c = 0x10000 + ((c & 0x3FF) << 10) + (c2 & 0x3FF);
                i++; // Consume the second surrogate
            }
        }

        // Encode to UTF-8
        if (c < 0x80) {
            utf8[utf8_pos++] = c;
        } else if (c < 0x800) {
            utf8[utf8_pos++] = 0xC0 | (c >> 6);
            utf8[utf8_pos++] = 0x80 | (c & 0x3F);
        } else if (c < 0x10000) {
            utf8[utf8_pos++] = 0xE0 | (c >> 12);
            utf8[utf8_pos++] = 0x80 | ((c >> 6) & 0x3F);
            utf8[utf8_pos++] = 0x80 | (c & 0x3F);
        } else {
            utf8[utf8_pos++] = 0xF0 | (c >> 18);
            utf8[utf8_pos++] = 0x80 | ((c >> 12) & 0x3F);
            utf8[utf8_pos++] = 0x80 | ((c >> 6) & 0x3F);
            utf8[utf8_pos++] = 0x80 | (c & 0x3F);
        }
    }

    utf8[utf8_pos] = '\0';
    return utf8;
}

static void strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
}

static int strncmp(const char* dest, const char* src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (dest[i] != src[i]) {
            return dest[i] - src[i];
        }
        if (dest[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

static void strncat(char* dest, const char* src, size_t n) {
    size_t dest_len = strlen(dest);
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[dest_len + i] = src[i];
    }
    dest[dest_len + i] = '\0';
}

#endif //OS_STR_H
