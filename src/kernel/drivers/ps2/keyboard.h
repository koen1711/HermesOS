#ifndef PS2_KEYBOARD_H
#define PS2_KEYBOARD_H

#include <os/stdint.h>

#define PS2_KEYBOARD_IRQ 1

int ps2_keyboard_initialize();
void ps2_keyboard_interrupt_handler(uint8_t irq, void* ctx);

#endif //PS2_KEYBOARD_H
