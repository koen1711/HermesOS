#include "pci.h"

#include <hardware/memory/alloc.h>
#include <hardware/port/pci_ids.h>

#include "ports.h"
#include <hardware/terminal/stdio.h>
#include "utils/str/str.h"

uint16_t pci_config_read_u16(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset) {
	uint16_t config = 0;

	const uint32_t address = bus << 16 | slot << 11 |
		func << 8 | offset & 0xFC | 0x80000000;

	port_write_u32(PCI_ADDRESS_PORT, address);
	config = port_read_u16(PCI_VALUE_PORT + (offset & 2));
	return config;
}

uint8_t pci_config_read_u8(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset) {
	uint8_t config = 0;

	const uint32_t address = bus << 16 | slot << 11 |
		func << 8 | offset & 0xFC | 0x80000000;

	port_write_u32(PCI_ADDRESS_PORT, address);
	config = port_read_u8(PCI_VALUE_PORT + (offset & 3));
	return config;
}

bool pci_is_device_multifunction(const uint8_t bus, const uint8_t device) {
    return pci_config_read_u16(bus, device, 0, 0xE) & (1 << 7);
}

void pci_initialize()
{
	const scan_result* result = pci_scan(-1);

	for (int i = 0; i < result->count; i++) {
		const uint32_t dev = result->devices[i];
		const uint16_t vendor_id = pci_config_read_u16(dev >> 16, dev >> 8, 0, PCI_VENDOR_ID);
		const uint16_t device_id = pci_config_read_u16(dev >> 16, dev >> 8, 0, PCI_DEVICE_ID);

		const char* vendor_name = pci_lookup_device_name(vendor_id, device_id);
		if (vendor_name == 0) {
			vendor_name = str_concat_variadic(4, "Unknown 0x", str_u16_hex(vendor_id), " 0x", str_u16_hex(device_id));
		}
		printf("PCI: %s\n", vendor_name);
	}
}

uint32_t pci_box_device(const uint8_t bus, const uint8_t slot, const uint8_t func) {
	return bus << 16 | slot << 8 | func;
}

uint16_t pci_find_type(const uint32_t dev) {
	return pci_config_read_u16(dev >> 16, dev >> 8, 0, PCI_CLASS) << 8 | pci_config_read_u8(dev >> 16, dev >> 8, 0, PCI_SUBCLASS);
}

void pci_scan_func(const int type, const int bus, const int slot, const int func, scan_result *result) {
	const uint32_t dev = pci_box_device(bus, slot, func);
	if (type == -1 || type == pci_find_type(dev)) {
		result->devices[result->count] = dev;
		result->count = result->count + 1;
	}
	if (pci_find_type(dev) == PCI_TYPE_BRIDGE) {
		pci_scan_bus(type, pci_config_read_u8(bus, slot, func, PCI_SECONDARY_BUS), result);
	}
}

void pci_scan_slot(const int type, const int bus, const int slot, scan_result *result) {
	if (pci_config_read_u16(bus, slot, 0, PCI_VENDOR_ID) == PCI_NONE)
		return;
	pci_scan_func(type, bus, slot, 0, result);
	// Check for multifunction devices
	if (!pci_is_device_multifunction(bus, slot))
		return;
	for (int func = 1; func < PCI_NUM_FUNCTIONS; func++) {
		const uint32_t device = pci_box_device(bus, slot, func);
		if (pci_config_read_u16(bus, device, func, PCI_DEVICE_ID) != PCI_NONE) {
			pci_scan_func(type, bus, slot, func, result);
		}
	}
}

void pci_scan_bus(const int type, const int bus, scan_result *result) {
	for (int slot = 0; slot < PCI_NUM_DEVICES; ++slot) {
		pci_scan_slot(type, bus, slot, result);
	}
}

scan_result* pci_scan(const int type) {
	scan_result* result = malloc(sizeof(scan_result));
	result->count = 0;

	// Check if the system is a single PCI bus system
	if ((pci_config_read_u8(0, 0, 0, PCI_HEADER_TYPE) & 0x80) == 0) {
		pci_scan_bus(type,0,result);
		return result;
	}

	bool hit = false;
	for (int func = 0; func < PCI_NUM_FUNCTIONS; ++func) {
		const uint32_t dev = pci_box_device(0, 0, func);
		// Check if the device is valid
		if (pci_config_read_u16(dev, 0, PCI_VENDOR_ID, PCI_DEVICE_ID) == PCI_NONE) {
			hit = true;
			pci_scan_bus(type, func, result);
		}
	}

	if (!hit) {
		for (int bus = 0; bus < PCI_NUM_BUSES; ++bus) {
			for (int slot = 0; slot < PCI_NUM_DEVICES; ++slot) {
				pci_scan_slot(type,bus,slot,result);
			}
		}
	}

	return result;
}
