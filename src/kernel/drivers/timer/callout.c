#include <drivers/timer/callout.h>

#include <hardware/interrupts/irq.h>
#include <hardware/timer/pit.h>
#include <hardware/timer/tsc.h>
#include <hardware/smp/smp.h>
#include <utils/sync/spinlock.h>

#define WHEEL_SIZE 256

struct callout_wheel {
    spinlock_t lock;
    uint64_t done;
    struct callout* running;
    struct dlist slots[WHEEL_SIZE];
} __attribute__((aligned(64)));

static struct callout_wheel wheels[MAX_CPUS];
static volatile uint64_t ticks;

void callout_initialize()
{
    for (int c = 0; c < MAX_CPUS; c++) {
        for (int i = 0; i < WHEEL_SIZE; i++)
            dlist_init(&wheels[c].slots[i]);
        spinlock_init(&wheels[c].lock, "callout");
        wheels[c].done = 0;
        wheels[c].running = nullptr;
    }
    ticks = 0;
}

void callout_init(struct callout* c)
{
    c->link.prev = c->link.next = nullptr;
    c->expires = 0;
    c->func = nullptr;
    c->arg = nullptr;
    c->pending = false;
    c->mpsafe = false;
    c->cpu = -1;
    c->dispatched = 0;
}

static void callout_unlink(struct callout* c)
{
    dlist_remove(&wheels[c->cpu].slots[c->expires % WHEEL_SIZE], &c->link);
    __atomic_store_n(&c->pending, false, __ATOMIC_RELEASE);
}

static struct callout_wheel* callout_lock_wheel(const struct callout* c)
{
    for (;;) {
        const int cpu = __atomic_load_n(&c->cpu, __ATOMIC_ACQUIRE);
        if (cpu < 0)
            return nullptr;
        struct callout_wheel* w = &wheels[cpu];
        spinlock_acquire(&w->lock);
        if (__atomic_load_n(&c->cpu, __ATOMIC_RELAXED) == cpu)
            return w;
        spinlock_release(&w->lock);
    }
}

void callout_reset(struct callout* c, uint64_t delay, const callout_func_t func, void* arg)
{
    if (delay == 0)
        delay = 1;

    const uint64_t flags = irq_save();
    const int self = cpu_this()->index;
    struct callout_wheel* mine = &wheels[self];
    struct callout_wheel* old;
    for (;;) {
        const int cpu = __atomic_load_n(&c->cpu, __ATOMIC_ACQUIRE);
        old = cpu < 0 || cpu == self ? nullptr : &wheels[cpu];
        if (old && cpu < self)
            spinlock_acquire(&old->lock);
        spinlock_acquire(&mine->lock);
        if (old && cpu > self)
            spinlock_acquire(&old->lock);
        if (__atomic_load_n(&c->cpu, __ATOMIC_RELAXED) == cpu)
            break;
        if (old)
            spinlock_release(&old->lock);
        spinlock_release(&mine->lock);
    }

    if (c->pending)
        callout_unlink(c);
    c->expires = ticks + delay;
    c->func = func;
    c->arg = arg;
    __atomic_store_n(&c->cpu, self, __ATOMIC_RELEASE);
    dlist_insert_tail(&mine->slots[c->expires % WHEEL_SIZE], &c->link);
    __atomic_store_n(&c->pending, true, __ATOMIC_RELEASE);

    if (old)
        spinlock_release(&old->lock);
    spinlock_release(&mine->lock);
    irq_restore(flags);
}

bool callout_stop(struct callout* c)
{
    if (!__atomic_load_n(&c->pending, __ATOMIC_ACQUIRE))
        return false;
    const uint64_t flags = irq_save();
    struct callout_wheel* w = callout_lock_wheel(c);
    const bool was = w && c->pending;
    if (was)
        callout_unlink(c);
    if (w)
        spinlock_release(&w->lock);
    irq_restore(flags);
    return was;
}

