#ifndef DRIVERS_H
#define DRIVERS_H

#include <drivers/keyboard/ps2_keyboard.h>
#include <drivers/timer/rtc.h>

static void register_drivers()
{
    // Register all drivers here
    ps2_keyboard_initialize();
    rtc_initialize();
}

#endif //DRIVERS_H
