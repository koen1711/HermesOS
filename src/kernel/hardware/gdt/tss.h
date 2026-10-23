#ifndef TSS_H
#define TSS_H

#include <os/stdint.h>

#define TSS_GDT_INDEX 5
#define TSS_SELECTOR  (TSS_GDT_INDEX * 8) // 0x28

typedef struct {
    uint32_t res0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t res1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t res2;
    uint16_t res3;
    uint16_t iopb;
} __attribute__((__packed__)) tss_t;

_Static_assert(sizeof(tss_t) == 104, "TSS layout must match the CPU");

extern tss_t task_state_segment;

void tss_initialize();

#endif //TSS_H
