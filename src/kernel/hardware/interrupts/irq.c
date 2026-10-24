#include <hardware/interrupts/irq.h>
#include <hardware/interrupts/interrupts.h>
#include <hardware/interrupts/pic.h>
#include <os/errno.h>
#include <utils/sync/spinlock.h>


#define IRQ_MAX_HANDLERS 4      // per line; PCI lines are often shared
#define IRQ_MAX_SOFTINTR 4

struct irq_action {
    irq_handler_t handler;
    void* ctx;
};

static struct irq_action actions[IRQ_MAX_LINES][IRQ_MAX_HANDLERS];
static uint32_t running[IRQ_MAX_LINES];
static spinlock_t actions_lock = { .name = "irq_actions" };
static softintr_handler_t softintrs[IRQ_MAX_SOFTINTR];
static uint32_t irq_depth;

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

    uint64_t flags;
    spinlock_acquire_irqsave(&actions_lock, &flags);
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        if (!actions[irq][i].handler) {

            actions[irq][i].ctx = ctx;
            __atomic_store_n(&actions[irq][i].handler, handler, __ATOMIC_RELEASE);

            idt_set_entry(IRQ_OFFSET + irq, (uintptr_t)stubs[irq]);
            pic_unmask(irq);
            spinlock_release_irqrestore(&actions_lock, flags);
            return 0;
        }
    }
    spinlock_release_irqrestore(&actions_lock, flags);
    return -ENOSPC;
}

void irq_unregister_handler(const uint8_t irq, const irq_handler_t handler, const void* ctx)
{
    if (irq >= IRQ_MAX_LINES)
        return;

    uint64_t flags;
    spinlock_acquire_irqsave(&actions_lock, &flags);
    bool in_use = false;
    bool removed = false;
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        if (!removed && actions[irq][i].handler == handler && actions[irq][i].ctx == ctx) {
            __atomic_store_n(&actions[irq][i].handler, nullptr, __ATOMIC_SEQ_CST);
            removed = true;
        }
        in_use |= actions[irq][i].handler != nullptr;
    }
    if (!in_use && irq != 0 && irq != 1 && irq != 2)
        pic_mask(irq);

    if (removed && !irq_in_interrupt()) {
        while (__atomic_load_n(&running[irq], __ATOMIC_SEQ_CST))
            asm volatile("pause");
    }
    spinlock_release_irqrestore(&actions_lock, flags);
}

int irq_register_softintr(const softintr_handler_t handler)
{
    if (!handler)
        return -EINVAL;
    const uint64_t flags = irq_save();
    for (int i = 0; i < IRQ_MAX_SOFTINTR; i++) {
        if (softintrs[i] == handler || !softintrs[i]) {
            softintrs[i] = handler;
            irq_restore(flags);
            return 0;
        }
    }
    irq_restore(flags);
    return -ENOSPC;
}

bool irq_in_interrupt()
{
    return irq_depth != 0;
}

void irq_dispatch(const uint8_t irq)
{
    if (irq >= IRQ_MAX_LINES)
        return;

    irq_depth++;
    __atomic_add_fetch(&running[irq], 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        const irq_handler_t handler = __atomic_load_n(&actions[irq][i].handler, __ATOMIC_ACQUIRE);
        if (handler)
            handler(irq, actions[irq][i].ctx);
    }
    __atomic_sub_fetch(&running[irq], 1, __ATOMIC_SEQ_CST);
    if (irq_depth == 1) {
        for (int i = 0; i < IRQ_MAX_SOFTINTR; i++)
            if (softintrs[i])
                softintrs[i]();
    }
    irq_depth--;
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
