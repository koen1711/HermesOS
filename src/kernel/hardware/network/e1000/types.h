#ifndef OS_E1000_TYPES_H
#define OS_E1000_TYPES_H

#define INTEL_VEND     0x8086  // Vendor ID for Intel
#include <os/stdint.h>

enum e1000_device_id {
    E1000_DEV = 0x100E,
    E1000_I217 = 0x153A,
    E1000_82577LM = 0x10EA,
};

enum e1000_registers
{
    REG_CTRL = 0x0000,
    REG_STATUS = 0x0008,
    REG_EEPROM = 0x0014,
    REG_CTRL_EXT = 0x0018,
    REG_ICR = 0x00C0,       // interrupt cause, cleared on read
    REG_IMASK = 0x00D0,     // IMS: interrupt mask set
    REG_IMC = 0x00D8,       // interrupt mask clear
    REG_RCTRL = 0x0100,
    REG_RXDESCLO = 0x2800,
    REG_RXDESCHI = 0x2804,
    REG_RXDESCLEN = 0x2808,
    REG_RXDESCHEAD = 0x2810,
    REG_RXDESCTAIL = 0x2818,

    REG_TCTRL = 0x0400,
    REG_TXDESCLO = 0x3800,
    REG_TXDESCHI = 0x3804,
    REG_TXDESCLEN = 0x3808,
    REG_TXDESCHEAD = 0x3810,
    REG_TXDESCTAIL = 0x3818,

    REG_RDTR = 0x2820,
    REG_RXDCTL = 0x2828,
    REG_RADV = 0x282C,
    REG_RSRPD = 0x2C00,

    REG_TIPG = 0x0410,

    REG_MTA = 0x5200,       // multicast table array, 128 x 32 bits
    REG_RAL = 0x5400,       // receive address low
    REG_RAH = 0x5404,       // receive address high
};

// CTRL register
#define CTRL_LRST     (1 << 3)      // link reset
#define CTRL_ASDE     (1 << 5)      // auto speed detection
#define CTRL_SLU      (1 << 6)      // set link up
#define CTRL_ILOS     (1 << 7)      // invert loss of signal
#define CTRL_RST      (1 << 26)     // device reset
#define CTRL_VME      (1 << 30)     // VLAN mode
#define CTRL_PHY_RST  (1u << 31)

// STATUS register
#define STATUS_LU     (1 << 1)      // link up

// ICR / IMS / IMC bits
#define ICR_TXDW      (1 << 0)      // transmit descriptor written back
#define ICR_LSC       (1 << 2)      // link status change
#define ICR_RXDMT0    (1 << 4)      // receive descriptors below threshold
#define ICR_RXO       (1 << 6)      // receiver overrun
#define ICR_RXT0      (1 << 7)      // receive timer

// RAH: address valid
#define RAH_AV        (1u << 31)

// RX descriptor status / TX descriptor status
#define RX_STATUS_DD  (1 << 0)
#define RX_STATUS_EOP (1 << 1)

#define E1000_MMIO_SIZE   0x20000   // 128 KiB register space
#define E1000_RX_BUF_SIZE 2048      // matches RCTL_BSIZE_2048
#define E1000_MAX_FRAME   1514      // without FCS, no jumbo frames


#define ECTRL_SLU        0x40        //set link up

enum e1000_receive_control {
    RCTL_EN = 1 << 1,
    RCTL_SBP = 1 << 2,
    RCTL_UPE = 1 << 3,
    RCTL_MPE = 1 << 4,
    RCTL_LPE = 1 << 5,
    RCTL_LBM_NONE = 0 << 6,
    RCTL_LBM_PHY = 3 << 6,
    RCTL_RDMTS_HALF = 0 << 8,
    RCTL_RDMTS_QUARTER = 1 << 8,
    RCTL_RDMTS_EIGHTH = 2 << 8,
    RCTL_MO_36 = 0 << 12,
    RCTL_MO_35 = 1 << 12,
    RCTL_MO_34 = 2 << 12,
    RCTL_MO_32 = 3 << 12,
    RCTL_BAM = 1 << 15,
    RCTL_VFE = 1 << 18,
    RCTL_CFIEN = 1 << 19,
    RCTL_CFI = 1 << 20,
    RCTL_DPF = 1 << 22,
    RCTL_PMCF = 1 << 23,
    RCTL_SECRC = 1 << 26,
};

// Buffer Sizes
#define RCTL_BSIZE_256                  (3 << 16)
#define RCTL_BSIZE_512                  (2 << 16)
#define RCTL_BSIZE_1024                 (1 << 16)
#define RCTL_BSIZE_2048                 (0 << 16)
#define RCTL_BSIZE_4096                 ((3 << 16) | (1 << 25))
#define RCTL_BSIZE_8192                 ((2 << 16) | (1 << 25))
#define RCTL_BSIZE_16384                ((1 << 16) | (1 << 25))


// Transmit Command

enum e1000_transmit_command
{
    CMD_EOP = 0x01,
    CMD_IFCS = 0x02,
    CMD_IC = 0x04,
    CMD_RS = 0x08,
    CMD_RPS = 0x10,
    CMD_VLE = 0x40,
    CMD_IDE = 0x80,
};


// TCTL Register

enum e1000_transmit_control {
    TCTL_EN = 1 << 1,
    TCTL_PSP = 1 << 3 ,
    TCTL_CT_SHIFT = 4,
    TCTL_COLD_SHIFT = 12,
    TCTL_SWXOFF = 1 << 22,
    TCTL_RTLC = 1 << 24,
};

enum e1000_transmit_status
{
    TSTA_DD = 1 << 0,
    TSTA_EC = 1 << 1,
    TSTA_LC = 1 << 2,
    TSTA_TU = 1 << 3,
};

#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 8

struct e1000_rx_desc {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint16_t checksum;
    volatile uint8_t status;
    volatile uint8_t errors;
    volatile uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint8_t cso;
    volatile uint8_t cmd;
    volatile uint8_t status;
    volatile uint8_t css;
    volatile uint16_t special;
} __attribute__((packed));

#endif //OS_E1000_TYPES_H