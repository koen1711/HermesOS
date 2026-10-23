#include "e1000.h"

#include <drivers/net/core/netif.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netqueue.h>
#include <drivers/net/ethernet/ethernet.h>
#include <hardware/interrupts/irq.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/mmu.h>
#include <hardware/port/ports.h>
#include <os/errno.h>

// DMA assumes the heap is identity mapped (virtual == physical), which holds for
// the first 1 GiB set up by setup_identity_mapping.
static inline uint64_t dma_addr(const void* p) { return (uint64_t)(uintptr_t)p; }

// malloc with the alignment the NIC needs; the raw pointer is not kept, rings live forever
static void* malloc_aligned(const size_t size, const size_t align)
{
    auto raw = (uintptr_t)malloc(size + align);
    if (!raw) return nullptr;
    return (void*)((raw + align - 1) & ~(uintptr_t)(align - 1));
}

static const struct netif_ops e1000_netif_ops = {
    .init = e1000_netif_init,
    .transmit = e1000_transmit,
    .ioctl = e1000_ioctl,
    .qflush = e1000_qflush,
};

static bool e1000_supported(const pci_device* pci)
{
    return pci->vendor_id == INTEL_VEND
        && (pci->device_id == E1000_DEV || pci->device_id == E1000_I217 || pci->device_id == E1000_82577LM);
}

void e1000_initialize()
{
    for (uint32_t bus = 0; bus < PCI_NUM_BUSES; bus++) {
        for (uint32_t slot = 0; slot < PCI_NUM_DEVICES; slot++) {
            pci_device pci;
            if (pci_get_device(bus, slot, 0, &pci) != 0 || !e1000_supported(&pci))
                continue;

            struct e1000_device* dev = e1000_probe(&pci);
            if (!dev || !e1000_start_link(dev))
                continue;

            struct netif* nif = netif_alloc(NETIF_TYPE_ETHERNET, "em");
            if (!nif)
                continue;
            nif->softc = dev;
            nif->ops = &e1000_netif_ops;
            dev->nif = nif;
            eth_ifattach(nif, dev->mac);
            netif_link_state_change(nif, e1000_link_up(dev) ? NETIF_LINK_UP : NETIF_LINK_DOWN);
        }
    }
}

struct e1000_device* e1000_probe(pci_device* pci)
{
    struct e1000_device* dev = malloc(sizeof(struct e1000_device));
    if (!dev) return nullptr;
    memset(dev, 0, sizeof(*dev));
    dev->pci = *pci;

    // BAR0 is the register window: MMIO on the 8254x, possibly 64-bit
    const uint32_t bar0 = pci_config_read_u32(pci->bus, pci->device, pci->function, PCI_BAR0);
    dev->bar_type = bar0 & 1;
    if (dev->bar_type == 0) {
        uint64_t base = bar0 & ~0xFULL;
        if (((bar0 >> 1) & 3) == 2)
            base |= (uint64_t)pci_config_read_u32(pci->bus, pci->device, pci->function, PCI_BAR1) << 32;
        dev->mem_base = base;

        // identity-map the 128 KiB register window, uncached
        for (uint64_t off = 0; off < E1000_MMIO_SIZE; off += 0x1000)
            map_page(base + off, base + off, MMU_PRESENT | MMU_WRITABLE | MMU_CACHE_DISABLE);
    }

    // the I/O window (IOADDR/IODATA) is the first I/O BAR
    for (uint8_t bar = PCI_BAR1; bar <= PCI_BAR5; bar += 4) {
        const uint32_t value = pci_config_read_u32(pci->bus, pci->device, pci->function, bar);
        if (value & 1) {
            dev->io_base = value & ~3;
            break;
        }
    }
    if (dev->bar_type == 1)
        dev->io_base = bar0 & ~3;

    pci_enable_bus_master(&dev->pci);

