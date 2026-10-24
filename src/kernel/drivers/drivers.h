#ifndef DRIVERS_H
#define DRIVERS_H

#include <drivers/ps2/keyboard.h>
#include <drivers/timer/rtc.h>
#include <drivers/timer/pit.h>
#include <drivers/syscalls/syscalls.h>

#include "video/LFB.h"

static void register_drivers()
{
    syscalls_initialize();
    pit_initialize(PIT_DEFAULT_HZ);
    ps2_keyboard_initialize();
    rtc_initialize();
    lfb_initialize();
}

#endif //DRIVERS_H
