#ifndef PS2_H
#define PS2_H

#define DATA_PORT 0x60
#define COMMAND_PORT 0x64

#include <stdint.h>
#include <stdbool.h>

void io_wait(void);

bool ps2_is_dual_channel();
bool ps2_wait_for_controller();
void ports_clear_read_buffer(uint16_t command_port, uint16_t data_port);

void ps2_initialize();

uint8_t ports_read_uint8(uint16_t port);
void ports_write_uint8(uint16_t port, uint8_t data);

uint16_t ports_read_uint16(uint16_t port);
void ports_write_uint16(uint16_t port, uint16_t data);

uint32_t ports_read_uint32(uint16_t port);
void ports_write_uint32(uint16_t port, uint32_t data);

#endif //PS2_H
