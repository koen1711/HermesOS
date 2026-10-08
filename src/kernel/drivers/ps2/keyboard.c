#include "keyboard.h"

#include <os/stdbool.h>
#include <hardware/port/ports.h>
#include "codes.h"
#include "drivers/terminal/terminal.h"

int ps2_keyboard_initialize() {
    port_clear_read_buffer(COMMAND_PORT, DATA_PORT);

    port_write_u8(COMMAND_PORT, 0xAE);
    port_write_u8(COMMAND_PORT, 0x20);

    const uint8_t status = (port_read_u8(DATA_PORT) | 1) & ~0x10;
    port_write_u8(COMMAND_PORT, 0x60);
    port_write_u8(DATA_PORT, status);

    port_write_u8(COMMAND_PORT, 0xAA);
    if (port_read_u8(DATA_PORT) != 0x55)
        return -1;

    return 0;
}

void ps2_keyboard_interrupt_handler() {
    char charKey = '\0';
    key_code key = port_read_u8(DATA_PORT);
    const bool keyUp = key & 0x80;
    static uint8_t escape = 0;
    static uint8_t ctrl = 0;
    static uint8_t shift = 0;
    static uint8_t alt = 0;

    // Check if modifiers changed.
    switch(key) {
        case SC_RELEASE | SC_LSHIFT: shift = 0; break;
        case SC_RELEASE | SC_RSHIFT: shift = 0; break;
        case SC_RELEASE | SC_LCTRL: ctrl = 0; break;
        case SC_RELEASE | SC_LALT: alt = 0; break;

        case SC_LSHIFT:
        case SC_RSHIFT:
            shift = 0xFF; break;
        case SC_LCTRL: ctrl = 0xFF; break;
        case SC_LALT: alt = 0xFF; break;
    }

    // Press or release handler and fix scan code
    if(key & SC_RELEASE)
        key &= ~SC_RELEASE;

    // Virtual Key Code Mapping
    if(key == SC_ESCAPE_CODE) {
        escape = 0xFF;
    }

    switch(key) {
        // Letters
        case SC_A: charKey = shift ? 'A' : 'a'; break;
        case SC_B: charKey = shift ? 'B' : 'b'; break;
        case SC_C: charKey = shift ? 'C' : 'c'; break;
        case SC_D: charKey = shift ? 'D' : 'd'; break;
        case SC_E: charKey = shift ? 'E' : 'e'; break;
        case SC_F: charKey = shift ? 'F' : 'f'; break;
        case SC_G: charKey = shift ? 'G' : 'g'; break;
        case SC_H: charKey = shift ? 'H' : 'h'; break;
        case SC_I: charKey = shift ? 'I' : 'i'; break;
        case SC_J: charKey = shift ? 'J' : 'j'; break;
        case SC_K: charKey = shift ? 'K' : 'k'; break;
        case SC_L: charKey = shift ? 'L' : 'l'; break;
        case SC_M: charKey = shift ? 'M' : 'm'; break;
        case SC_N: charKey = shift ? 'N' : 'n'; break;
        case SC_O: charKey = shift ? 'O' : 'o'; break;
        case SC_P: charKey = shift ? 'P' : 'p'; break;
        case SC_Q: charKey = shift ? 'Q' : 'q'; break;
        case SC_R: charKey = shift ? 'R' : 'r'; break;
        case SC_S: charKey = shift ? 'S' : 's'; break;
        case SC_T: charKey = shift ? 'T' : 't'; break;
        case SC_U: charKey = shift ? 'U' : 'u'; break;
        case SC_V: charKey = shift ? 'V' : 'v'; break;
        case SC_W: charKey = shift ? 'W' : 'w'; break;
        case SC_X: charKey = shift ? 'X' : 'x'; break;
        case SC_Y: charKey = shift ? 'Y' : 'y'; break;
        case SC_Z: charKey = shift ? 'Z' : 'z'; break;

        // Numbers
        case SC_0: charKey = shift ? ')' : '0'; break;
        case SC_1: charKey = shift ? '!' : '1'; break;
        case SC_2: charKey = shift ? '@' : '2'; break;
        case SC_3: charKey = shift ? '#' : '3'; break;
        case SC_4: charKey = shift ? '$' : '4'; break;
        case SC_5: charKey = shift ? '%' : '5'; break;
        case SC_6: charKey = shift ? '^' : '6'; break;
        case SC_7: charKey = shift ? '&' : '7'; break;
        case SC_8: charKey = shift ? '*' : '8'; break;
        case SC_9: charKey = shift ? '(' : '9'; break;

        // Special
        case SC_ESCAPE: charKey = '\0'; break;
        case SC_BACKTICK: charKey = shift ? '~' : '`'; break;
        case SC_MINUS: charKey = shift ? '_' : '-'; break;
        case SC_EQUALS: charKey = shift ? '+' : '='; break;
        case SC_BACKSPACE: charKey = '\b'; break;

        // Function Keys
        case SC_F1: charKey = '\0'; break;
        case SC_F2: charKey = '\0'; break;
        case SC_F3: charKey = '\0'; break;
        case SC_F4: charKey = '\0'; break;
        case SC_F5: charKey = '\0'; break;
        case SC_F6: charKey = '\0'; break;
        case SC_F7: charKey = '\0'; break;
        case SC_F8: charKey = '\0'; break;
        case SC_F9: charKey = '\0'; break;
        case SC_F10: charKey = '\0'; break;
        case SC_F11: charKey = '\0'; break;
        case SC_F12: charKey = '\0'; break;

        // Locks
        case SC_CAPSLOCK: charKey = '\0'; break;
        case SC_NUMLOCK: charKey = '\0'; break;
        case SC_SCRLOCK: charKey = '\0'; break;

        // Miscellaneous
        case SC_LSHIFT: charKey = '\0'; break;
        case SC_RSHIFT: charKey = '\0'; break;
        case SC_LCTRL: charKey = '\0'; break;
        case SC_LALT: charKey = '\0'; break;
        case SC_ENTER: charKey = '\n'; break;
        case SC_SPACE: charKey = ' '; break;

        // Punctuation
        case SC_BRACKET_OPEN: charKey = shift ? '{' : '['; break;
        case SC_BRACKET_CLOSE: charKey = shift ? '}' : ']'; break;
        case SC_BACKSLASH: charKey = shift ? '|' : '\\'; break;
        case SC_SEMI_COLON: charKey = shift ? ':' : ';'; break;
        case SC_SINGLE_QUOTE: charKey = shift ? '"' : '\''; break;
        case SC_COMMA: charKey = shift ? '<' : ','; break;
        case SC_PERIOD: charKey = shift ? '>' : '.'; break;
        case SC_SLASH: charKey = shift ? '?' : '/'; break;
    }

    escape = 0;

    if (charKey != '\0' && !keyUp) {
        terminal_printf("%c", charKey);
    }

}