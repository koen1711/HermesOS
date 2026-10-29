#ifndef OS_BOCHS_H
#define OS_BOCHS_H
#include "hardware/port/pci.h"
#include "drivers/video/framebuffer.h"

int bochs_detect(pci_device *dev);
int bochs_init(const pci_device *dev, framebuffer_t *fb);

#endif //OS_BOCHS_H