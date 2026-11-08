#include "virtio_net.h"

#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netif.h>
#include <drivers/net/core/netisr.h>
#include <drivers/net/core/netqueue.h>
#include <drivers/net/ethernet/ethernet.h>
#include <drivers/terminal/terminal.h>
#include <drivers/timer/callout.h>
#include <hardware/interrupts/interrupts.h>
#include <hardware/interrupts/irq.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>
#include <hardware/smp/smp.h>
#include <hardware/virtio/virtio.h>
#include <os/errno.h>
#include <utils/str/str.h>
#include <utils/sync/spinlock.h>

#define VIRTIO_NET_LEGACY_ID 0x1000

#define VNET_F_GUEST_CSUM    (1u << 1)
#define VNET_F_MAC           (1u << 5)
#define VNET_F_STATUS        (1u << 16)

#define VNET_S_LINK_UP       1
#define VNET_HDR_F_NEEDS_CSUM 1
#define VNET_HDR_F_DATA_VALID 2

#define VNET_RXQ             0
#define VNET_TXQ             1
#define VNET_BUF_SIZE        2048
#define VNET_MAX_DEVS        4
#define VNET_TX_BATCH        32
#define VNET_TX_OWNER_ROUNDS 4

struct vnet_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

struct vnet_device {
    struct virtio_device vdev;
    struct netif* nif;
    uint8_t mac[ETH_ADDR_LEN];

    struct virtqueue rxq;
    struct virtqueue txq;
    uint8_t* rx_bufs;
    uint8_t* tx_bufs;

    spinlock_t rx_lock;
    bool rx_sched;
    bool rx_polling;

    struct netbuf* tx_pending;
    uint32_t tx_pending_count;
    bool tx_busy;
    struct callout tx_kick;
};

static struct vnet_device* vnet_devs[VNET_MAX_DEVS];
static uint32_t vnet_pollers;

static uint8_t* vnet_buf(uint8_t* pool, const uint16_t slot)
{
    return pool + (uint64_t)slot * VNET_BUF_SIZE;
}

static void vnet_rx_post(struct vnet_device* dev)
{
    const uint16_t slot = dev->rxq.free_head;
    const struct virtio_buf buf = {virt_to_phys(vnet_buf(dev->rx_bufs, slot)), VNET_BUF_SIZE, true};
    virtqueue_add(&dev->rxq, &buf, 1);
}

static unsigned vnet_rx(struct vnet_device* dev, const unsigned budget)
{
    struct netif* nif = dev->nif;
    unsigned done = 0;
    uint16_t head;
    uint32_t len;

    uint64_t flags;
    spinlock_acquire_irqsave(&dev->rx_lock, &flags);
    while (done < budget && virtqueue_get(&dev->rxq, &head, &len)) {
        const uint8_t* buf = vnet_buf(dev->rx_bufs, head);
        const auto hdr = (const struct vnet_hdr*)buf;
        const uint8_t* frame = buf + sizeof(struct vnet_hdr);
        done++;

        if (len < sizeof(struct vnet_hdr) + ETH_HEADER_LEN || len > VNET_BUF_SIZE) {
            nif->stats.rx_errors++;
        } else {
            const uint32_t flen = len - sizeof(struct vnet_hdr);
            struct netbuf* nb = netbuf_alloc(true);
            if (nb && netbuf_append(nb, frame, flen)) {
                nb->rcvif = nif;
                if (frame[0] & 1)
                    nb->flags |= frame[0] == 0xFF && frame[1] == 0xFF && frame[2] == 0xFF ? NETBUF_BCAST : NETBUF_MCAST;
                if (hdr->flags & (VNET_HDR_F_NEEDS_CSUM | VNET_HDR_F_DATA_VALID))
                    nb->csum_flags |= NETBUF_CSUM_L4_VALID;
                nif->stats.rx_packets++;
                nif->stats.rx_bytes += flen;
                netif_input(nif, nb);
            } else {
                if (nb)
                    netbuf_free_chain(nb);
                nif->stats.rx_dropped++;
            }
        }
        vnet_rx_post(dev);
    }
    if (done)
        virtqueue_kick(&dev->rxq);
    spinlock_release_irqrestore(&dev->rx_lock, flags);
    return done;
}

