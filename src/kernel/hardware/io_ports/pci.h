#ifndef PCI_H
#define PCI_H

#define PCI_ADDRESS_PORT 0xCF8
#define PCI_VALUE_PORT   0xCFC

#define PCI_VENDOR_ID            0x00 // 2
#define PCI_DEVICE_ID            0x02 // 2
#define PCI_COMMAND              0x04 // 2
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

#define PCI_NONE 0xFFFF

#define PCI_TYPE_BRIDGE 0x0604
#define PCI_TYPE_SATA   0x0106

#define PCI_SECONDARY_BUS        0x19 // 1

#define PCI_NUM_BUSES 256
#define PCI_NUM_DEVICES 32
#define PCI_NUM_FUNCTIONS 8

#define MAX_DEVICES 10

#include <stdint.h>
#include <stdbool.h>

struct pci_device {
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
};

struct pci_address {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

struct scan_result {
    uint32_t devices[PCI_NUM_BUSES * PCI_NUM_DEVICES * PCI_NUM_FUNCTIONS];
    uint32_t count;
};

void pci_initialize();
bool pci_get_device(uint32_t bus, uint32_t device);

static struct pci_address pci_convert_address(const uint32_t address) {
    struct pci_address addr;
    addr.bus = address >> 16;
    addr.device = address >> 8;
    addr.function = address;
    return addr;
}

void pci_write_u32(uint16_t port, uint32_t data);
uint32_t pci_read_u32(uint16_t port);
void pci_write_u16(uint16_t port, uint16_t data);
uint16_t pci_read_u16(uint16_t port);
void pci_write_u8(uint16_t port, uint8_t data);
uint8_t pci_read_u8(uint16_t port);

uint32_t pci_config_read_u32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read_u16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t pci_config_read_u8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

// Scan the PCI bus for devices
struct scan_result* pci_scan(int type);
void pci_scan_bus(int type, int bus, struct scan_result *result);

uint16_t pci_config_read_u16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);


#endif //PCI_H
