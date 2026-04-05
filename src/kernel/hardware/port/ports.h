#ifndef PS2_H
#define PS2_H

#define DATA_PORT 0x60
#define COMMAND_PORT 0x64

#include <stdint.h>

void port_io_wait(void);

void port_clear_read_buffer(uint16_t command_port, uint16_t data_port);

uint8_t port_read_u8(uint16_t port);
void port_write_u8(uint16_t port, uint8_t data);

uint16_t port_read_u16(uint16_t port);
void port_write_u16(uint16_t port, uint16_t data);

uint32_t port_read_u32(uint16_t port);
void port_write_u32(uint16_t port, uint32_t data);

#endif //PS2_H
