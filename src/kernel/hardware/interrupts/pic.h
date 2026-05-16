#ifndef PIC_H
#define PIC_H

#include <os/stdint.h>

#define PIC_MASTER_COMMAND 0x20
#define PIC_MASTER_DATA 0x21
#define PIC_SLAVE_COMMAND 0xA0
#define PIC_SLAVE_DATA 0xA1

void pic_write(uint16_t port, uint8_t data);
uint8_t pic_read(uint16_t port);

void pic_remap(uint8_t master_offset, uint8_t slave_offset);
void pic_eoi(uint8_t irq);

#endif //PIC_H