static bool vnet_rx_softintr(const unsigned budget)
{
    __atomic_add_fetch(&vnet_pollers, 1, __ATOMIC_SEQ_CST);
    bool more = false;
    for (int i = 0; i < VNET_MAX_DEVS; i++) {
        struct vnet_device* dev = __atomic_load_n(&vnet_devs[i], __ATOMIC_SEQ_CST);
        if (!dev)
            continue;
        unsigned done = 0;
        bool full = false;
        while (!full && __atomic_load_n(&dev->rx_sched, __ATOMIC_SEQ_CST)
               && !__atomic_exchange_n(&dev->rx_polling, true, __ATOMIC_ACQUIRE)) {
            if (__atomic_exchange_n(&dev->rx_sched, false, __ATOMIC_ACQUIRE)) {
                done += vnet_rx(dev, budget - done);
                if (done >= budget) {
                    __atomic_store_n(&dev->rx_sched, true, __ATOMIC_RELEASE);
                    full = true;
                } else if (virtqueue_intr_enable(&dev->rxq)) {
                    virtqueue_intr_disable(&dev->rxq);
                    __atomic_store_n(&dev->rx_sched, true, __ATOMIC_RELEASE);
                }
            }
            __atomic_store_n(&dev->rx_polling, false, __ATOMIC_SEQ_CST);
        }
        more |= full;
    }
    __atomic_sub_fetch(&vnet_pollers, 1, __ATOMIC_SEQ_CST);
    return more;
}

static bool vnet_link_up(const struct vnet_device* dev)
{
    if (!(dev->vdev.features & VNET_F_STATUS))
        return true;
    return virtio_config_u16(&dev->vdev, ETH_ADDR_LEN) & VNET_S_LINK_UP;
}

static void vnet_config_changed(struct vnet_device* dev)
{
    const bool locked = net_lock_enter();
    netif_link_state_change(dev->nif, vnet_link_up(dev) ? NETIF_LINK_UP : NETIF_LINK_DOWN);
    if (locked)
        net_lock_exit();
}

static void vnet_rx_wake(struct vnet_device* dev)
{
    virtqueue_intr_disable(&dev->rxq);
    __atomic_store_n(&dev->rx_sched, true, __ATOMIC_RELEASE);
    netisr_rx_schedule();
}

static void vnet_interrupt(const uint8_t irq, void* ctx)
{
    struct vnet_device* dev = ctx;
    if (irq != dev->vdev.irq)
        return;
    const uint8_t isr = virtio_isr(&dev->vdev);
    if (isr & VIRTIO_ISR_CONFIG)
        vnet_config_changed(dev);
    if (isr & VIRTIO_ISR_QUEUE)
        vnet_rx_wake(dev);
}

static void vnet_msix_config(const uint8_t irq, void* ctx)
{
    (void)irq;
    vnet_config_changed(ctx);
}

static void vnet_msix_rx(const uint8_t irq, void* ctx)
{
    (void)irq;
    vnet_rx_wake(ctx);
}

static bool vnet_upgrade_msix(struct vnet_device* dev)
{
    struct virtio_device* v = &dev->vdev;
    if (virtio_msix_prepare(v, 2) != 0)
        return false;
    if (irq_register_handler_mpsafe(v->msix_irq[0], vnet_msix_config, dev) != 0) {
        virtio_msix_disable(v);
        return false;
    }
    if (irq_register_handler_mpsafe(v->msix_irq[1], vnet_msix_rx, dev) != 0) {
        irq_unregister_handler(v->msix_irq[0], vnet_msix_config, dev);
        virtio_msix_disable(v);
        return false;
    }
    virtio_msix_activate(v);
    const bool ok = virtio_msix_config_vector(v, 0) && virtio_msix_queue_vector(v, VNET_RXQ, 1)
        && virtio_msix_queue_vector(v, VNET_TXQ, VIRTIO_MSI_NO_VECTOR);
    if (!ok) {
        virtio_msix_config_vector(v, VIRTIO_MSI_NO_VECTOR);
        virtio_msix_queue_vector(v, VNET_RXQ, VIRTIO_MSI_NO_VECTOR);
        irq_unregister_handler(v->msix_irq[1], vnet_msix_rx, dev);
        irq_unregister_handler(v->msix_irq[0], vnet_msix_config, dev);
        virtio_msix_disable(v);
    } else {
        irq_unregister_handler(v->irq, vnet_interrupt, dev);
    }
    vnet_rx_wake(dev);
    return ok;
}

static void vnet_upgrade_all()
{
    for (int i = 0; i < VNET_MAX_DEVS; i++) {
        struct vnet_device* dev = __atomic_load_n(&vnet_devs[i], __ATOMIC_SEQ_CST);
        if (dev && vnet_upgrade_msix(dev))
            terminal_printf("virtio-net%d: msi-x vectors %u/%u\n", i, IRQ_OFFSET + dev->vdev.msix_irq[0],
                            IRQ_OFFSET + dev->vdev.msix_irq[1]);
    }
}

static void vnet_tx_reclaim(struct vnet_device* dev)
{
    uint16_t head;
    while (virtqueue_get(&dev->txq, &head, nullptr)) {}
}

