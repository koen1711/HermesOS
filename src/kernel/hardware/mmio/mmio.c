#include "mmio.h"

uint8_t mmio_read_u8(const uint16_t address)
{
    return *(volatile uint8_t*)address;
}

void mmio_write_u8(const uint16_t address, const uint8_t data)
{
    *(volatile uint8_t*)address = data;
}

uint16_t mmio_read_u16(const uint16_t address)
{
    return *(volatile uint16_t*)address;
}

void mmio_write_u16(const uint16_t address, const uint16_t data)
{
    *(volatile uint16_t*)address = data;
}

uint32_t mmio_read_u32(const uint16_t address)
{
    return *(volatile uint32_t*)address;
}

void mmio_write_u32(const uint16_t address, const uint32_t data)
{
    *(volatile uint32_t*)address = data;
}