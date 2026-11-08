#include <hardware/interrupts/irq.h>
#include <hardware/interrupts/interrupts.h>
#include <hardware/smp/lapic.h>
#include <hardware/smp/smp.h>
#include <os/errno.h>
#include <utils/sync/spinlock.h>

#include "pic/pic.h"
#include "pic/ioapic.h"


#define IRQ_MAX_HANDLERS 4      // per line; PCI lines are often shared
#define IRQ_MAX_SOFTINTR 4

struct irq_action {
    irq_handler_t handler;
    void* ctx;
    bool mpsafe;
};

static struct irq_action actions[IRQ_MAX_LINES][IRQ_MAX_HANDLERS];
static uint32_t running[IRQ_MAX_LINES];
static spinlock_t actions_lock = { .name = "irq_actions" };
static softintr_handler_t softintrs[IRQ_MAX_SOFTINTR];

extern void irq_interrupt_0(), irq_interrupt_1(), irq_interrupt_2(), irq_interrupt_3(),
            irq_interrupt_4(), irq_interrupt_5(), irq_interrupt_6(), irq_interrupt_7(),
            irq_interrupt_8(), irq_interrupt_9(), irq_interrupt_10(), irq_interrupt_11(),
            irq_interrupt_12(), irq_interrupt_13(), irq_interrupt_14(), irq_interrupt_15(),
            irq_interrupt_16(), irq_interrupt_17(), irq_interrupt_18(), irq_interrupt_19(),
            irq_interrupt_20(), irq_interrupt_21(), irq_interrupt_22(), irq_interrupt_23();

static void (*const stubs[IRQ_MAX_LINES])() = {
    irq_interrupt_0, irq_interrupt_1, irq_interrupt_2, irq_interrupt_3,
    irq_interrupt_4, irq_interrupt_5, irq_interrupt_6, irq_interrupt_7,
    irq_interrupt_8, irq_interrupt_9, irq_interrupt_10, irq_interrupt_11,
    irq_interrupt_12, irq_interrupt_13, irq_interrupt_14, irq_interrupt_15,
    irq_interrupt_16, irq_interrupt_17, irq_interrupt_18, irq_interrupt_19,
    irq_interrupt_20, irq_interrupt_21, irq_interrupt_22, irq_interrupt_23,
};

static bool use_ioapic;

static uint32_t irq_dest(const uint8_t irq)
{
    const int n = cpu_count();
    if (irq == 0 || n <= 1)
        return cpu_get(0)->apic_id;
    return cpu_get(1 + irq % (n - 1))->apic_id;
}

static bool msi_used[IRQ_MAX_LINES];
static irq_msi_hook_t msi_hooks[IRQ_MAX_MSI_HOOKS];
static bool msi_ready;

static void irq_line_unmask(const uint8_t irq)
{
    if (irq >= IRQ_ISA_LINES)
        return;
    if (use_ioapic) {
        ioapic_route(irq, IRQ_OFFSET + irq, irq_dest(irq));
        ioapic_unmask(irq);
    } else {
        pic_unmask(irq);
    }
}

static void irq_line_mask(const uint8_t irq)
{
    if (irq >= IRQ_ISA_LINES)
        return;
    if (use_ioapic)
        ioapic_mask(irq);
    else if (irq != 0 && irq != 1 && irq != 2)
        pic_mask(irq);
}

bool irq_ioapic_active()
{
    return use_ioapic;
}

bool irq_switch_to_ioapic()
{
    uint64_t flags;
    spinlock_acquire_irqsave(&actions_lock, &flags);
    if (use_ioapic || !lapic_present() || !ioapic_initialize()) {
        spinlock_release_irqrestore(&actions_lock, flags);
        return use_ioapic;
    }
    pic_disable();
    use_ioapic = true;
    for (uint8_t irq = 0; irq < IRQ_ISA_LINES; irq++) {
        for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
            if (actions[irq][i].handler) {
                irq_line_unmask(irq);
                break;
            }
        }
    }
    spinlock_release_irqrestore(&actions_lock, flags);
    return true;
}

