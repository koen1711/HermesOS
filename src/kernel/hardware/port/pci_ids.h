#ifndef PCI_IDS_C_H
#define PCI_IDS_C_H

#include <os/stdint.h>

const char* pci_lookup_device_name(uint16_t vendor_id, uint16_t device_id);

#endif //PCI_IDS_C_H
