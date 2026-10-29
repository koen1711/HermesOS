#include "LFB.h"

#include <os/stddef.h>
#include "drivers/terminal/terminal.h"


static video_driver_t *active_driver = nullptr;
static pci_address active_gpu;
static framebuffer_t framebuffer;
static bool framebuffer_ready = false;

const framebuffer_t* lfb_framebuffer() {
    return framebuffer_ready ? &framebuffer : nullptr;
}

int lfb_initialize(void) {
    if (framebuffer_ready) return 0;

    terminal_printf("Initializing LFB...\n");

    const scan_result *result = pci_scan(0x03);
    if (result == nullptr) return -1;

    terminal_printf("Found %d display devices.\n", result->count);

    for (int i = 0; i < result->count; i++)
    {
        terminal_printf("Checking device %d/%d...\n", i + 1, result->count);
        const pci_address dev = pci_convert_address(result->devices[i]);

        for (size_t j = 0; j < NUM_VIDEO_DRIVERS; j++)
        {
            pci_device device;
            if (pci_get_device(dev.bus, dev.device, dev.function, &device) != 0)
                continue;

            if (video_drivers[j].detect(&device))
            {
                active_driver = &video_drivers[j];
                active_gpu = dev;

                const int err = video_drivers[j].init(&device, &framebuffer);
                if (err != 0)
                    return err;

                framebuffer_ready = true;
                terminal_switch_mode(VGA_LFB_MODE);
                return 0;
            }
        }
    }

    return -1; // no supported GPU found
}