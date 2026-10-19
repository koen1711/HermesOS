#include "interrupts.h"

#include <os/stdint.h>
#include <hardware/interrupts/pic.h>

#include <hardware/interrupts/irq.h>

idt_entry idt[IDT_SIZE];

void idt_set_entry(const uint8_t index, const uintptr_t handler)
{
    idt_entry entry;
    entry.offset_low = handler & 0xFFFF;
    entry.selector = 0x08;
    entry.ist = 0;
    entry.type_attr = 0x8E;
    entry.offset_mid = (handler >> 16) & 0xFFFF;
    entry.offset_high = (handler >> 32) & 0xFFFFFFFF;
    entry.zero = 0;

    idt[index] = entry;
}

void handle_interrupt(struct interrupt_context* context)
{
    // vga_text_write_string("Interrupt handledA");
}



bool are_interrupts_enabled()
{
    uint64_t flags;
    asm volatile ( "pushfq\n\t"
                   "popq %0"
                   : "=g"(flags) );
    return flags & (1 << 9);
}


void idt_initialize()
{

    struct {
        uint16_t size;
        uint64_t address;
    } __attribute__((packed)) idtr;

    idtr.size = IDT_SIZE * IDT_ENTRY_SIZE - 1;
    idtr.address = (uint64_t)idt;

    pic_remap(IRQ_OFFSET, IRQ_OFFSET + 8);

    asm volatile("lidt %0" : : "m"(idtr));
    irq_restore(0x200);
}