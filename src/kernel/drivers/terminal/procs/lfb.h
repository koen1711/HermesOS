#ifndef OS_LFB_TEXT_H
#define OS_LFB_TEXT_H

#include <os/stdbool.h>
#include <os/stdint.h>
#include <os/stddef.h>

#include "drivers/terminal/terminal.h"

const terminal_backend* lfb_text_backend();

#endif //OS_LFB_TEXT_H