int irq_msi_alloc()
{
    if (!use_ioapic)
        return -ENODEV;
    uint64_t flags;
    spinlock_acquire_irqsave(&actions_lock, &flags);
    for (uint8_t irq = IRQ_ISA_LINES; irq < IRQ_MAX_LINES; irq++) {
        if (!msi_used[irq]) {
            msi_used[irq] = true;
            spinlock_release_irqrestore(&actions_lock, flags);
            return irq;
        }
    }
    spinlock_release_irqrestore(&actions_lock, flags);
    return -ENOSPC;
}

int irq_on_msi_ready(const irq_msi_hook_t hook)
{
    if (__atomic_load_n(&msi_ready, __ATOMIC_ACQUIRE)) {
        hook();
        return 0;
    }
    for (int i = 0; i < IRQ_MAX_MSI_HOOKS; i++) {
        irq_msi_hook_t expected = nullptr;
        if (__atomic_compare_exchange_n(&msi_hooks[i], &expected, hook, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return 0;
    }
    return -ENOSPC;
}

void irq_msi_ready()
{
    if (!use_ioapic || __atomic_exchange_n(&msi_ready, true, __ATOMIC_ACQ_REL))
        return;
    for (int i = 0; i < IRQ_MAX_MSI_HOOKS; i++) {
        const irq_msi_hook_t hook = __atomic_exchange_n(&msi_hooks[i], nullptr, __ATOMIC_ACQUIRE);
        if (hook)
            hook();
    }
}

void irq_msi_free(const uint8_t irq)
{
    if (irq >= IRQ_ISA_LINES && irq < IRQ_MAX_LINES)
        __atomic_store_n(&msi_used[irq], false, __ATOMIC_RELEASE);
}

void irq_msi_message(const uint8_t irq, uint64_t* address, uint32_t* data)
{
    *address = 0xFEE00000ull | (uint64_t)(irq_dest(irq) & 0xFF) << 12;
    *data = IRQ_OFFSET + irq;
}

void irq_eoi(const uint8_t irq)
{
    if (use_ioapic || irq >= IRQ_ISA_LINES)
        lapic_eoi();
    else
        pic_eoi(irq);
}

static int irq_register(const uint8_t irq, const irq_handler_t handler, void* ctx, const bool mpsafe)
{
    if (irq >= IRQ_MAX_LINES || !handler)
        return -EINVAL;

    uint64_t flags;
    spinlock_acquire_irqsave(&actions_lock, &flags);
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        if (!actions[irq][i].handler) {

            actions[irq][i].ctx = ctx;
            actions[irq][i].mpsafe = mpsafe;
            __atomic_store_n(&actions[irq][i].handler, handler, __ATOMIC_RELEASE);

            idt_set_entry(IRQ_OFFSET + irq, (uintptr_t)stubs[irq]);
            irq_line_unmask(irq);
            spinlock_release_irqrestore(&actions_lock, flags);
            return 0;
        }
    }
    spinlock_release_irqrestore(&actions_lock, flags);
    return -ENOSPC;
}

int irq_register_handler(const uint8_t irq, const irq_handler_t handler, void* ctx)
{
    return irq_register(irq, handler, ctx, false);
}

int irq_register_handler_mpsafe(const uint8_t irq, const irq_handler_t handler, void* ctx)
{
    return irq_register(irq, handler, ctx, true);
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
    if (!in_use)
        irq_line_mask(irq);

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
    return cpu_this()->irq_depth != 0;
}

void irq_dispatch(const uint8_t irq)
{
    if (irq >= IRQ_MAX_LINES)
        return;

    struct cpu* cpu = cpu_this();
    cpu->irq_depth++;
    __atomic_add_fetch(&running[irq], 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        const irq_handler_t handler = __atomic_load_n(&actions[irq][i].handler, __ATOMIC_ACQUIRE);
        if (handler) {
            const bool locked = !actions[irq][i].mpsafe && kernel_lock_enter();
            handler(irq, actions[irq][i].ctx);
            if (locked)
                kernel_lock_exit();
        }
    }
    __atomic_sub_fetch(&running[irq], 1, __ATOMIC_SEQ_CST);
    cpu->irq_depth--;
}

void irq_softintr()
{
    struct cpu* cpu = cpu_this();
    if (cpu->irq_depth)
        return;
    cpu->irq_depth++;
    for (int i = 0; i < IRQ_MAX_SOFTINTR; i++)
        if (softintrs[i])
            softintrs[i]();
    cpu->irq_depth--;
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
