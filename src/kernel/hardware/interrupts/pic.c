#include "pic.h"

#include "interrupts.h"

#include "drivers/keyboard/ps2_keyboard.h"

void handle_pic_interrupt(const struct InterruptContext* context)
{
    const uint8_t irq_number = context->interrupt_number;

    switch(irq_number)
    {
    case 0:
        break;
    case 1:
       ps2_keyboard_interrupt_handler();
        break;
    }

    pic_eoi(irq_number);
}

void pic_write(uint16_t port, uint8_t data)
{
    asm volatile(
        "outb %0, %1\n"
        "1: jmp 1f\n"
        "1: jmp 1f\n"
        "1:"
        :
        : "a" (data), "Nd"(port)
    );
}

uint8_t pic_read(uint16_t port)
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

void pic_remap(const uint8_t master_offset, const uint8_t slave_offset)
{
    // Restart the PICs
    pic_write(PIC_MASTER_COMMAND, 0x11);
    pic_write(PIC_SLAVE_COMMAND, 0x11);

    // Map PIC to interrupts 32-47
    pic_write(PIC_MASTER_DATA, master_offset);
    pic_write(PIC_SLAVE_DATA, slave_offset);

    pic_write(PIC_MASTER_DATA, 0x04);
    pic_write(PIC_SLAVE_DATA, 0x02);

    pic_write(PIC_MASTER_DATA, 0x01);
    pic_write(PIC_SLAVE_DATA, 0x01);

    pic_write(PIC_MASTER_DATA, 0x00);
    pic_write(PIC_SLAVE_DATA, 0x00);
}

void pic_eoi(const uint8_t irq)
{
    if (irq >= 8)
        pic_write(PIC_SLAVE_COMMAND, 0x20);
    pic_write(PIC_MASTER_COMMAND, 0x20);
}