static void vnet_tx_enqueue(struct vnet_device* dev, struct netbuf* nb)
{
    struct netif* nif = dev->nif;
    size_t length = 0;
    for (const struct netbuf* b = nb; b; b = b->next)
        length += b->len;

    if (!dev->txq.num_free) {
        virtqueue_kick(&dev->txq);
        vnet_tx_reclaim(dev);
    }
    if (!dev->txq.num_free) {
        netbuf_free_chain(nb);
        nif->stats.tx_dropped++;
        return;
    }

    const uint16_t slot = dev->txq.free_head;
    uint8_t* buf = vnet_buf(dev->tx_bufs, slot);
    memset(buf, 0, sizeof(struct vnet_hdr));
    netbuf_copydata(nb, 0, length, buf + sizeof(struct vnet_hdr));
    netbuf_free_chain(nb);

    const struct virtio_buf vb = {virt_to_phys(buf), (uint32_t)(sizeof(struct vnet_hdr) + length), false};
    virtqueue_add(&dev->txq, &vb, 1);
    nif->stats.tx_packets++;
    nif->stats.tx_bytes += length;
}

static void vnet_tx_flush(struct netif* nif);

static void vnet_tx_kick(void* arg)
{
    vnet_tx_flush(arg);
}

static void vnet_tx_flush(struct netif* nif)
{
    struct vnet_device* dev = nif->softc;
    for (int round = 0;; round++) {
        if (!__atomic_load_n(&dev->tx_pending, __ATOMIC_SEQ_CST))
            return;
        if (round == VNET_TX_OWNER_ROUNDS) {
            callout_reset(&dev->tx_kick, 1, vnet_tx_kick, nif);
            return;
        }
        const uint64_t flags = irq_save();
        if (__atomic_exchange_n(&dev->tx_busy, true, __ATOMIC_ACQUIRE)) {
            irq_restore(flags);
            return;
        }

        struct netbuf* nb = __atomic_exchange_n(&dev->tx_pending, nullptr, __ATOMIC_ACQUIRE);
        struct netbuf* ordered = nullptr;
        uint32_t count = 0;
        while (nb) {
            struct netbuf* next = nb->next_pkt;
            nb->next_pkt = ordered;
            ordered = nb;
            nb = next;
            count++;
        }
        __atomic_sub_fetch(&dev->tx_pending_count, count, __ATOMIC_RELAXED);

        vnet_tx_reclaim(dev);
        while (ordered) {
            struct netbuf* next = ordered->next_pkt;
            ordered->next_pkt = nullptr;
            vnet_tx_enqueue(dev, ordered);
            ordered = next;
        }
        virtqueue_kick(&dev->txq);

        __atomic_store_n(&dev->tx_busy, false, __ATOMIC_SEQ_CST);
        irq_restore(flags);
    }
}

static int vnet_transmit(struct netif* nif, struct netbuf* nb)
{
    struct vnet_device* dev = nif->softc;

    size_t length = 0;
    for (const struct netbuf* b = nb; b; b = b->next)
        length += b->len;
    if (length == 0 || length > ETH_MAX_LEN) {
        netbuf_free_chain(nb);
        nif->stats.tx_errors++;
        return -EMSGSIZE;
    }

    const uint32_t pending = __atomic_add_fetch(&dev->tx_pending_count, 1, __ATOMIC_RELAXED);
    nb->next_pkt = __atomic_load_n(&dev->tx_pending, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(&dev->tx_pending, &nb->next_pkt, nb, true, __ATOMIC_SEQ_CST,
                                        __ATOMIC_RELAXED))
        ;
    if (pending >= VNET_TX_BATCH || !netif_tx_defer(nif))
        vnet_tx_flush(nif);
    return 0;
}

static void vnet_tx_try_reclaim(struct netif* nif)
{
    struct vnet_device* dev = nif->softc;
    const uint64_t flags = irq_save();
    if (!__atomic_exchange_n(&dev->tx_busy, true, __ATOMIC_ACQUIRE)) {
        vnet_tx_reclaim(dev);
        __atomic_store_n(&dev->tx_busy, false, __ATOMIC_SEQ_CST);
    }
    irq_restore(flags);
    vnet_tx_flush(nif);
}

static int vnet_netif_init(struct netif* nif)
{
    return nif->softc ? 0 : -EIO;
}

static int vnet_ioctl(struct netif* nif, const uint32_t cmd, void* arg)
{
    const struct vnet_device* dev = nif->softc;
    switch (cmd) {
    case NETIF_IOCTL_SET_FLAGS:
        nif->flags = *(const uint32_t*)arg;
        return 0;
    case NETIF_IOCTL_SET_MTU:
        return *(const uint32_t*)arg <= ETH_MTU ? 0 : -EINVAL;
    case NETIF_IOCTL_ADD_MULTI:
    case NETIF_IOCTL_DEL_MULTI:
        return 0;
    case NETIF_IOCTL_GET_MEDIA:
        *(uint32_t*)arg = vnet_link_up(dev) ? NETIF_LINK_UP : NETIF_LINK_DOWN;
        return 0;
    default:
        return -EINVAL;
    }
}