    // reset, then mask everything until the rings exist
    e1000_write_command(dev, REG_CTRL, e1000_read_command(dev, REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 100000 && (e1000_read_command(dev, REG_CTRL) & CTRL_RST); i++) {}
    e1000_disable_interrupts(dev);

    dev->eeprom_exists = false;
    return dev;
}


void e1000_write_command(const struct e1000_device* dev, const uint16_t address, const uint32_t value)
{
    if (dev->bar_type == 0) {
        *(volatile uint32_t*)(uintptr_t)(dev->mem_base + address) = value;
    } else {
        port_write_u32(dev->io_base, address);
        port_write_u32(dev->io_base + 4, value);
    }
}

uint32_t e1000_read_command(const struct e1000_device* dev, const uint16_t address)
{
    if (dev->bar_type == 0) {
        return *(volatile uint32_t*)(uintptr_t)(dev->mem_base + address);
    }
    port_write_u32(dev->io_base, address);
    return port_read_u32(dev->io_base + 4);
}

bool e1000_detect_eeprom(struct e1000_device* dev)
{
    e1000_write_command(dev, REG_EEPROM, 0x1);

    for(int i = 0; i < 1000 && !dev->eeprom_exists; i++)
    {
        if (e1000_read_command(dev, REG_EEPROM) & 0x10)
            dev->eeprom_exists = true;
        else
            dev->eeprom_exists = false;
    }

    return dev->eeprom_exists;
}

uint16_t e1000_eeprom_read(const struct e1000_device* dev, const uint8_t address)
{
    uint16_t data = 0;
    uint32_t tmp = 0;
    if ( dev->eeprom_exists)
    {
        e1000_write_command(dev, REG_EEPROM, (1) | ((uint32_t)(address) << 8) );
        while( !((tmp = e1000_read_command(dev, REG_EEPROM)) & (1 << 4)) );
    }
    else
    {
        e1000_write_command(dev, REG_EEPROM, (1) | ((uint32_t)(address) << 2) );
        while( !((tmp = e1000_read_command(dev, REG_EEPROM)) & (1 << 1)) );
    }
    data = (uint16_t)(tmp >> 16 & 0xFFFF);
    return data;
}

bool e1000_read_mac_address(struct e1000_device* dev)
{
    if ( dev->eeprom_exists)
    {
        uint32_t temp = e1000_eeprom_read(dev, 0);
        dev->mac[0] = temp &0xff;
        dev->mac[1] = temp >> 8;
        temp = e1000_eeprom_read(dev, 1);
        dev->mac[2] = temp &0xff;
        dev->mac[3] = temp >> 8;
        temp = e1000_eeprom_read(dev, 2);
        dev->mac[4] = temp &0xff;
        dev->mac[5] = temp >> 8;
    }
    else
    {
        // no EEPROM (e1000e): firmware left the address in receive address register 0
        const uint32_t low = e1000_read_command(dev, REG_RAL);
        const uint32_t high = e1000_read_command(dev, REG_RAH);
        if (low == 0)
            return false;
        for (int i = 0; i < 4; i++)
            dev->mac[i] = low >> (8 * i);
        dev->mac[4] = high & 0xff;
        dev->mac[5] = (high >> 8) & 0xff;
    }

    // program it as the unicast filter address
    e1000_write_command(dev, REG_RAL, dev->mac[0] | dev->mac[1] << 8 | dev->mac[2] << 16 | (uint32_t)dev->mac[3] << 24);
    e1000_write_command(dev, REG_RAH, dev->mac[4] | dev->mac[5] << 8 | RAH_AV);
    return true;
}

bool e1000_start_link(struct e1000_device* dev)
{
    e1000_detect_eeprom(dev);
    if (!e1000_read_mac_address(dev))
        return false;

    // set link up with auto speed detection, take the PHY and link out of reset
    uint32_t ctrl = e1000_read_command(dev, REG_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS | CTRL_VME);
    e1000_write_command(dev, REG_CTRL, ctrl);

    for(int i = 0; i < 0x80; i++)
        e1000_write_command(dev, REG_MTA + i*4, 0);

    e1000_rx_init(dev);
    e1000_tx_init(dev);

    // without an IRQ the driver still works when polled (e1000_handle_receive / e1000_reclaim_tx)
    if (!dev->irq_registered && irq_register_handler(dev->pci.interrupt_line, e1000_interrupt, dev) == 0)
        dev->irq_registered = true;
    if (dev->irq_registered)
        e1000_enable_interrupts(dev);
    return true;
}

bool e1000_link_up(struct e1000_device* dev)
{
    return e1000_read_command(dev, REG_STATUS) & STATUS_LU;
}

void e1000_rx_init(struct e1000_device* dev)
{
    // base 16-byte aligned, length a multiple of 128
    auto descs = (struct e1000_rx_desc*)malloc_aligned(sizeof(struct e1000_rx_desc) * E1000_NUM_RX_DESC, 128);
    for(int i = 0; i < E1000_NUM_RX_DESC; i++)
    {
        dev->rx_buffers[i] = malloc_aligned(E1000_RX_BUF_SIZE, 16);
        dev->rx_descs[i] = &descs[i];
        dev->rx_descs[i]->addr = dma_addr(dev->rx_buffers[i]);
        dev->rx_descs[i]->status = 0;
    }

    e1000_write_command(dev, REG_RXDESCLO, (uint32_t)(dma_addr(descs) & 0xFFFFFFFF));
    e1000_write_command(dev, REG_RXDESCHI, (uint32_t)(dma_addr(descs) >> 32));

    e1000_write_command(dev, REG_RXDESCLEN, E1000_NUM_RX_DESC * 16);

    e1000_write_command(dev, REG_RXDESCHEAD, 0);
    e1000_write_command(dev, REG_RXDESCTAIL, E1000_NUM_RX_DESC-1);
    dev->rx_cur = 0;
    // unicast to our address, broadcast, and multicast through the MTA; promiscuous via ioctl
    e1000_write_command(dev, REG_RCTRL, RCTL_EN | RCTL_LBM_NONE | RCTL_RDMTS_HALF | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
}

void e1000_tx_init(struct e1000_device* dev)
{
    auto descs = (struct e1000_tx_desc*)malloc_aligned(sizeof(struct e1000_tx_desc) * E1000_NUM_TX_DESC, 128);

    for(int i = 0; i < E1000_NUM_TX_DESC; i++)
    {
        dev->tx_descs[i] = &descs[i];
        dev->tx_descs[i]->addr = 0;
        dev->tx_descs[i]->cmd = 0;
        dev->tx_descs[i]->status = TSTA_DD;
        dev->tx_netbufs[i] = nullptr;
    }

    e1000_write_command(dev, REG_TXDESCLO, (uint32_t)(dma_addr(descs) & 0xFFFFFFFF));
    e1000_write_command(dev, REG_TXDESCHI, (uint32_t)(dma_addr(descs) >> 32));

    e1000_write_command(dev, REG_TXDESCLEN, E1000_NUM_TX_DESC * 16);

    e1000_write_command(dev, REG_TXDESCHEAD, 0);
    e1000_write_command(dev, REG_TXDESCTAIL, 0);
    dev->tx_cur = 0;
    dev->tx_clean = 0;

    if (dev->pci.device_id == E1000_I217 || dev->pci.device_id == E1000_82577LM) {
        // packets are not sent on the I217 and 82577LM unless TCTL is configured with these bits
        e1000_write_command(dev, REG_TCTRL,  0b0110000000000111111000011111010);
    } else {
        e1000_write_command(dev, REG_TCTRL,  TCTL_EN
            | TCTL_PSP
            | (15 << TCTL_CT_SHIFT)
            | (64 << TCTL_COLD_SHIFT)
            | TCTL_RTLC);
    }
    e1000_write_command(dev, REG_TIPG,  0x0060200A);
}

void e1000_enable_interrupts(const struct e1000_device* dev)
{
    e1000_write_command(dev, REG_IMASK, ICR_TXDW | ICR_LSC | ICR_RXDMT0 | ICR_RXO | ICR_RXT0);
    e1000_read_command(dev, REG_ICR);
}

void e1000_disable_interrupts(struct e1000_device* dev)
{
    e1000_write_command(dev, REG_IMC, 0xFFFFFFFF);
    e1000_read_command(dev, REG_ICR);
}

void e1000_interrupt(uint8_t irq, void* ctx)
{
    auto dev = (struct e1000_device*)ctx;
    if (irq != dev->pci.interrupt_line)
        return;

    // reading ICR acknowledges every cause; the line may be shared, so 0 means "not us"
    const uint32_t status = e1000_read_command(dev, REG_ICR);
    if (status & ICR_LSC)
        netif_link_state_change(dev->nif, e1000_link_up(dev) ? NETIF_LINK_UP : NETIF_LINK_DOWN);
    if (status & (ICR_RXT0 | ICR_RXDMT0 | ICR_RXO))
        e1000_handle_receive(dev);
    if (status & ICR_TXDW)
        e1000_reclaim_tx(dev);
}

void e1000_handle_receive(struct e1000_device* dev)
{
    struct netif* nif = dev->nif;

    while (dev->rx_descs[dev->rx_cur]->status & RX_STATUS_DD)
    {
        volatile struct e1000_rx_desc* desc = dev->rx_descs[dev->rx_cur];
        const uint8_t* buf = dev->rx_buffers[dev->rx_cur];
        const uint16_t len = desc->length;

        // a 2 KiB buffer holds any non-jumbo frame, so EOP is always set on good frames
        if (desc->errors || !(desc->status & RX_STATUS_EOP)) {
            nif->stats.rx_errors++;
        } else {
            // copy out so the descriptor buffer can be handed straight back to the NIC
            struct netbuf* nb = netbuf_alloc(true);
            if (nb && netbuf_append(nb, buf, len)) {
                nb->rcvif = nif;
                if (buf[0] & 1)
                    nb->flags |= buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF ? NETBUF_BCAST : NETBUF_MCAST;
                nif->stats.rx_packets++;
                nif->stats.rx_bytes += len;
                netif_input(nif, nb);
            } else {
                if (nb)
                    netbuf_free_chain(nb);
                nif->stats.rx_dropped++;
            }
        }

        desc->status = 0;
        const uint16_t old_cur = dev->rx_cur;
        dev->rx_cur = (dev->rx_cur + 1) % E1000_NUM_RX_DESC;
        e1000_write_command(dev, REG_RXDESCTAIL, old_cur);
    }
}

void e1000_reclaim_tx(struct e1000_device* dev)
{
    while (dev->tx_clean != dev->tx_cur && (dev->tx_descs[dev->tx_clean]->status & TSTA_DD))
    {
        // the chain is stored on its last descriptor, so it is freed once all of it is sent
        if (dev->tx_netbufs[dev->tx_clean]) {
            netbuf_free_chain(dev->tx_netbufs[dev->tx_clean]);
            dev->tx_netbufs[dev->tx_clean] = nullptr;
        }
        dev->tx_clean = (dev->tx_clean + 1) % E1000_NUM_TX_DESC;
    }
}

static uint16_t e1000_tx_free(const struct e1000_device* dev)
{
    // one slot stays empty so a full ring can be told apart from an empty one
    const uint16_t used = (dev->tx_cur + E1000_NUM_TX_DESC - dev->tx_clean) % E1000_NUM_TX_DESC;
    return E1000_NUM_TX_DESC - 1 - used;
}

int e1000_netif_init(struct netif* nif)
{
    struct e1000_device* dev = (struct e1000_device*)nif->softc;
    if (!dev || !dev->rx_descs[0] || !dev->tx_descs[0])
        return -EIO;
    e1000_write_command(dev, REG_RCTRL, e1000_read_command(dev, REG_RCTRL) | RCTL_EN);
    return 0;
}

int e1000_transmit(struct netif* nif, struct netbuf* nb)
{
    const auto dev = (struct e1000_device*)nif->softc;

    size_t length = 0;
    uint16_t segments = 0;
    for (const struct netbuf* b = nb; b; b = b->next) {
        if (b->len) {
            length += b->len;
            segments++;
        }
    }
    if (length == 0 || length > E1000_MAX_FRAME) {
        netbuf_free_chain(nb);
        nif->stats.tx_errors++;
        return -EMSGSIZE;
    }

    e1000_reclaim_tx(dev);
    if (e1000_tx_free(dev) < segments) {
        netbuf_free_chain(nb);
        nif->stats.tx_dropped++;
        return -ENOBUFS;
    }

    uint16_t last = dev->tx_cur;
    for (const struct netbuf* b = nb; b; b = b->next) {
        if (!b->len)
            continue;
        last = dev->tx_cur;
        volatile struct e1000_tx_desc* desc = dev->tx_descs[last];
        desc->addr = dma_addr(b->data);
        desc->length = b->len;
        desc->cmd = CMD_IFCS | CMD_RS;
        desc->status = 0;
        dev->tx_netbufs[last] = nullptr;
        dev->tx_cur = (dev->tx_cur + 1) % E1000_NUM_TX_DESC;
    }
    dev->tx_descs[last]->cmd |= CMD_EOP;
    dev->tx_netbufs[last] = nb;

    nif->stats.tx_packets++;
    nif->stats.tx_bytes += length;
    e1000_write_command(dev, REG_TXDESCTAIL, dev->tx_cur);
    return 0;
}

int e1000_ioctl(struct netif* nif, const uint32_t cmd, void* arg)
{
    const auto dev = (struct e1000_device*)nif->softc;

    switch (cmd) {
    case NETIF_IOCTL_SET_FLAGS: {
        const uint32_t flags = *(const uint32_t*)arg;
        uint32_t rctl = e1000_read_command(dev, REG_RCTRL);
        if (flags & NETIF_PROMISC)
            rctl |= RCTL_UPE | RCTL_MPE;
        else
            rctl &= ~(RCTL_UPE | RCTL_MPE);
        e1000_write_command(dev, REG_RCTRL, rctl);
        nif->flags = flags;
        return 0;
    }
    case NETIF_IOCTL_SET_MTU:
        return *(const uint32_t*)arg <= E1000_MAX_FRAME - 14 ? 0 : -EINVAL;
    case NETIF_IOCTL_ADD_MULTI: {
        const uint8_t* mac = arg;
        const uint32_t hash = ((mac[4] >> 4) | ((uint32_t)mac[5] << 4)) & 0xFFF;
        const uint16_t reg = REG_MTA + (hash >> 5) * 4;
        e1000_write_command(dev, reg, e1000_read_command(dev, reg) | (1u << (hash & 31)));
        return 0;
    }
    case NETIF_IOCTL_DEL_MULTI:
        return 0;
    case NETIF_IOCTL_GET_MEDIA:
        *(uint32_t*)arg = e1000_link_up(dev) ? NETIF_LINK_UP : NETIF_LINK_DOWN;
        return 0;
    default:
        return -EINVAL;
    }
}

void e1000_qflush(struct netif* nif)
{
    netqueue_drain(&nif->send_queue);
    e1000_reclaim_tx(nif->softc);
}
