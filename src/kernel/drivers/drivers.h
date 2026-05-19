#ifndef DRIVERS_H
#define DRIVERS_H

#include <drivers/ps2/keyboard.h>
#include <drivers/timer/rtc.h>

#include "video/LFB.h"

static void register_drivers()
{
    // Register all drivers here
    ps2_keyboard_initialize();
    rtc_initialize();
    lfb_initialize();
}

#endif //DRIVERS_H
