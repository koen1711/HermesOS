/*
Copyright (C) 2015-2019 The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file LICENSE for details.
*/

#include "terminal/console.h"
#include "cfunctions/page/page.h"
#include "runtime//process.h"
#include "hardware/keyboard/keyboard.h"
#include "hardware/mouse/mouse.h"
#include "interrupt/interrupt.h"
#include "cfunctions/time/clock.h"
#include "hardware/drives/ata.h"
#include "hardware/drives/iso9660/cdromfs.h"
#include "cfunctions/string/string.h"
#include "cfunctions/time/rtc.h"
#include "kernelcore.h"
#include "cfunctions/memory/kmalloc.h"
#include "cfunctions/memory/memorylayout.h"
#include "terminal/kshell.h"
#include "hardware/drives/diskfs.h"
#include "hardware/drives/fat/fatfs.h"

/*
This is the C initialization point of the kernel.
By the time we reach this point, we are in protected mode,
with interrupts disabled, a valid C stack, but no malloc heap.
Now we initialize each subsystem in the proper order:
*/

int kernel_main()
{
	struct console *console = console_create_root();
	console_addref(console);

	printf("video: %d x %d\n", video_xres, video_yres, video_xbytes);
	printf("kernel: %d bytes\n", kernel_size);

	page_init();
	kmalloc_init((char *) KMALLOC_START, KMALLOC_LENGTH);
	interrupt_init();
	mouse_init();
	keyboard_init();
	rtc_init();
	clock_init();
	process_init();
	ata_init();
    fatfs_init();
	cdrom_init();
	diskfs_init();

	current->ktable[KNO_STDIN]   = kobject_create_console(console);
	current->ktable[KNO_STDOUT]  = kobject_copy(current->ktable[0]);
	current->ktable[KNO_STDERR]  = kobject_copy(current->ktable[1]);
	current->ktable[KNO_STDWIN]  = kobject_create_window(&window_root);
	current->ktable[KNO_STDDIR]  = 0; // No current dir until something is mounted.

	printf("\n");
	kshell_launch();

	while(1) {
		console_putchar(console,console_getchar(console));
	}

	return 0;
}
