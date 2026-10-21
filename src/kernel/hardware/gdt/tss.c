#include "tss.h"

#define TSS_TYPE_AVAILABLE_64 0x89

extern uint64_t gdt_start[];

TSS TaskStateSegment;

void tss_initialize(void) {
    const uint64_t base = (uint64_t)&TaskStateSegment;
    const uint64_t limit = sizeof(TSS) - 1;


    TaskStateSegment.iopb = sizeof(TSS);

    gdt_start[TSS_GDT_INDEX] = (limit & 0xFFFF)
                             | ((base & 0xFFFFFF) << 16)
                             | ((uint64_t)TSS_TYPE_AVAILABLE_64 << 40)
                             | (((limit >> 16) & 0xF) << 48)
                             | (((base >> 24) & 0xFF) << 56);
    gdt_start[TSS_GDT_INDEX + 1] = base >> 32;

    asm volatile("ltr %w0" : : "r"((uint16_t)TSS_SELECTOR));
}
