#ifndef OS_E1000_H
#define OS_E1000_H

#include <os/stdint.h>
#include <os/stdbool.h>
#include <hardware/port/pci.h>
#include <hardware/network/e1000/types.h>

// Intel 8254x (e1000) NIC driver. Plugs into the network stack through
// struct netif_ops (drivers/net/core/netif.h) and hands received frames to netif_input.
// References: Intel 8254x Software Developer's Manual, OSDev wiki "Intel Ethernet i217",
// FreeBSD sys/dev/e1000 (em(4))

struct netif;
struct netbuf;

// Driver state, stored in netif->softc
struct e1000_device {
    pci_device pci;
    struct netif* nif;

    uint8_t bar_type;       // type of BAR0: 0 = MMIO, 1 = I/O port
    uint16_t io_base;       // I/O base address
    uint64_t mem_base;      // MMIO base address
    bool eeprom_exists;
    uint8_t mac[6];

    struct e1000_rx_desc* rx_descs[E1000_NUM_RX_DESC];     // ring of E1000_NUM_RX_DESC, physically contiguous
    struct e1000_tx_desc* tx_descs[E1000_NUM_TX_DESC];     // ring of E1000_NUM_TX_DESC
    uint8_t* rx_buffers[E1000_NUM_RX_DESC];
    struct netbuf* tx_netbufs[E1000_NUM_TX_DESC];  // freed once the NIC sets TSTA_DD
    uint16_t rx_cur;        // next RX descriptor the NIC will fill
    uint16_t tx_cur;        // next free TX descriptor
    uint16_t tx_clean;      // oldest TX descriptor not yet reclaimed
    bool irq_registered;
};

void e1000_initialize();
struct e1000_device* e1000_probe(pci_device* pci);

void e1000_write_command(const struct e1000_device* dev, uint16_t address, uint32_t value);
uint32_t e1000_read_command(const struct e1000_device* dev, uint16_t address);

bool e1000_detect_eeprom(struct e1000_device* dev);
uint16_t e1000_eeprom_read(const struct e1000_device* dev, uint8_t address);
bool e1000_read_mac_address(struct e1000_device* dev);
bool e1000_start_link(struct e1000_device* dev);
bool e1000_link_up(struct e1000_device* dev);

void e1000_rx_init(struct e1000_device* dev);
void e1000_tx_init(struct e1000_device* dev);
void e1000_enable_interrupts(const struct e1000_device* dev);
void e1000_disable_interrupts(struct e1000_device* dev);

void e1000_interrupt(uint8_t irq, void* ctx);
void e1000_handle_receive(struct e1000_device* dev);
void e1000_reclaim_tx(struct e1000_device* dev);

int e1000_netif_init(struct netif* nif);
// Takes ownership of nb, also on failure (netif_ops.transmit contract)
int e1000_transmit(struct netif* nif, struct netbuf* nb);
int e1000_ioctl(struct netif* nif, uint32_t cmd, void* arg);
void e1000_qflush(struct netif* nif);

#endif //OS_E1000_H
