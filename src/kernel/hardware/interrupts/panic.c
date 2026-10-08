#include "panic.h"

#include "drivers/terminal/terminal.h"

void panic(const char* message, const char* file, int line)
{
    terminal_printf("Kernel Panic: %s\n", message);
    terminal_printf("File: %s, Line: %d\n", file, line);

    // Halt the system
    while (1) {
        asm volatile ("cli; hlt");
    }
}
