#ifndef OS_BOCHS_H
#define OS_BOCHS_H
#include "hardware/port/pci.h"

int bochs_detect(pci_device *dev);
int bochs_init(pci_device *dev);

#endif //OS_BOCHS_H