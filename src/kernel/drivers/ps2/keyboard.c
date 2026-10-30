#include "keyboard.h"

#include <os/stdbool.h>
#include <hardware/port/ports.h>
#include <hardware/interrupts/irq.h>
#include "codes.h"
#include "drivers/terminal/terminal.h"
#include "drivers/terminal/tty.h"

#define SC_KEYPAD_STAR 0x37
#define SC_KEYPAD_7 0x47
#define SC_KEYPAD_8 0x48
#define SC_KEYPAD_9 0x49
#define SC_KEYPAD_MINUS 0x4A
#define SC_KEYPAD_4 0x4B
#define SC_KEYPAD_5 0x4C
#define SC_KEYPAD_6 0x4D
#define SC_KEYPAD_PLUS 0x4E
#define SC_KEYPAD_1 0x4F
#define SC_KEYPAD_2 0x50
#define SC_KEYPAD_3 0x51
#define SC_KEYPAD_0 0x52
#define SC_KEYPAD_PERIOD 0x53

#define SC_EXT_UP 0x48
#define SC_EXT_DOWN 0x50
#define SC_EXT_LEFT 0x4B
#define SC_EXT_RIGHT 0x4D
#define SC_EXT_HOME 0x47
#define SC_EXT_END 0x4F
#define SC_EXT_PAGE_UP 0x49
#define SC_EXT_PAGE_DOWN 0x51
#define SC_EXT_INSERT 0x52
#define SC_EXT_DELETE 0x53
#define SC_EXT_KEYPAD_ENTER 0x1C
#define SC_EXT_KEYPAD_SLASH 0x35

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL 0x02
#define PS2_TIMEOUT 100000

static int ps2_wait_write() {
    for (int i = 0; i < PS2_TIMEOUT; i++) {
        if (!(port_read_u8(COMMAND_PORT) & PS2_STATUS_INPUT_FULL))
            return 0;
    }
    return -1;
}

static int ps2_wait_read() {
    for (int i = 0; i < PS2_TIMEOUT; i++) {
        if (port_read_u8(COMMAND_PORT) & PS2_STATUS_OUTPUT_FULL)
            return 0;
    }
    return -1;
}

static void ps2_command(const uint8_t command) {
    ps2_wait_write();
    port_write_u8(COMMAND_PORT, command);
}

static void ps2_write_data(const uint8_t data) {
    ps2_wait_write();
    port_write_u8(DATA_PORT, data);
}

static int ps2_read_data() {
    if (ps2_wait_read() < 0)
        return -1;
    return port_read_u8(DATA_PORT);
}

int ps2_keyboard_initialize() {
    ps2_command(0xAD);
    port_clear_read_buffer(COMMAND_PORT, DATA_PORT);

    ps2_command(0x20);
    const int config = ps2_read_data();
    const uint8_t status = ((config < 0 ? 0x01 : config) | 1) & ~0x10;

    ps2_command(0xAA);
    const int self_test = ps2_read_data();

    ps2_command(0x60);
    ps2_write_data(status);

    ps2_command(0xAE);
    port_clear_read_buffer(COMMAND_PORT, DATA_PORT);

    const int ret = irq_register_handler(PS2_KEYBOARD_IRQ, ps2_keyboard_interrupt_handler, nullptr);
    if (self_test != 0x55)
        return -1;
    return ret;
}

static const char keymap[128] = {
    [SC_ESCAPE] = 0x1B,
    [SC_1] = '1', [SC_2] = '2', [SC_3] = '3', [SC_4] = '4', [SC_5] = '5',
    [SC_6] = '6', [SC_7] = '7', [SC_8] = '8', [SC_9] = '9', [SC_0] = '0',
    [SC_MINUS] = '-', [SC_EQUALS] = '=', [SC_BACKSPACE] = 0x7F, [SC_TAB] = '\t',
    [SC_Q] = 'q', [SC_W] = 'w', [SC_E] = 'e', [SC_R] = 'r', [SC_T] = 't',
    [SC_Y] = 'y', [SC_U] = 'u', [SC_I] = 'i', [SC_O] = 'o', [SC_P] = 'p',
    [SC_BRACKET_OPEN] = '[', [SC_BRACKET_CLOSE] = ']', [SC_ENTER] = '\r',
    [SC_A] = 'a', [SC_S] = 's', [SC_D] = 'd', [SC_F] = 'f', [SC_G] = 'g',
    [SC_H] = 'h', [SC_J] = 'j', [SC_K] = 'k', [SC_L] = 'l',
    [SC_SEMI_COLON] = ';', [SC_SINGLE_QUOTE] = '\'', [SC_BACKTICK] = '`', [SC_BACKSLASH] = '\\',
    [SC_Z] = 'z', [SC_X] = 'x', [SC_C] = 'c', [SC_V] = 'v', [SC_B] = 'b',
    [SC_N] = 'n', [SC_M] = 'm', [SC_COMMA] = ',', [SC_PERIOD] = '.', [SC_SLASH] = '/',
    [SC_SPACE] = ' ',
    [SC_KEYPAD_STAR] = '*', [SC_KEYPAD_MINUS] = '-', [SC_KEYPAD_PLUS] = '+', [SC_KEYPAD_PERIOD] = '.',
    [SC_KEYPAD_0] = '0', [SC_KEYPAD_1] = '1', [SC_KEYPAD_2] = '2', [SC_KEYPAD_3] = '3', [SC_KEYPAD_4] = '4',
    [SC_KEYPAD_5] = '5', [SC_KEYPAD_6] = '6', [SC_KEYPAD_7] = '7', [SC_KEYPAD_8] = '8', [SC_KEYPAD_9] = '9',
};

