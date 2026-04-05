#include "panic.h"

#include "hardware/terminal/stdio.h"

void panic(const char* message, const char* file, int line)
{
    printf("Kernel Panic: %s\n", message);
    printf("File: %s, Line: %d\n", file, line);

    // Halt the system
    while (1) {
        asm volatile ("cli; hlt");
    }
}
