#include "pci.h"

#include <hardware/memory/alloc.h>
#include <hardware/port/pci_ids.h>

#include "ports.h"
#include "drivers/terminal/terminal.h"
#include "utils/str/str.h"

uint32_t pci_config_read_u32(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset) {
	const uint32_t address =
		(1u << 31) |
		((uint32_t)bus  << 16) |
		((uint32_t)slot << 11) |
		((uint32_t)func <<  8) |
		((uint32_t)offset & 0xFC);

	port_write_u32(PCI_ADDRESS_PORT, address);
	return port_read_u32(PCI_VALUE_PORT);
}

uint16_t pci_config_read_u16(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset) {
	uint16_t config = 0;

	const uint32_t address =
		(1u << 31) |
		((uint32_t)bus  << 16) |
		((uint32_t)slot << 11) |
		((uint32_t)func <<  8) |
		((uint32_t)offset & 0xFC);

	port_write_u32(PCI_ADDRESS_PORT, address);
	config = port_read_u16(PCI_VALUE_PORT + (offset & 2));
	return config;
}

uint8_t pci_config_read_u8(const uint8_t bus, const uint8_t slot, const uint8_t func, const uint8_t offset) {
	uint8_t config = 0;

	const uint32_t address =
		(1u << 31) |
		((uint32_t)bus  << 16) |
		((uint32_t)slot << 11) |
		((uint32_t)func <<  8) |
		((uint32_t)offset & 0xFC);

	port_write_u32(PCI_ADDRESS_PORT, address);
	config = port_read_u8(PCI_VALUE_PORT + (offset & 3));
	return config;
}

bool pci_is_device_multifunction(const uint8_t bus, const uint8_t slot) {
	return (pci_config_read_u8(bus, slot, 0, PCI_HEADER_TYPE) & 0x80) != 0;
}
uint32_t pci_box_device(const uint8_t bus, const uint8_t slot, const uint8_t func) {
	return bus << 16 | slot << 8 | func;
}

uint16_t pci_find_type(const uint32_t dev) {
	const uint8_t bus  = (dev >> 16) & 0xFF;
	const uint8_t slot = (dev >> 8)  & 0xFF;
	const uint8_t func = dev & 0x07;

	return ((uint16_t)pci_config_read_u8(bus, slot, func, PCI_CLASS) << 8) |
		   pci_config_read_u8(bus, slot, func, PCI_SUBCLASS);
}

#define PCI_BUS(dev)   (((dev) >> 16) & 0xFF)
#define PCI_SLOT(dev)  (((dev) >>  8) & 0xFF)
#define PCI_FUNC(dev) ((dev) & 0x07)

void pci_initialize()
{
	const scan_result* result = pci_scan(-1);

	for (int i = 0; i < result->count; i++) {
		const uint32_t dev = result->devices[i];
		const uint16_t vendor_id = pci_config_read_u16(PCI_BUS(dev), PCI_SLOT(dev), PCI_FUNC(dev), PCI_VENDOR_ID);
		const uint16_t device_id = pci_config_read_u16(PCI_BUS(dev), PCI_SLOT(dev), PCI_FUNC(dev), PCI_DEVICE_ID);

		const char* vendor_name = pci_lookup_device_name(vendor_id, device_id);
		if (vendor_name == 0) {
			vendor_name = str_concat_variadic(4, "Unknown 0x", str_u16_hex(vendor_id), " 0x", str_u16_hex(device_id));
		}

		terminal_printf("PCI: %s with function %u at %02x:%02x.%u\n", vendor_name, PCI_FUNC(dev), PCI_BUS(dev), PCI_SLOT(dev), PCI_FUNC(dev));

		uint8_t class_code = pci_config_read_u8(PCI_BUS(dev), PCI_SLOT(dev), PCI_FUNC(dev), PCI_CLASS);
		uint8_t subclass   = pci_config_read_u8(PCI_BUS(dev), PCI_SLOT(dev), PCI_FUNC(dev), PCI_SUBCLASS);
		uint8_t prog_if    = pci_config_read_u8(PCI_BUS(dev), PCI_SLOT(dev), PCI_FUNC(dev), PCI_PROG_IF);

		terminal_printf("PCI %02x:%02x.%u vid:did=%04x:%04x class=%02x subclass=%02x prog_if=%02x\n",
			   PCI_BUS(dev), PCI_SLOT(dev), (unsigned)PCI_FUNC(dev),
			   vendor_id, device_id, class_code, subclass, prog_if);
	}
}