static const char keymap_shift[128] = {
    [SC_ESCAPE] = 0x1B,
    [SC_1] = '!', [SC_2] = '@', [SC_3] = '#', [SC_4] = '$', [SC_5] = '%',
    [SC_6] = '^', [SC_7] = '&', [SC_8] = '*', [SC_9] = '(', [SC_0] = ')',
    [SC_MINUS] = '_', [SC_EQUALS] = '+', [SC_BACKSPACE] = 0x7F, [SC_TAB] = '\t',
    [SC_Q] = 'Q', [SC_W] = 'W', [SC_E] = 'E', [SC_R] = 'R', [SC_T] = 'T',
    [SC_Y] = 'Y', [SC_U] = 'U', [SC_I] = 'I', [SC_O] = 'O', [SC_P] = 'P',
    [SC_BRACKET_OPEN] = '{', [SC_BRACKET_CLOSE] = '}', [SC_ENTER] = '\r',
    [SC_A] = 'A', [SC_S] = 'S', [SC_D] = 'D', [SC_F] = 'F', [SC_G] = 'G',
    [SC_H] = 'H', [SC_J] = 'J', [SC_K] = 'K', [SC_L] = 'L',
    [SC_SEMI_COLON] = ':', [SC_SINGLE_QUOTE] = '"', [SC_BACKTICK] = '~', [SC_BACKSLASH] = '|',
    [SC_Z] = 'Z', [SC_X] = 'X', [SC_C] = 'C', [SC_V] = 'V', [SC_B] = 'B',
    [SC_N] = 'N', [SC_M] = 'M', [SC_COMMA] = '<', [SC_PERIOD] = '>', [SC_SLASH] = '?',
    [SC_SPACE] = ' ',
    [SC_KEYPAD_STAR] = '*', [SC_KEYPAD_MINUS] = '-', [SC_KEYPAD_PLUS] = '+', [SC_KEYPAD_PERIOD] = '.',
    [SC_KEYPAD_0] = '0', [SC_KEYPAD_1] = '1', [SC_KEYPAD_2] = '2', [SC_KEYPAD_3] = '3', [SC_KEYPAD_4] = '4',
    [SC_KEYPAD_5] = '5', [SC_KEYPAD_6] = '6', [SC_KEYPAD_7] = '7', [SC_KEYPAD_8] = '8', [SC_KEYPAD_9] = '9',
};

static bool shift = false;
static bool ctrl = false;
static bool alt = false;
static bool caps = false;
static bool extended = false;

static int function_key(const uint8_t key)
{
    if (key >= SC_F1 && key <= SC_F10)
        return key - SC_F1 + 1;
    if (key == SC_F11)
        return 11;
    if (key == SC_F12)
        return 12;
    return 0;
}

static void switch_relative(const int delta)
{
    tty_switch((terminal_active() + delta + TERMINAL_COUNT) % TERMINAL_COUNT);
}

static void handle_extended(const uint8_t key)
{
    switch (key) {
    case SC_EXT_UP: tty_input_string("\x1b[A"); break;
    case SC_EXT_DOWN: tty_input_string("\x1b[B"); break;
    case SC_EXT_RIGHT:
        if (alt)
            switch_relative(1);
        else
            tty_input_string("\x1b[C");
        break;
    case SC_EXT_LEFT:
        if (alt)
            switch_relative(-1);
        else
            tty_input_string("\x1b[D");
        break;
    case SC_EXT_HOME: tty_input_string("\x1b[H"); break;
    case SC_EXT_END: tty_input_string("\x1b[F"); break;
    case SC_EXT_INSERT: tty_input_string("\x1b[2~"); break;
    case SC_EXT_DELETE: tty_input_string("\x1b[3~"); break;
    case SC_EXT_PAGE_UP: tty_input_string("\x1b[5~"); break;
    case SC_EXT_PAGE_DOWN: tty_input_string("\x1b[6~"); break;
    case SC_EXT_KEYPAD_ENTER: tty_input('\r'); break;
    case SC_EXT_KEYPAD_SLASH: tty_input('/'); break;
    default: break;
    }
}

void ps2_keyboard_interrupt_handler(uint8_t irq, void* ctx) {
    (void)irq;
    (void)ctx;
    const uint8_t scancode = port_read_u8(DATA_PORT);
    if (scancode == SC_ESCAPE_CODE) {
        extended = true;
        return;
    }

    const bool released = scancode & SC_RELEASE;
    const uint8_t key = scancode & ~SC_RELEASE;
    const bool was_extended = extended;
    extended = false;

    switch (key) {
    case SC_LSHIFT:
    case SC_RSHIFT:
        if (!was_extended)
            shift = !released;
        return;
    case SC_LCTRL:
        ctrl = !released;
        return;
    case SC_LALT:
        alt = !released;
        return;
    default:
        break;
    }

    if (released)
        return;

    if (was_extended) {
        handle_extended(key);
        return;
    }

    const int fn = function_key(key);
    if (fn) {
        if (alt && fn <= TERMINAL_COUNT)
            tty_switch(fn - 1);
        return;
    }
    if (key == SC_CAPSLOCK) {
        caps = !caps;
        return;
    }

    char c = shift ? keymap_shift[key] : keymap[key];
    if (c == '\0')
        return;
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (ctrl && ((c >= '@' && c <= '_') || (c >= 'a' && c <= 'z')))
        c &= 0x1F;
    if (alt)
        tty_input(0x1B);
    tty_input(c);
}
