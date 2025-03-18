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

#include <stdint.h>

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
} __attribute__((packed)) ata_identify_t;

typedef struct {
    uintptr_t offset;
    uint16_t bytes;
    uint16_t last;
} prdt_t;

struct ata_device {
    int io_base;
    int control;
    int slave;
    int is_atapi;
    ata_identify_t identity;
    prdt_t * dma_prdt;
    uintptr_t dma_prdt_phys;
    uint8_t * dma_start;
    uintptr_t dma_start_phys;
    uint32_t bar4;
    uint32_t atapi_lba;
    uint32_t atapi_sector_size;
};

static struct ata_device ata_primary_master   = {.io_base = 0x1F0, .control = 0x3F6, .slave = 0};
static struct ata_device ata_primary_slave    = {.io_base = 0x1F0, .control = 0x3F6, .slave = 1};
static struct ata_device ata_secondary_master = {.io_base = 0x170, .control = 0x376, .slave = 0};
static struct ata_device ata_secondary_slave  = {.io_base = 0x170, .control = 0x376, .slave = 1};

void ata_initialize();

#endif //ATAPI_H
