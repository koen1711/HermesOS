#include <drivers/timer/callout.h>

#include <drivers/timer/pit.h>
#include <drivers/timer/tsc.h>
#include <utils/sync/spinlock.h>

#define WHEEL_SIZE 256

static struct dlist wheel[WHEEL_SIZE];
static spinlock_t callout_lock;
static volatile uint64_t ticks;

void callout_initialize()
{
    for (int i = 0; i < WHEEL_SIZE; i++)
        dlist_init(&wheel[i]);
    spinlock_init(&callout_lock, "callout");
    ticks = 0;
}

void callout_init(struct callout* c)
{
    c->link.prev = c->link.next = nullptr;
    c->expires = 0;
    c->func = nullptr;
    c->arg = nullptr;
    c->pending = false;
}

static void callout_unlink(struct callout* c)
{
    dlist_remove(&wheel[c->expires % WHEEL_SIZE], &c->link);
    c->pending = false;
}

void callout_reset(struct callout* c, uint64_t delay, const callout_func_t func, void* arg)
{
    if (delay == 0)
        delay = 1;

    uint64_t flags;
    spinlock_acquire_irqsave(&callout_lock, &flags);
    if (c->pending)
        callout_unlink(c);
    c->expires = ticks + delay;
    c->func = func;
    c->arg = arg;
    c->pending = true;
    dlist_insert_tail(&wheel[c->expires % WHEEL_SIZE], &c->link);
    spinlock_release_irqrestore(&callout_lock, flags);
}

bool callout_stop(struct callout* c)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&callout_lock, &flags);
    const bool was = c->pending;
    if (was)
        callout_unlink(c);
    spinlock_release_irqrestore(&callout_lock, flags);
    return was;
}

bool callout_pending(const struct callout* c)
{
    return c->pending;
}

void callout_tick()
{
    uint64_t flags;
    spinlock_acquire_irqsave(&callout_lock, &flags);
    const uint64_t now = ++ticks;
    const struct dlist* slot = &wheel[now % WHEEL_SIZE];

    for (;;) {
        struct callout* due = nullptr;
        for (struct dlist_node* n = dlist_first(slot); n; n = dlist_next(n)) {
            const auto c = dlist_entry(n, struct callout, link);
            if (c->expires <= now) {
                due = c;
                break;
            }
        }
        if (!due)
            break;

        callout_unlink(due);
        const callout_func_t func = due->func;
        void* arg = due->arg;
        spinlock_release_irqrestore(&callout_lock, flags);
        func(arg);
        spinlock_acquire_irqsave(&callout_lock, &flags);
    }
    spinlock_release_irqrestore(&callout_lock, flags);
}

void callout_poll()
{
    static uint64_t next_us;
    static bool running;
    if (pit_frequency() || running)
        return;
    running = true;     // a callout may itself wait and poll
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
