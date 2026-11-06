#ifndef PS2_H
#define PS2_H

#define DATA_PORT 0x60
#define COMMAND_PORT 0x64

#include <os/stdint.h>



static uint8_t port_read_u8(uint16_t port) {
    uint8_t data;
    asm volatile("inb %1, %0" : "=a"(data) : "d"(port));
    return data;
}

static void port_write_u8(uint16_t port, uint8_t data) {
    asm volatile("outb %0, %1" : : "a"(data), "d"(port));
}

static uint16_t port_read_u16(uint16_t port) {
    uint16_t data;
    asm volatile("inw %1, %0" : "=a"(data) : "d"(port));
    return data;
}

static void port_write_u16(uint16_t port, uint16_t data) {
    asm volatile("outw %0, %1" : : "a"(data), "d"(port));
}

static uint32_t port_read_u32(uint16_t port) {
    uint32_t data;
    asm volatile("inl %1, %0" : "=a"(data) : "d"(port));
    return data;
}

static void port_write_u32(uint16_t port, uint32_t data) {
    asm volatile("outl %0, %1" : : "a"(data), "d"(port));
}

static void port_io_wait(void)
{
    asm volatile(
        "outb %%al, $0x80"
        :
        : "a"(0));
}

static void port_clear_read_buffer(const uint16_t command_port, const uint16_t data_port) {
    while (port_read_u8(command_port) & 0x01) {
        port_read_u8(data_port);
    }
}

#endif //PS2_H
