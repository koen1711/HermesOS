#ifndef OS_VIRTIO_H
#define OS_VIRTIO_H

#include <os/stdint.h>
#include <os/stdbool.h>
#include <hardware/port/pci.h>

#define VIRTIO_VENDOR              0x1AF4

#define VIRTIO_STATUS_ACK          0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_DRIVER_OK    0x04
#define VIRTIO_STATUS_FAILED       0x80

#define VIRTIO_ISR_QUEUE           0x01
#define VIRTIO_ISR_CONFIG          0x02

#define VIRTIO_F_ANY_LAYOUT        (1u << 27)
#define VIRTIO_RING_F_EVENT_IDX    (1u << 29)

#define VRING_DESC_F_NEXT          1
#define VRING_DESC_F_WRITE         2
#define VRING_AVAIL_F_NO_INTERRUPT 1
#define VRING_USED_F_NO_NOTIFY     1

#define VIO_HOST_FEATURES  0x00
#define VIO_GUEST_FEATURES 0x04
#define VIO_QUEUE_PFN      0x08
#define VIO_QUEUE_NUM      0x0C
#define VIO_QUEUE_SEL      0x0E
#define VIO_QUEUE_NOTIFY   0x10
#define VIO_STATUS         0x12
#define VIO_ISR            0x13
#define VIO_CONFIG         0x14
#define VIO_MSI_CONFIG_VEC 0x14
#define VIO_MSI_QUEUE_VEC  0x16
#define VIO_CONFIG_MSIX    0x18

#define VIRTIO_MSI_NO_VECTOR 0xFFFF
#define VIRTIO_MSIX_MAX      4

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

struct virtio_device {
    pci_address addr;
    uint16_t io;
    uint8_t irq;
    uint32_t features;
    uint16_t config;
    uint16_t msix_cap;
    uint16_t msix_count;
    uint8_t msix_irq[VIRTIO_MSIX_MAX];
};

struct virtqueue {
    struct virtio_device* dev;
    uint16_t index;
    uint16_t size;
    struct vring_desc* desc;
    volatile struct vring_avail* avail;
    volatile struct vring_used* used;
    uint16_t free_head;
    uint16_t num_free;
    uint16_t last_used;
    uint16_t kicked;
    bool event;
};

struct virtio_buf {
    uint64_t addr;
    uint32_t len;
    bool write;
};

typedef void (*virtio_probe_t)(pci_address addr);

void virtio_pci_foreach(uint16_t device_id, virtio_probe_t probe);
bool virtio_attach(struct virtio_device* dev, pci_address addr);
uint32_t virtio_negotiate(struct virtio_device* dev, uint32_t wanted);
int virtio_msix_prepare(struct virtio_device* dev, uint16_t count);
void virtio_msix_activate(struct virtio_device* dev);
void virtio_msix_disable(struct virtio_device* dev);
bool virtio_msix_config_vector(const struct virtio_device* dev, uint16_t entry);
bool virtio_msix_queue_vector(const struct virtio_device* dev, uint16_t queue, uint16_t entry);

static void virtio_driver_ok(const struct virtio_device* dev)
{
    port_write_u8(dev->io + VIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
}

static void virtio_fail(const struct virtio_device* dev)
{
    port_write_u8(dev->io + VIO_STATUS, VIRTIO_STATUS_FAILED);
}

static uint8_t virtio_isr(const struct virtio_device* dev)
{
    return port_read_u8(dev->io + VIO_ISR);
}

static uint8_t virtio_config_u8(const struct virtio_device* dev, const uint16_t off)
{
    return port_read_u8(dev->io + dev->config + off);
}

static uint16_t virtio_config_u16(const struct virtio_device* dev, const uint16_t off)
{
    return port_read_u16(dev->io + dev->config + off);
}

static uint32_t virtio_config_u32(const struct virtio_device* dev, const uint16_t off)
{
    return port_read_u32(dev->io + dev->config + off);
}

bool virtqueue_setup(struct virtio_device* dev, struct virtqueue* vq, uint16_t index);
int virtqueue_add(struct virtqueue* vq, const struct virtio_buf* bufs, uint16_t count);
void virtqueue_kick(struct virtqueue* vq);

static bool virtqueue_pending(const struct virtqueue* vq)
{
    return vq->last_used != vq->used->idx;
}

bool virtqueue_get(struct virtqueue* vq, uint16_t* head, uint32_t* len);

static void virtqueue_intr_disable(const struct virtqueue* vq)
{
    if (vq->event)
        vq->avail->ring[vq->size] = (uint16_t)(vq->last_used + 0x8000);
    else
        __atomic_or_fetch(&vq->avail->flags, VRING_AVAIL_F_NO_INTERRUPT, __ATOMIC_SEQ_CST);
}

bool virtqueue_intr_enable(const struct virtqueue* vq);
bool virtqueue_intr_enable_delayed(const struct virtqueue* vq);


#endif //OS_VIRTIO_H
