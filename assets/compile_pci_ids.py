#!/usr/bin/env python3

import sys
import os
import re
from collections import defaultdict

def parse_pci_ids(filename):
    """Parse the pci.ids file and extract vendor and device information."""
    vendors = {}
    current_vendor = None

    with open(filename, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.rstrip()

            # Skip comments and empty lines
            if line.startswith('#') or not line:
                continue

            # Vendor line
            if not line.startswith('\t'):
                vendor_match = re.match(r'^([0-9a-f]{4})\s+(.+)$', line)
                if vendor_match:
                    vendor_id = vendor_match.group(1)
                    vendor_name = vendor_match.group(2)
                    current_vendor = vendor_id
                    vendors[current_vendor] = {
                        'name': vendor_name,
                        'devices': {}
                    }

            # Device line
            elif line.startswith('\t') and not line.startswith('\t\t') and current_vendor:
                device_match = re.match(r'^\t([0-9a-f]{4})\s+(.+)$', line)
                if device_match:
                    device_id = device_match.group(1)
                    device_name = device_match.group(2)
                    vendors[current_vendor]['devices'][device_id] = device_name

    return vendors

def generate_c_code(vendors):
    """Generate C code with a switch statement for the PCI vendors and devices."""
    c_code = """/**
 * pci_ids.c - PCI Vendor and Device ID to name mapping
 * 
 * Automatically generated from pci.ids file
 * Only includes vendor and device IDs (no subsystem IDs)
 */

#include <stdint.h>

const char* pci_lookup_device_name(uint16_t vendor_id, uint16_t device_id)
{
    switch (vendor_id) {
"""

    # Generate the nested switch statements for each vendor
    for vendor_id, vendor_info in sorted(vendors.items()):
        c_code += f"    case 0x{vendor_id}:  /* {vendor_info['name']} */\n"
        c_code += "        switch (device_id) {\n"

        # Add each device under this vendor
        for device_id, device_name in sorted(vendor_info['devices'].items()):
            device_name_escaped = device_name.replace('"', '\\"')
            c_code += f'        case 0x{device_id}: return "{device_name_escaped}";\n'

        c_code += "        default: return 0;\n"
        c_code += "        }\n"

    # Close the vendor switch and function
    c_code += "    default:\n"
    c_code += '        return 0;\n'
    c_code += "    }\n"
    c_code += "}\n\n"

    # Add a function to look up vendor names
    c_code += """const char* pci_lookup_vendor_name(uint16_t vendor_id)
{
    switch (vendor_id) {
"""

    # Generate vendor lookup cases
    for vendor_id, vendor_info in sorted(vendors.items()):
        vendor_name_escaped = vendor_info['name'].replace('"', '\\"')
        c_code += f'    case 0x{vendor_id}: return "{vendor_name_escaped}";\n'

    # Close the vendor lookup function
    c_code += "    default:\n"
    c_code += '        return 0;\n'
    c_code += "    }\n"
    c_code += "}\n"

    return c_code

def generate_header_file():
    """Generate a header file for the PCI ID functions."""
    header = """/**
 * pci_ids.h - Header for PCI Vendor and Device ID lookup functions
 */

#ifndef PCI_IDS_H
#define PCI_IDS_H

#include <hardware/io_ports/pci_ids.h>

/**
 * Look up a device name from vendor and device IDs
 * 
 * @param vendor_id The PCI vendor ID
 * @param device_id The PCI device ID
 * @return The device name or 0 if not found
 */
const char* pci_lookup_device_name(uint16_t vendor_id, uint16_t device_id);

/**
 * Look up a vendor name from vendor ID
 * 
 * @param vendor_id The PCI vendor ID
 * @return The vendor name or 0 if not found
 */
const char* pci_lookup_vendor_name(uint16_t vendor_id);

#endif /* PCI_IDS_H */
"""
    return header

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} pci.ids output_dir")
        sys.exit(1)

    pci_ids_file = sys.argv[1]
    output_dir = sys.argv[2]

    if not os.path.isfile(pci_ids_file):
        print(f"Error: PCI IDs file '{pci_ids_file}' not found.")
        sys.exit(1)

    if not os.path.isdir(output_dir):
        print(f"Error: Output directory '{output_dir}' not found.")
        sys.exit(1)

    # Parse the PCI IDs file
    print(f"Parsing PCI IDs file: {pci_ids_file}")
    vendors = parse_pci_ids(pci_ids_file)

    # Generate the C code
    c_code = generate_c_code(vendors)
    header_code = generate_header_file()

    # Write the C code to file
    c_file_path = os.path.join(output_dir, "pci_ids.c")
    h_file_path = os.path.join(output_dir, "pci_ids.h")

    with open(c_file_path, 'w', encoding='utf-8') as f:
        f.write(c_code)

    with open(h_file_path, 'w', encoding='utf-8') as f:
        f.write(header_code)

    print(f"Generated {c_file_path} with {len(vendors)} vendors and {sum(len(v['devices']) for v in vendors.values())} devices")
    print(f"Generated {h_file_path}")

if __name__ == "__main__":
    main()