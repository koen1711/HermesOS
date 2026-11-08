#ifndef PIC_H
#define PIC_H

#include <os/stdint.h>

#define PIC_MASTER_COMMAND 0x20
#define PIC_MASTER_DATA 0x21
#define PIC_SLAVE_COMMAND 0xA0
#define PIC_SLAVE_DATA 0xA1

static void pic_write(uint16_t port, uint8_t data)
{
    asm volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

static uint8_t pic_read(uint16_t port)
{
    uint8_t data;
    asm volatile(
        "inb %1, %0\n"
        "1: jmp 1f\n"
        "1: jmp 1f\n"
        "1:"
        : "=a"(data)
        : "Nd"(port));
    return data;
}

void pic_remap(uint8_t master_offset, uint8_t slave_offset);
void pic_eoi(uint8_t irq);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_disable();

#endif //PIC_H
