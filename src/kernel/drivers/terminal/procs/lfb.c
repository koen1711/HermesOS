#include "lfb.h"

#include "vga.h"
#include <utils/str/str.h>

#include "hardware/port/ports.h"

terminal_contents* lfb_terminal = NULL;

void lfb_text_initialize(terminal_contents* term)
{
    lfb_terminal = term;
    // TODO: Implement
}

void lfb_text_put_at(const char c, const uint8_t color, const size_t x, const size_t y)
{
    // TODO: Implement
}

void lfb_text_put_char(const char c, const size_t x, const size_t y)
{
    // TODO: Implement
}

void lfb_text_update()
{
    if (lfb_terminal == NULL || lfb_terminal->content == NULL) return;

    size_t row = 0;
    size_t column = 0;
    for (size_t i = 0; i < lfb_terminal->length; i++) {
        if (lfb_terminal->content[i] == '\n') {
            row++;
            column = 0;
            continue;
        }
        lfb_text_put_char(lfb_terminal->content[i], column, row);
        column++;
    }
}

void lfb_text_toggle_cursor(const bool enable)
{
    // TODO: Implement
}