void callout_drain(struct callout* c)
{
    const int self = cpu_this()->index;
    do {
        callout_stop(c);
        for (int i = 0; i < MAX_CPUS; i++) {
            if (i == self)
                continue;
            while (__atomic_load_n(&wheels[i].running, __ATOMIC_ACQUIRE) == c) {
                smp_spin_poll();
                asm volatile("pause");
            }
        }
    } while (callout_pending(c));
}

bool callout_pending(const struct callout* c)
{
    return __atomic_load_n(&c->pending, __ATOMIC_ACQUIRE);
}

uint32_t callout_dispatches(const struct callout* c)
{
    return __atomic_load_n(&c->dispatched, __ATOMIC_ACQUIRE);
}

static struct callout* wheel_due(struct callout_wheel* w, const uint64_t now)
{
    uint64_t from = w->done + 1;
    if (now - w->done > WHEEL_SIZE)
        from = now - WHEEL_SIZE + 1;
    for (uint64_t t = from; t <= now; t++) {
        const struct dlist* slot = &w->slots[t % WHEEL_SIZE];
        for (struct dlist_node* n = dlist_first(slot); n; n = dlist_next(n)) {
            const auto c = dlist_entry(n, struct callout, link);
            if (c->expires <= now)
                return c;
        }
        w->done = t;
    }
    return nullptr;
}

static void wheel_run(struct callout_wheel* w, const uint64_t now)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&w->lock, &flags);
    bool big_locks = false, kernel_locked = false, net_locked = false;

    for (;;) {
        struct callout* due = wheel_due(w, now);
        if (!due)
            break;

        if (!due->mpsafe && !big_locks) {
            spinlock_release_irqrestore(&w->lock, flags);
            kernel_locked = kernel_lock_enter();
            net_locked = net_lock_enter();
            big_locks = true;
            spinlock_acquire_irqsave(&w->lock, &flags);
            continue;
        }

        callout_unlink(due);
        __atomic_add_fetch(&due->dispatched, 1, __ATOMIC_RELEASE);
        const callout_func_t func = due->func;
        void* arg = due->arg;
        struct callout* outer = w->running;
        __atomic_store_n(&w->running, due, __ATOMIC_RELEASE);
        spinlock_release_irqrestore(&w->lock, flags);
        func(arg);
        __atomic_store_n(&w->running, outer, __ATOMIC_RELEASE);
        spinlock_acquire_irqsave(&w->lock, &flags);
    }
    spinlock_release_irqrestore(&w->lock, flags);
    if (net_locked)
        net_lock_exit();
    if (kernel_locked)
        kernel_lock_exit();
}

void callout_tick()
{
    const uint64_t now = __atomic_add_fetch(&ticks, 1, __ATOMIC_ACQ_REL);
    wheel_run(&wheels[cpu_this()->index], now);
}

void callout_tick_local()
{
    struct callout_wheel* w = &wheels[cpu_this()->index];
    const uint64_t now = __atomic_load_n(&ticks, __ATOMIC_ACQUIRE);
    if (w->done < now)
        wheel_run(w, now);
}

void callout_poll()
{
    static uint64_t next_us;
    static bool running;
    if (pit_frequency() || running)
        return;
    running = true;
    const uint64_t now = tsc_micros();
    const uint64_t step = 1000000 / ticks_hz();
    if (!next_us)
        next_us = now + step;
    while (now >= next_us) {
        callout_tick();
        next_us += step;
    }
    running = false;
}

uint64_t ticks_get()
{
    return ticks;
}

uint32_t ticks_hz()
{
    const uint32_t hz = pit_frequency();
    return hz ? hz : PIT_DEFAULT_HZ;
}

uint64_t ticks_from_ms(const uint64_t ms)
{
    const uint64_t t = ms * ticks_hz() / 1000;
    return t ? t : 1;
}
