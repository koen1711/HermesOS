#include "LFB.h"

#include <os/stddef.h>
#include "hardware/terminal/stdio.h"

static video_driver_t *active_driver = NULL;
static pci_address active_gpu;

int lfb_initialize(void) {
    printf("Initializing LFB...\n");

    const scan_result *result = pci_scan(0x03);
    if (result == NULL) return -1;

    printf("Found %d display devices.\n", result->count);

    for (int i = 0; i < result->count; i++)
    {
        printf("Checking device %d/%d...\n", i + 1, result->count);
        const pci_address dev = pci_convert_address(result->devices[i]);

        for (size_t j = 0; j < NUM_VIDEO_DRIVERS; j++)
        {
            pci_device* device = NULL;
            if (pci_get_device(dev.bus, dev.device, dev.function, device) != 0)
                continue;

            if (video_drivers[j].detect(device))
            {
                active_driver = &video_drivers[j];
                active_gpu = dev;
                return video_drivers[j].init(device);
            }
        }
    }

    return -1; // no supported GPU found
}