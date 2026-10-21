#ifndef PIT_H
#define PIT_H

#include <os/stdint.h>

#define PIT_FREQUENCY 100


void pit_initialize(void);


uint64_t pit_ticks(void);

void handle_timer_interrupt();

#endif //PIT_H