static void vnet_qflush(struct netif* nif)
{
    netqueue_drain(&nif->send_queue);
    vnet_tx_try_reclaim(nif);
}

static void vnet_poll(struct netif* nif)
{
    vnet_rx(nif->softc, ~0u);
    vnet_tx_try_reclaim(nif);
}

static const struct netif_ops vnet_netif_ops = {
    .init = vnet_netif_init,
    .transmit = vnet_transmit,
    .tx_flush = vnet_tx_flush,
    .ioctl = vnet_ioctl,
    .qflush = vnet_qflush,
    .poll = vnet_poll,
};

static uint8_t* vnet_alloc_pool(const uint16_t slots)
{
    const uint64_t bytes = PAGE_ALIGN_UP((uint64_t)slots * VNET_BUF_SIZE);
    uint8_t* pool = alloc_kernel_pages(bytes / PAGE_SIZE);
    if (pool)
        memset(pool, 0, bytes);
    return pool;
}

static void vnet_probe(const pci_address addr)
{
    int index = -1;
    for (int i = 0; i < VNET_MAX_DEVS && index < 0; i++)
        if (!vnet_devs[i])
            index = i;
    if (index < 0)
        return;

    struct vnet_device* dev = malloc_safe(sizeof(struct vnet_device));
    if (!dev)
        return;
    if (!virtio_attach(&dev->vdev, addr)) {
        free(dev);
        return;
    }
    spinlock_init(&dev->rx_lock, "virtio-net-rx");
    callout_init(&dev->tx_kick);
    dev->tx_kick.mpsafe = true;

    const uint32_t features =
        virtio_negotiate(&dev->vdev, VNET_F_MAC | VNET_F_STATUS | VNET_F_GUEST_CSUM | VIRTIO_F_ANY_LAYOUT);
    if (!(features & VIRTIO_F_ANY_LAYOUT) || !virtqueue_setup(&dev->vdev, &dev->rxq, VNET_RXQ)
        || !virtqueue_setup(&dev->vdev, &dev->txq, VNET_TXQ)
        || !(dev->rx_bufs = vnet_alloc_pool(dev->rxq.size)) || !(dev->tx_bufs = vnet_alloc_pool(dev->txq.size))) {
        virtio_fail(&dev->vdev);
        terminal_printf("virtio-net: setup failed\n");
        return;
    }

    for (int i = 0; i < ETH_ADDR_LEN; i++)
        dev->mac[i] = features & VNET_F_MAC ? virtio_config_u8(&dev->vdev, (uint16_t)i) : (uint8_t)(i ? 0x00 : 0x02);

    while (dev->rxq.num_free)
        vnet_rx_post(dev);
    virtqueue_intr_disable(&dev->txq);

    struct netif* nif = netif_alloc(NETIF_TYPE_ETHERNET, "vtnet");
    if (!nif) {
        virtio_fail(&dev->vdev);
        return;
    }
    nif->softc = dev;
    nif->ops = &vnet_netif_ops;
    dev->nif = nif;

    if (dev->vdev.irq >= IRQ_ISA_LINES || irq_register_handler_mpsafe(dev->vdev.irq, vnet_interrupt, dev) != 0) {
        virtio_fail(&dev->vdev);
        netif_free(nif);
        terminal_printf("virtio-net: no usable irq\n");
        return;
    }
    virtio_driver_ok(&dev->vdev);
    virtqueue_kick(&dev->rxq);

    eth_ifattach(nif, dev->mac);
    netif_link_state_change(nif, vnet_link_up(dev) ? NETIF_LINK_UP : NETIF_LINK_DOWN);
    __atomic_store_n(&vnet_devs[index], dev, __ATOMIC_SEQ_CST);
    __atomic_store_n(&dev->rx_sched, true, __ATOMIC_SEQ_CST);
    terminal_printf("virtio-net%d: %02x:%02x:%02x:%02x:%02x:%02x, queues %u/%u, %s %u\n", index, dev->mac[0],
                    dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5], dev->rxq.size, dev->txq.size,
                    "irq", dev->vdev.irq);
}

void virtio_net_initialize()
{
    netisr_register_poller(vnet_rx_softintr);
    irq_on_msi_ready(vnet_upgrade_all);
    virtio_pci_foreach(VIRTIO_NET_LEGACY_ID, vnet_probe);
}
