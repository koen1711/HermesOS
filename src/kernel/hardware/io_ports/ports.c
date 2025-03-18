#include "ports.h"

bool ps2_is_dual_channel() {
    return (ports_read_uint8(0x92) & 0x01) == 0x01;
}

bool ps2_wait_for_controller() {
    uint64_t timeout = 100000;
    while (timeout--) {
        if ((ports_read_uint8(0x64) & 0x02) == 0) return true;
    }
    return false;
}

void ports_clear_read_buffer(const uint16_t command_port, const uint16_t data_port) {
    while (ports_read_uint8(command_port) & 0x01) {
        ports_read_uint8(data_port);
    }
}

uint8_t ports_read_uint8(uint16_t port) {
    uint8_t data;
    asm volatile("inb %1, %0" : "=a"(data) : "d"(port));
    return data;
}

void ports_write_uint8(uint16_t port, uint8_t data) {
    asm volatile("outb %0, %1" : : "a"(data), "d"(port));
}

uint16_t ports_read_uint16(uint16_t port) {
    uint16_t data;
    asm volatile("inw %1, %0" : "=a"(data) : "d"(port));
    return data;
}

void ports_write_uint16(uint16_t port, uint16_t data) {
    asm volatile("outw %0, %1" : : "a"(data), "d"(port));
}

uint32_t ports_read_uint32(uint16_t port) {
    uint32_t data;
    asm volatile("inl %1, %0" : "=a"(data) : "d"(port));
    return data;
}

void ports_write_uint32(uint16_t port, uint32_t data) {
    asm volatile("outl %0, %1" : : "a"(data), "d"(port));
}

void io_wait(void)
{
    asm volatile(
        "outb %%al, $0x80"
        :
        : "a"(0));
}


