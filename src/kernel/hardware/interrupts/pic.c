#include "pic.h"

#include <os/stdbool.h>

#include "interrupts.h"
#include "irq.h"

#define PIC_READ_ISR 0x0B

static bool pic_is_spurious(const uint8_t irq)
{
    if (irq == 7) {
        pic_write(PIC_MASTER_COMMAND, PIC_READ_ISR);
        return !(pic_read(PIC_MASTER_COMMAND) & 0x80);
    }
    if (irq == 15) {
        pic_write(PIC_SLAVE_COMMAND, PIC_READ_ISR);
        if (!(pic_read(PIC_SLAVE_COMMAND) & 0x80)) {
            pic_write(PIC_MASTER_COMMAND, 0x20);
            return true;
        }
    }
    return false;
}

void handle_pic_interrupt(const interrupt_context* context)
{
    const uint8_t irq_number = context->interrupt_number - IRQ_OFFSET;

    if (pic_is_spurious(irq_number))
        return;

    irq_dispatch(irq_number);

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

static void pic_write_masks();

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

    pic_write_masks();
}

static uint16_t masks = 0xFFFB;

static void pic_write_masks()
{
    pic_write(PIC_MASTER_DATA, masks & 0xFF);
    pic_write(PIC_SLAVE_DATA, masks >> 8);
}

void pic_mask(const uint8_t irq)
{
    if (irq == 2)
        return;
    masks |= 1 << irq;
    pic_write_masks();
}

void pic_unmask(const uint8_t irq)
{
    masks &= ~(1 << irq);
    pic_write_masks();
}

void pic_eoi(const uint8_t irq)
{
    if (irq >= 8)
        pic_write(PIC_SLAVE_COMMAND, 0x20);
    pic_write(PIC_MASTER_COMMAND, 0x20);
}

