#ifndef DRIVERS_TIMER_TSC_H
#define DRIVERS_TIMER_TSC_H

#include <os/stdint.h>

void tsc_initialize();
uint64_t tsc_read();

uint64_t tsc_micros();

#endif //DRIVERS_TIMER_TSC_H
