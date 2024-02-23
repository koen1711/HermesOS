#include "main.h"

void c_start()
{

    // print the multiboot info
    //printf("multiboot info: \n");
    // check if bit 11 and 12 are set
//    if ((mbi->flags & 0x400) && (mbi->flags & 0x800)) {
//        // get the multiboot info from grub in the ebx register in 32 bit mode
//        printf("multiboot info: %x\n", mbi);
//    } else {
//        int kernel_size = mbi->high_mem - mbi->low_mem;
//        int video_xbytes = mbi->framebuffer_pitch;
//        int video_xres = mbi->framebuffer_width;
//        int video_yres = mbi->framebuffer_height;
//        // print video info
//        printf("video: %d x %d\n", video_xres, video_yres, video_xbytes);
//        kernel_main(kernel_size, video_xres, video_yres, video_xbytes);
//    }

    // total memory is 1GB in bytes
    // gdb breakpoint instruction
    kernel_main(1024);
}