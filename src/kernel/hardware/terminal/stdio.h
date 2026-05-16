#ifndef STDIO_H
#define STDIO_H
#include <os/stddef.h>

void terminal_initialize();

int snprintf(char *str, size_t size, const char *format, ...);
void printf(const char* format, ...);

#endif //STDIO_H
