#ifndef OS_MMIO_H
#define OS_MMIO_H

#include <os/stdint.h>

uint8_t mmio_read_u8(uint16_t address);
void mmio_write_u8(uint16_t address, uint8_t data);

uint16_t mmio_read_u16(uint16_t address);
void mmio_write_u16(uint16_t address, uint16_t data);

uint32_t mmio_read_u32(uint16_t address);
void mmio_write_u32(uint16_t address, uint32_t data);

#endif //OS_MMIO_H