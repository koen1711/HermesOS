#include "interrupts.h"

#include <stdbool.h>
#include <hardware/interrupts/pic.h>

extern void irq_interrupt_timer();
extern void irq_interrupt_keyboard();

idt_entry idt[IDT_SIZE];



void idt_set_entry(const uint8_t index, const uint64_t handler)
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
    unsigned long flags;
    asm volatile ( "pushf\n\t"
                   "pop %0"
                   : "=g"(flags) );
    return flags & (1 << 9);
}

void irq_restore(unsigned long flags)
{
    asm ("push %0\n\tpopf" : : "rm"(flags) : "memory","cc");
}


void idt_initialize()
{

    // Create a proper IDT descriptor structure
    struct {
        uint16_t size;
        uint64_t address;
    } __attribute__((packed)) idtr;

    // Set the size (limit) of the IDT
    idtr.size = IDT_SIZE * IDT_ENTRY_SIZE - 1;

    // Set the base address of the IDT
    idtr.address = (uint64_t)idt;

    // Set up interrupt handler for interrupt 1
    uintptr_t handler_address = (uintptr_t)handle_interrupt;

    uintptr_t irq_timer_handler_address = (uintptr_t)irq_interrupt_timer;
    uintptr_t irq_keyboard_handler_address = (uintptr_t)irq_interrupt_keyboard;

    // for (int i = 0; i < IRQ_OFFSET; i++)
    //     idt_set_entry(i, handler_address);
    idt_set_entry(IRQ_OFFSET, (uint64_t)irq_timer_handler_address);
    idt_set_entry(IRQ_OFFSET + 1, (uint64_t)irq_keyboard_handler_address);



    // Remap the PIC to interrupts 32-47
    pic_remap(IRQ_OFFSET, IRQ_OFFSET + 8);
    irq_restore(0x200);

    // Load the IDT register with our descriptor
    asm("lidt %0" : : "m"(idtr));
}