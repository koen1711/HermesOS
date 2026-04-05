#ifndef ATAPI_H
#define ATAPI_H

#define ATA_SECTOR_SIZE 512
#define ATA_CACHE_SIZE  4096

#define ATA_CMD_IDENTIFY   0xEC

#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_FEATURES   0x01
#define ATA_REG_SECCOUNT0  0x02
#define ATA_REG_LBA0       0x03
#define ATA_REG_LBA1       0x04
#define ATA_REG_LBA2       0x05
#define ATA_REG_HDDEVSEL   0x06
#define ATA_REG_COMMAND    0x07
#define ATA_REG_STATUS     0x07
#define ATA_REG_SECCOUNT1  0x08
#define ATA_REG_LBA3       0x09
#define ATA_REG_LBA4       0x0A
#define ATA_REG_LBA5       0x0B
#define ATA_REG_CONTROL    0x0C
#define ATA_REG_ALTSTATUS  0x0C
#define ATA_REG_DEVADDRESS 0x0D

#define ATA_SR_BSY     0x80
#define ATA_SR_DRDY    0x40
#define ATA_SR_DF      0x20
#define ATA_SR_DSC     0x10
#define ATA_SR_DRQ     0x08
#define ATA_SR_CORR    0x04
#define ATA_SR_IDX     0x02
#define ATA_SR_ERR     0x01

#define ATA_CMD_READ_PIO          0x20
#define ATA_CMD_READ_PIO_EXT      0x24

#define ATA_CMD_WRITE_PIO         0x30
#define ATA_CMD_WRITE_PIO_EXT     0x34

#include <stdint.h>

#include <hardware/port/ports.h>
#include "partition_table.h"

typedef struct {
    uint16_t flags;
    uint16_t _1[9]; // Unused
    char serial[20];
    uint16_t _2[3]; // Unused
    char firmware[8];
    char model[40];
    uint16_t sectors_per_int;
    uint16_t _3; // Unused
    uint16_t capabilities[2];
    uint16_t _4[2]; // Unused
    uint16_t valid_ext_data;
    uint16_t _5[5]; // Unused
    uint16_t size_of_rw_mult;
    uint32_t sectors_28;
    uint16_t _6[38];
    uint64_t sectors_48;
    uint16_t _7[152]; // Unused
} __attribute__((packed)) ata_identify;

typedef struct {
    uintptr_t offset;
    uint16_t bytes;
    uint16_t last;
} prdt;

typedef struct {
    int io_base;
    int control;
    int slave;
    int is_atapi;
    ata_identify identity;
    prdt * dma_prdt;
    uintptr_t dma_prdt_phys;
    uint8_t * dma_start;
    uintptr_t dma_start_phys;
    uint32_t bar4;
    uint32_t atapi_lba;
    uint32_t atapi_sector_size;
} ata_device;

static ata_device ata_primary_master   = {.io_base = 0x1F0, .control = 0x3F6, .slave = 0};
static ata_device ata_primary_slave    = {.io_base = 0x1F0, .control = 0x3F6, .slave = 1};
static ata_device ata_secondary_master = {.io_base = 0x170, .control = 0x376, .slave = 0};
static ata_device ata_secondary_slave  = {.io_base = 0x170, .control = 0x376, .slave = 1};

static void ata_select_device(const ata_device * dev)
{
    port_write_u8(dev->io_base + ATA_REG_HDDEVSEL, 0xA0 | dev->slave << 4);
}

static int is_el_torito(const uint8_t* sector) {
    return sector[0] == 0x01 &&  // Boot Record Indicator
        sector[1] == 'C' &&   // "CD001" standard identifier
        sector[2] == 'D' &&
        sector[3] == '0' &&
        sector[4] == '0' &&
        sector[5] == '1' &&
        sector[6] == 0x00;   // Boot System Identifier
}

static int is_gpt(const uint8_t* sector) {
    // GPT signature is "EFI PART" at offset 0x200 (sector 1)
    return sector[0] == 'E' &&
        sector[1] == 'F' &&
        sector[2] == 'I' &&
        sector[3] == ' ' &&
        sector[4] == 'P' &&
        sector[5] == 'A' &&
        sector[6] == 'R' &&
        sector[7] == 'T';
}

static bool is_valid_gpt_entry(const gpt_entry* entry) {
    // Check if the type GUID is all zeros
    if (entry->partition_type_guid[0] == 0 && entry->partition_type_guid[1] == 0) return false;
    return true;
}

int ata_read(uint32_t unique_id, uint64_t begin_byte, uint64_t end_byte, uint8_t *buffer);

void ata_initialize();

#endif //ATAPI_H