bool is_match(const int type, const uint8_t class_code, uint16_t pci_type)
{
	if (type == -1) return true;
	if (type <= 0xFF) return class_code == type;
	return pci_type == (uint16_t)type;
}

void pci_scan_func(const int type, const int bus, const int slot, const int func, scan_result *result) {
	const uint32_t dev = pci_box_device(bus, slot, func);
	const uint16_t pci_type = pci_find_type(dev);
	const uint8_t class_code = (pci_type >> 8);

	if (is_match(type, class_code, pci_type)) {
		result->devices[result->count] = dev;
		result->count = result->count + 1;
	}

	if (pci_type == PCI_TYPE_BRIDGE)
		pci_scan_bus(type, pci_config_read_u8(bus, slot, func, PCI_SECONDARY_BUS), result);
}

void pci_scan_slot(const int type, const int bus, const int slot, scan_result *result) {
	if (pci_config_read_u16(bus, slot, 0, PCI_VENDOR_ID) == PCI_NONE)
		return;

	pci_scan_func(type, bus, slot, 0, result);

	// Check for multifunction devices
	if (!pci_is_device_multifunction(bus, slot))
		return;
	for (int func = 1; func < PCI_NUM_FUNCTIONS; func++) {
		if (pci_config_read_u16(bus, slot, func, PCI_VENDOR_ID) == PCI_NONE)
			continue;
		pci_scan_func(type, bus, slot, func, result);
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
		pci_scan_bus(type, 0, result);
		return result;
	}

	bool hit = false;
	for (int func = 0; func < PCI_NUM_FUNCTIONS; ++func) {
		if (pci_config_read_u16(0, 0, func, PCI_VENDOR_ID) != PCI_NONE) {
			hit = true;
			pci_scan_bus(type, func, result); // if your design is "func indicates bus" for host bridges
		}
	}

	if (!hit) {
		for (int bus = 0; bus < PCI_NUM_BUSES; ++bus) {
			for (int slot = 0; slot < PCI_NUM_DEVICES; ++slot) {
				pci_scan_slot(type, bus, slot, result);
			}
		}
	}

	return result;
}

int pci_get_device(const uint32_t bus, const uint32_t device, const uint32_t func, pci_device *out)
{
	const uint16_t vendor_id = pci_config_read_u16(bus, device, func, PCI_VENDOR_ID);
	if (vendor_id == PCI_NONE)
		return -1;

	out->bus = bus;
	out->device = device;
	out->function = func;
	out->vendor_id = vendor_id;
	out->device_id = pci_config_read_u16(bus, device, func, PCI_DEVICE_ID);
	out->class_code = pci_config_read_u8(bus, device, func, PCI_CLASS);
	out->subclass = pci_config_read_u8(bus, device, func, PCI_SUBCLASS);
	out->prog_if = pci_config_read_u8(bus, device, func, PCI_PROG_IF);
	out->revision_id = pci_config_read_u8(bus, device, func, PCI_REVISION_ID);
	out->header_type = pci_config_read_u8(bus, device, func, PCI_HEADER_TYPE);
	out->interrupt_line = pci_config_read_u8(bus, device, func, PCI_INTERRUPT_LINE);
	out->interrupt_pin = pci_config_read_u8(bus, device, func, PCI_INTERRUPT_PIN);
	out->multifunction = pci_is_device_multifunction(bus, device);

	return 0;
}