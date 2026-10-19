#ifndef DRIVERS_TIMER_CALLOUT_H
#define DRIVERS_TIMER_CALLOUT_H

#include <os/stdint.h>
#include <utils/queue/queue.h>

typedef void (*callout_func_t)(void* arg);

struct callout {
    struct dlist_node link;
    uint64_t expires;
    callout_func_t func;
    void* arg;
    bool pending;
};

void callout_initialize();
void callout_init(struct callout* c);

void callout_reset(struct callout* c, uint64_t delay, callout_func_t func, void* arg);
bool callout_stop(struct callout* c);
bool callout_pending(const struct callout* c);

void callout_tick();

uint64_t ticks_get();
uint32_t ticks_hz();
uint64_t ticks_from_ms(uint64_t ms);

#endif //DRIVERS_TIMER_CALLOUT_H
