#ifndef PCI_H
#define PCI_H

#define PCI_ADDRESS_PORT 0xCF8
#define PCI_VALUE_PORT   0xCFC

#define PCI_VENDOR_ID            0x00 // 2
#define PCI_DEVICE_ID            0x02 // 2
#define PCI_COMMAND              0x04 // 2
#define PCI_COMMAND_MASTER       0x04
#define PCI_STATUS               0x06 // 2
#define PCI_REVISION_ID          0x08 // 1

#define PCI_PROG_IF              0x09 // 1
#define PCI_SUBCLASS             0x0a // 1
#define PCI_CLASS                0x0b // 1
#define PCI_CACHE_LINE_SIZE      0x0c // 1
#define PCI_LATENCY_TIMER        0x0d // 1
#define PCI_HEADER_TYPE          0x0e // 1
#define PCI_BIST                 0x0f // 1
#define PCI_BAR0                 0x10 // 4
#define PCI_BAR1                 0x14 // 4
#define PCI_BAR2                 0x18 // 4
#define PCI_BAR3                 0x1C // 4
#define PCI_BAR4                 0x20 // 4
#define PCI_BAR5                 0x24 // 4

#define PCI_COMMAND_INTX_DISABLE 0x400
#define PCI_STATUS_CAP_LIST      0x10
#define PCI_CAP_PTR              0x34 // 1
#define PCI_CAP_MSIX             0x11
#define PCI_INTERRUPT_LINE        0x3C // 1
#define PCI_INTERRUPT_PIN         0x3D // 1

#define PCI_NONE 0xFFFF

#define PCI_TYPE_BRIDGE 0x0604
#define PCI_TYPE_SATA   0x0106

#define PCI_SECONDARY_BUS        0x19 // 1

#define PCI_NUM_BUSES 256
#define PCI_NUM_DEVICES 32
#define PCI_NUM_FUNCTIONS 8

#define MAX_DEVICES 10

#include <os/stdint.h>
#include <os/stdbool.h>

#include "ports.h"

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision_id;
    uint8_t header_type;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    bool multifunction;
} pci_device;

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} pci_address;

typedef struct {
    uint32_t devices[PCI_NUM_BUSES * PCI_NUM_DEVICES * PCI_NUM_FUNCTIONS];
    uint32_t count;
} scan_result;

void pci_initialize();
int pci_get_device(uint32_t bus, uint32_t device, uint32_t func, pci_device *out);

static pci_address pci_convert_address(const uint32_t address) {
    pci_address addr;
    addr.bus      = address >> 16 & 0xFF;
    addr.device   = address >>  8 & 0xFF;
    addr.function =  address        & 0x07;
    return addr;
}

static uint32_t pci_resolve_address(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset)
{
    return 1u << 31 |
        (uint32_t)bus << 16 |
        (uint32_t)slot << 11 |
        (uint32_t)func << 8 |
        (uint32_t)offset & 0xFC;
}

static void pci_config_write_u32(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset, const uint32_t value)
{
    port_write_u32(PCI_ADDRESS_PORT, pci_resolve_address(bus, slot, func, offset));
    port_write_u32(PCI_VALUE_PORT, value);
}

static void pci_config_write_u16(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset, const uint16_t value)
{
    port_write_u32(PCI_ADDRESS_PORT, pci_resolve_address(bus, slot, func, offset));
    port_write_u16(PCI_VALUE_PORT + (offset & 2), value);
}

static void pci_config_write_u8(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset, const uint8_t value)
{
    port_write_u32(PCI_ADDRESS_PORT, pci_resolve_address(bus, slot, func, offset));
    port_write_u8(PCI_VALUE_PORT + (offset & 3), value);
}

static uint32_t pci_config_read_u32(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset)
{
    port_write_u32(PCI_ADDRESS_PORT, pci_resolve_address(bus, slot, func, offset));
    return port_read_u32(PCI_VALUE_PORT);
}

static uint16_t pci_config_read_u16(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset) {
    uint16_t config = 0;

    port_write_u32(PCI_ADDRESS_PORT, pci_resolve_address(bus, slot, func, offset));
    config = port_read_u16(PCI_VALUE_PORT + (offset & 2));
    return config;
}

static uint8_t pci_config_read_u8(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset) {
    uint8_t config = 0;

    port_write_u32(PCI_ADDRESS_PORT, pci_resolve_address(bus, slot, func, offset));
    config = port_read_u8(PCI_VALUE_PORT + (offset & 3));
    return config;
}

static uint8_t pci_find_capability(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t id)
{
    if (!(pci_config_read_u16(bus, slot, func, PCI_STATUS) & PCI_STATUS_CAP_LIST))
        return 0;
    uint8_t cap = pci_config_read_u8(bus, slot, func, PCI_CAP_PTR) & ~3;
    for (int hops = 0; cap && hops < 48; hops++) {
        if (pci_config_read_u8(bus, slot, func, cap) == id)
            return cap;
        cap = pci_config_read_u8(bus, slot, func, cap + 1) & ~3;
    }
    return 0;
}

void pci_enable_bus_master(const pci_device* device);

scan_result* pci_scan(int type);
void pci_scan_bus(int type, int bus, scan_result *result);

static uint16_t pci_get_bar_type(const pci_device* device, const uint8_t bar_index)
{
    uint16_t bar_type = 0;
    bar_type = pci_config_read_u8(device->bus, device->device, device->function, PCI_BAR0 + bar_index);
    return bar_type;
}

#endif //PCI_H
