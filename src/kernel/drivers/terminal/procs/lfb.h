#ifndef OS_LFB_TEXT_H
#define OS_LFB_TEXT_H

#include <os/stdbool.h>
#include <os/stdint.h>
#include <os/stddef.h>

#include "drivers/terminal/terminal.h"



void lfb_text_initialize(terminal_contents* term);
void lfb_text_update();


#endif //OS_LFB_TEXT_H