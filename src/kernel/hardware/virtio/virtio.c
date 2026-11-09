#include "virtio.h"

#include <hardware/interrupts/irq.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>
#include <hardware/port/ports.h>
#include <os/errno.h>
#include <utils/str/str.h>

#define MSIX_CTRL_ENABLE 0x8000
#define MSIX_CTRL_MASK   0x4000
#define MSIX_ENTRY_WORDS 4



void virtio_pci_foreach(const uint16_t device_id, const virtio_probe_t probe)
{
    const scan_result* result = pci_scan(-1);
    for (uint32_t i = 0; i < result->count; i++) {
        if (result->devices[i] == 0)
            continue;
        const pci_address addr = pci_convert_address(result->devices[i]);
        if (pci_config_read_u16(addr.bus, addr.device, addr.function, PCI_VENDOR_ID) == VIRTIO_VENDOR
            && pci_config_read_u16(addr.bus, addr.device, addr.function, PCI_DEVICE_ID) == device_id)
            probe(addr);
    }
}

bool virtio_attach(struct virtio_device* dev, const pci_address addr)
{
    const uint32_t bar0 = pci_config_read_u32(addr.bus, addr.device, addr.function, PCI_BAR0);
    if (!(bar0 & 1))
        return false;
    dev->addr = addr;
    dev->io = (uint16_t)(bar0 & ~3u);
    dev->irq = pci_config_read_u8(addr.bus, addr.device, addr.function, PCI_INTERRUPT_LINE);
    dev->features = 0;
    dev->config = VIO_CONFIG;
    dev->msix_cap = 0;
    dev->msix_count = 0;

    const uint16_t command = pci_config_read_u16(addr.bus, addr.device, addr.function, PCI_COMMAND);
    pci_config_write_u16(addr.bus, addr.device, addr.function, PCI_COMMAND, command | PCI_COMMAND_MASTER | 1);

    port_write_u8(dev->io + VIO_STATUS, 0);
    port_write_u8(dev->io + VIO_STATUS, VIRTIO_STATUS_ACK);
    port_write_u8(dev->io + VIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    return true;
}

uint32_t virtio_negotiate(struct virtio_device* dev, const uint32_t wanted)
{
    dev->features = port_read_u32(dev->io + VIO_HOST_FEATURES) & wanted;
    port_write_u32(dev->io + VIO_GUEST_FEATURES, dev->features);
    return dev->features;
}

static void msix_free_irqs(struct virtio_device* dev)
{
    for (uint16_t i = 0; i < dev->msix_count; i++)
        irq_msi_free(dev->msix_irq[i]);
    dev->msix_count = 0;
}

int virtio_msix_prepare(struct virtio_device* dev, const uint16_t count)
{
    const pci_address a = dev->addr;
    const uint8_t cap = pci_find_capability(a.bus, a.device, a.function, PCI_CAP_MSIX);
    if (!cap || count == 0 || count > VIRTIO_MSIX_MAX)
        return -ENODEV;
    const uint16_t ctrl = pci_config_read_u16(a.bus, a.device, a.function, cap + 2);
    if ((ctrl & 0x7FF) + 1u < count)
        return -ENOSPC;

    const uint32_t table = pci_config_read_u32(a.bus, a.device, a.function, cap + 4);
    const uint8_t bar_reg = PCI_BAR0 + 4 * (table & 7);
    const uint32_t bar = pci_config_read_u32(a.bus, a.device, a.function, bar_reg);
    if (bar & 1)
        return -ENODEV;
    uint64_t phys = bar & ~0xFull;
    if (((bar >> 1) & 3) == 2)
        phys |= (uint64_t)pci_config_read_u32(a.bus, a.device, a.function, bar_reg + 4) << 32;
    phys += table & ~7u;

    const uint64_t page = phys & ~(uint64_t)(PAGE_SIZE - 1);
    const uint64_t span = PAGE_ALIGN_UP(phys - page + 16ull * count);
    const uint64_t virt = vmm_alloc_mmio_region(span);
    if (!virt)
        return -ENOMEM;
    for (uint64_t off = 0; off < span; off += PAGE_SIZE) {
        if (map_page(virt + off, page + off, MMU_PRESENT | MMU_WRITABLE | MMU_CACHE_DISABLE) < 0) {
            vmm_free_mmio_region(virt, span);
            return -ENOMEM;
        }
    }
    volatile uint32_t* entries = (volatile uint32_t*)(virt + (phys - page));

    dev->msix_count = 0;
    for (uint16_t i = 0; i < count; i++) {
        const int irq = irq_msi_alloc();
        if (irq < 0) {
            msix_free_irqs(dev);
            vmm_free_mmio_region(virt, span);
            return irq;
        }
        dev->msix_irq[dev->msix_count++] = (uint8_t)irq;
        uint64_t address;
        uint32_t data;
        irq_msi_message((uint8_t)irq, &address, &data);
        volatile uint32_t* e = entries + i * MSIX_ENTRY_WORDS;
        e[0] = (uint32_t)address;
        e[1] = (uint32_t)(address >> 32);
        e[2] = data;
        e[3] = 0;
    }

    dev->msix_cap = cap;
    return 0;
}

void virtio_msix_activate(struct virtio_device* dev)
{
    const pci_address a = dev->addr;
    const uint16_t ctrl = pci_config_read_u16(a.bus, a.device, a.function, dev->msix_cap + 2);
    pci_config_write_u16(a.bus, a.device, a.function, dev->msix_cap + 2, (ctrl | MSIX_CTRL_ENABLE) & ~MSIX_CTRL_MASK);
    const uint16_t command = pci_config_read_u16(a.bus, a.device, a.function, PCI_COMMAND);
    pci_config_write_u16(a.bus, a.device, a.function, PCI_COMMAND, command | PCI_COMMAND_INTX_DISABLE);
    dev->config = VIO_CONFIG_MSIX;
}

void virtio_msix_disable(struct virtio_device* dev)
{
    if (!dev->msix_cap)
        return;
    const pci_address a = dev->addr;
    const uint16_t ctrl = pci_config_read_u16(a.bus, a.device, a.function, dev->msix_cap + 2);
    pci_config_write_u16(a.bus, a.device, a.function, dev->msix_cap + 2, ctrl & ~MSIX_CTRL_ENABLE);
    const uint16_t command = pci_config_read_u16(a.bus, a.device, a.function, PCI_COMMAND);
    pci_config_write_u16(a.bus, a.device, a.function, PCI_COMMAND, command & ~PCI_COMMAND_INTX_DISABLE);
    msix_free_irqs(dev);
    dev->msix_cap = 0;
    dev->config = VIO_CONFIG;
}

bool virtio_msix_config_vector(const struct virtio_device* dev, const uint16_t entry)
{
    port_write_u16(dev->io + VIO_MSI_CONFIG_VEC, entry);
    return port_read_u16(dev->io + VIO_MSI_CONFIG_VEC) == entry;
}

bool virtio_msix_queue_vector(const struct virtio_device* dev, const uint16_t queue, const uint16_t entry)
{
    port_write_u16(dev->io + VIO_QUEUE_SEL, queue);
    port_write_u16(dev->io + VIO_MSI_QUEUE_VEC, entry);
    return port_read_u16(dev->io + VIO_MSI_QUEUE_VEC) == entry;
}

bool virtqueue_setup(struct virtio_device* dev, struct virtqueue* vq, const uint16_t index)
{
    port_write_u16(dev->io + VIO_QUEUE_SEL, index);
    const uint16_t size = port_read_u16(dev->io + VIO_QUEUE_NUM);
    if (size == 0)
        return false;

    const uint64_t avail_end = (uint64_t)size * sizeof(struct vring_desc) + 6 + 2ULL * size;
    const uint64_t used_off = PAGE_ALIGN_UP(avail_end);
    const uint64_t total = used_off + PAGE_ALIGN_UP(6 + 8ULL * size);
    uint8_t* ring = alloc_kernel_pages(total / PAGE_SIZE);
    if (!ring)
        return false;
    memset(ring, 0, total);

    vq->dev = dev;
    vq->index = index;
    vq->size = size;
    vq->desc = (struct vring_desc*)ring;
    vq->avail = (volatile struct vring_avail*)(ring + (uint64_t)size * sizeof(struct vring_desc));
    vq->used = (volatile struct vring_used*)(ring + used_off);
    for (uint16_t i = 0; i < size; i++)
        vq->desc[i].next = (uint16_t)(i + 1);
    vq->free_head = 0;
    vq->num_free = size;
    vq->last_used = 0;
    vq->kicked = 0;
    vq->event = dev->features & VIRTIO_RING_F_EVENT_IDX;

    port_write_u32(dev->io + VIO_QUEUE_PFN, (uint32_t)(virt_to_phys(ring) >> 12));
    return true;
}

int virtqueue_add(struct virtqueue* vq, const struct virtio_buf* bufs, const uint16_t count)
{
    if (count == 0 || vq->num_free < count)
        return -1;
    const uint16_t head = vq->free_head;
    uint16_t cur = head;
    for (uint16_t i = 0; i < count; i++) {
        struct vring_desc* d = &vq->desc[cur];
        d->addr = bufs[i].addr;
        d->len = bufs[i].len;
        d->flags = (bufs[i].write ? VRING_DESC_F_WRITE : 0) | (i + 1 < count ? VRING_DESC_F_NEXT : 0);
        if (i + 1 < count)
            cur = d->next;
    }
    vq->free_head = vq->desc[cur].next;
    vq->num_free -= count;

    const uint16_t idx = vq->avail->idx;
    vq->avail->ring[idx % vq->size] = head;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    vq->avail->idx = (uint16_t)(idx + 1);
    return head;
}

void virtqueue_kick(struct virtqueue* vq)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const uint16_t now = vq->avail->idx;
    const uint16_t old = vq->kicked;
    vq->kicked = now;
    bool notify;
    if (vq->event) {
        const uint16_t event = *(const volatile uint16_t*)&vq->used->ring[vq->size];
        notify = (uint16_t)(now - event - 1) < (uint16_t)(now - old);
    } else {
        notify = !(vq->used->flags & VRING_USED_F_NO_NOTIFY);
    }
    if (notify)
        port_write_u16(vq->dev->io + VIO_QUEUE_NOTIFY, vq->index);
}

