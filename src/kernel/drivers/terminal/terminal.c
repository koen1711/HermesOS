#include "terminal.h"

#include "hardware/interrupts/panic.h"
#include "hardware/memory/alloc.h"
#include <utils/str/vsnprintf.h>

#include "os/stdarg.h"

#include "procs/vga.h"
#include "procs/lfb.h"

terminal_mode current_mode = VGA_TEXT_MODE;
terminal_contents* cur_terminal = NULL;

void terminal_initialize(const terminal_mode mode)
{
    current_mode = mode;
    terminal_contents* terminal = malloc(sizeof(terminal_contents));
    if (terminal == NULL)
    {
        panic("Failed to allocate terminal", __FILE__, __LINE__);
        return;
    }

    // Initialize the terminal content buffer
    terminal->length = 0;
    terminal->capacity = 1024; // Initial capacity
    terminal->content = malloc(terminal->capacity);
    if (terminal->content == NULL)
    {
        panic("Failed to allocate terminal content buffer", __FILE__, __LINE__);
        return;
    }
    memset((void*)terminal->content, 0, terminal->capacity);

    if (mode == VGA_TEXT_MODE) {
        vga_text_initialize(terminal);
    } else if (mode == VGA_LFB_MODE) {
        lfb_text_initialize(terminal);
    }
    cur_terminal = terminal;
}

void terminal_switch_mode(terminal_mode mode)
{
    current_mode = mode;
    if (mode == VGA_TEXT_MODE) {
        vga_text_initialize(cur_terminal);
    } else if (mode == VGA_LFB_MODE) {
        lfb_text_initialize(cur_terminal);
    }
}

void terminal_printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    // Calculate the required length for the formatted string
    const int required_length = vsnprintf(NULL, 0, format, args);
    va_end(args);

    if (required_length < 0) {
        return; // Formatting error
    }

    // Ensure the terminal content buffer has enough capacity
    if (cur_terminal->length + required_length >= cur_terminal->capacity) {
        size_t new_capacity = cur_terminal->capacity * 2;
        while (cur_terminal->length + required_length >= new_capacity) {
            new_capacity *= 2;
        }
        const char* new_content = realloc(cur_terminal->content, new_capacity);
        if (new_content == NULL) {
            return; // Allocation failed
        }
        cur_terminal->content = new_content;
        cur_terminal->capacity = new_capacity;
    }

    // Format the string into the terminal content buffer
    va_start(args, format);
    vsnprintf((char*)cur_terminal->content + cur_terminal->length, cur_terminal->capacity - cur_terminal->length, format, args);
    va_end(args);

    cur_terminal->length += required_length;

    terminal_update();
}

void terminal_update()
{
    if (current_mode == VGA_TEXT_MODE) {
        vga_text_update();
    } else if (current_mode == VGA_LFB_MODE) {
        lfb_text_update();
    }
}