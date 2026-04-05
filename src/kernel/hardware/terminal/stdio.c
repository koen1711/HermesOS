#include "stdio.h"
#include "terminal.h"
#include "procs/vga.h"

#include <stdarg.h>
#include <hardware/memory/alloc.h>
#include <utils/str/vsnprintf.h>

#include "hardware/interrupts/panic.h"


terminal_mode current_mode = VGA_TEXT_MODE;
terminal_contents* cur_terminal = NULL;

extern void* _kernel_virtual_end;
static void* next = &_kernel_virtual_end;

void* old_malloc(const uintptr_t size) {
    void* result = next;
    next += size;
    return result;
}

void old_free(void* ptr) {

}

void* old_realloc(void* ptr, const uintptr_t size) {
    if (ptr == NULL) {
        return old_malloc(size);
    }
    if (size == 0) {
        old_free(ptr);
        return NULL;
    }

    // For simplicity, we will just allocate a new block and copy the contents
    void* new_ptr = old_malloc(size);
    if (new_ptr == NULL) {
        return NULL; // Allocation failed
    }

    // Copy the old contents to the new block
    memcpy(new_ptr, ptr, size); // Assuming size is less than or equal to the size of the old block
    return new_ptr;
}


void terminal_initialize(const terminal_mode mode)
{
    current_mode = mode;
    terminal_contents* terminal = old_malloc(sizeof(terminal_contents));
    if (terminal == NULL)
    {
        panic("Failed to allocate terminal", __FILE__, __LINE__);
        return;
    }

    
    // Initialize the terminal content buffer
    terminal->length = 0;
    terminal->capacity = 1024; // Initial capacity
    terminal->content = old_malloc(terminal->capacity);
    if (terminal->content == NULL)
    {
        panic("Failed to allocate terminal content buffer", __FILE__, __LINE__);
        return;
    }
    memset((void*)terminal->content, 0, terminal->capacity);

    vga_text_initialize(terminal);
    cur_terminal = terminal;
}

void terminal_update()
{
    if (current_mode == VGA_TEXT_MODE) {
        vga_text_update();
    }
}

void printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    // Calculate the size needed for the formatted string
    const size_t size = vsnprintf(NULL, 0, format, args);
    va_end(args);

    // Ensure we have enough space in the terminal content buffer
    if (cur_terminal->length + size >= cur_terminal->capacity) {
        cur_terminal->capacity = cur_terminal->length + size + 1; // +1 for null terminator
        cur_terminal->content = old_realloc((void*)cur_terminal->content, cur_terminal->capacity);
        if (cur_terminal->content == NULL)
            return;
    }

    // Format the string and append it to the terminal content buffer
    va_start(args, format);
    vsnprintf((char*)cur_terminal->content + cur_terminal->length, size + 1, format, args);
    va_end(args);

    // Update the length of the terminal content
    cur_terminal->length += size;

    terminal_update();
}