bool virtqueue_get(struct virtqueue* vq, uint16_t* head, uint32_t* len)
{
    if (!virtqueue_pending(vq))
        return false;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const volatile struct vring_used_elem* e = &vq->used->ring[vq->last_used % vq->size];
    const uint16_t id = (uint16_t)e->id;
    if (len)
        *len = e->len;
    vq->last_used++;
    if (id >= vq->size)
        return virtqueue_get(vq, head, len);

    uint16_t cur = id;
    for (;;) {
        vq->num_free++;
        if (!(vq->desc[cur].flags & VRING_DESC_F_NEXT))
            break;
        cur = vq->desc[cur].next;
    }
    vq->desc[cur].next = vq->free_head;
    vq->free_head = id;
    *head = id;
    return true;
}

bool virtqueue_intr_enable(const struct virtqueue* vq)
{
    if (vq->event) {
        vq->avail->ring[vq->size] = vq->last_used;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    } else {
        __atomic_and_fetch(&vq->avail->flags, (uint16_t)~VRING_AVAIL_F_NO_INTERRUPT, __ATOMIC_SEQ_CST);
    }
    return virtqueue_pending(vq);
}

bool virtqueue_intr_enable_delayed(const struct virtqueue* vq)
{
    if (!vq->event)
        return virtqueue_intr_enable(vq);
    const uint16_t bufs = (uint16_t)((uint16_t)(vq->avail->idx - vq->last_used) * 3 / 4);
    vq->avail->ring[vq->size] = (uint16_t)(vq->last_used + bufs);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return (uint16_t)(vq->used->idx - vq->last_used) > bufs;
}
