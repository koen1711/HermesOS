#ifndef OS_LFB_H
#define OS_LFB_H
#include "bochs/bochs.h"
#include "hardware/port/pci.h"

typedef struct {
    const char *name;

    int (*detect)(pci_device *dev);
    int (*init)(const pci_device *dev);

} video_driver_t;

static video_driver_t video_drivers[] = {
    { "Bochs/QEMU stdvga", bochs_detect, bochs_init },
};

#define NUM_VIDEO_DRIVERS (sizeof(video_drivers) / sizeof(video_drivers[0]))

int lfb_initialize();

#endif //OS_LFB_H