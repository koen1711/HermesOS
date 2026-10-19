#include <hardware/interrupts/irq.h>
#include <hardware/interrupts/interrupts.h>
#include <hardware/interrupts/pic.h>
#include <os/errno.h>


#define IRQ_MAX_HANDLERS 4      // per line; PCI lines are often shared

struct irq_action {
    irq_handler_t handler;
    void* ctx;
};

static struct irq_action actions[IRQ_MAX_LINES][IRQ_MAX_HANDLERS];

// one entry stub per PIC line, in interrupt.asm
extern void irq_interrupt_0(), irq_interrupt_1(), irq_interrupt_2(), irq_interrupt_3(),
            irq_interrupt_4(), irq_interrupt_5(), irq_interrupt_6(), irq_interrupt_7(),
            irq_interrupt_8(), irq_interrupt_9(), irq_interrupt_10(), irq_interrupt_11(),
            irq_interrupt_12(), irq_interrupt_13(), irq_interrupt_14(), irq_interrupt_15();

static void (*const stubs[IRQ_MAX_LINES])() = {
    irq_interrupt_0, irq_interrupt_1, irq_interrupt_2, irq_interrupt_3,
    irq_interrupt_4, irq_interrupt_5, irq_interrupt_6, irq_interrupt_7,
    irq_interrupt_8, irq_interrupt_9, irq_interrupt_10, irq_interrupt_11,
    irq_interrupt_12, irq_interrupt_13, irq_interrupt_14, irq_interrupt_15,
};

int irq_register_handler(const uint8_t irq, const irq_handler_t handler, void* ctx)
{
    if (irq >= IRQ_MAX_LINES || !handler)
        return -EINVAL;

    const uint64_t flags = irq_save();
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        if (!actions[irq][i].handler) {
            actions[irq][i].handler = handler;
            actions[irq][i].ctx = ctx;
            // vector first, then let the line through
            idt_set_entry(IRQ_OFFSET + irq, (uintptr_t)stubs[irq]);
            pic_unmask(irq);
            irq_restore(flags);
            return 0;
        }
    }
    irq_restore(flags);
    return -ENOSPC;
}

void irq_unregister_handler(const uint8_t irq, const irq_handler_t handler)
{
    if (irq >= IRQ_MAX_LINES)
        return;

    const uint64_t flags = irq_save();
    bool in_use = false;
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        if (actions[irq][i].handler == handler) {
            actions[irq][i].handler = nullptr;
            actions[irq][i].ctx = nullptr;
        }
        in_use |= actions[irq][i].handler != nullptr;
    }
    if (!in_use && irq != 0 && irq != 1 && irq != 2)
        pic_mask(irq);
    irq_restore(flags);
}

void irq_dispatch(const uint8_t irq)
{
    if (irq >= IRQ_MAX_LINES)
        return;

    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        const struct irq_action action = actions[irq][i];
        if (action.handler)
            action.handler(irq, action.ctx);
    }
}

uint64_t irq_save()
{
    uint64_t flags;
    asm volatile("pushfq\n\tpopq %0\n\tcli" : "=rm"(flags) : : "memory");
    return flags;
}

void irq_restore(uint64_t flags)
{
    asm volatile("pushq %0\n\tpopfq" : : "rm"(flags) : "memory", "cc");
}
