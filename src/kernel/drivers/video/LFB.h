#ifndef OS_LFB_H
#define OS_LFB_H
#include "bochs/bochs.h"
#include "hardware/port/pci.h"

typedef struct {
    const char *name;

    int (*detect)(pci_device *dev);   // returns 1 if this driver owns the device
    int (*init)(pci_device *dev);     // sets up the framebuffer

} video_driver_t;

static video_driver_t video_drivers[] = {
    { "Bochs/QEMU stdvga", bochs_detect, bochs_init },
};

#define NUM_VIDEO_DRIVERS (sizeof(video_drivers) / sizeof(video_drivers[0]))

// Initializes the Linear Frame Buffer
int lfb_initialize();

#endif //OS_LFB_H