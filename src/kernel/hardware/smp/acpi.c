#include "acpi.h"

#include <hardware/memory/mmu.h>
#include <hardware/memory/alloc.h>
#include <os/stdbool.h>

#define MADT_LOCAL_APIC 0
#define MADT_LAPIC_OVERRIDE 5
#define MADT_ENABLED 0x1
#define MADT_ONLINE_CAPABLE 0x2

struct rsdp {
    char signature[8];
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem[6];
    char oem_table[8];
    uint32_t oem_revision;
    uint32_t creator;
    uint32_t creator_revision;
} __attribute__((packed));

struct madt {
    struct sdt_header header;
    uint32_t lapic;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

static bool checksum_ok(const void* p, const size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += ((const uint8_t*)p)[i];
    return sum == 0;
}

static const void* map_phys(const uint64_t phys, const size_t len)
{
    if (phys + len <= 0x40000000ULL)
        return (const void*)phys;
    const uint64_t base = phys & ~0xFFFULL;
    const size_t size = (phys + len - base + 0xFFF) & ~0xFFFULL;
    const uint64_t virt = vmm_alloc_mmio_region(size);
    if (!virt)
        return nullptr;
    for (uint64_t off = 0; off < size; off += 0x1000)
        map_page(virt + off, base + off, MMU_PRESENT);
    return (const void*)(virt + (phys - base));
}

static const struct rsdp* find_rsdp_in(const uint64_t start, const uint64_t end)
{
    for (uint64_t p = start; p + sizeof(struct rsdp) <= end; p += 16) {
        const struct rsdp* r = (const struct rsdp*)p;
        if (!memcmp((void*)r->signature, "RSD PTR ", 8) && checksum_ok(r, 20))
            return r;
    }
    return nullptr;
}

static const struct sdt_header* map_table(const uint64_t phys)
{
    const struct sdt_header* h = map_phys(phys, sizeof(struct sdt_header));
    if (!h)
        return nullptr;
    const struct sdt_header* full = map_phys(phys, h->length);
    return full && checksum_ok(full, full->length) ? full : nullptr;
}

static const struct madt* find_madt()
{
    const uint64_t ebda = (uint64_t)(*(const uint16_t*)0x40E) << 4;
    const struct rsdp* rsdp = ebda ? find_rsdp_in(ebda, ebda + 1024) : nullptr;
    if (!rsdp)
        rsdp = find_rsdp_in(0xE0000, 0x100000);
    if (!rsdp)
        return nullptr;

    const bool xsdt = rsdp->revision >= 2 && rsdp->xsdt;
    const struct sdt_header* root = map_table(xsdt ? rsdp->xsdt : rsdp->rsdt);
    if (!root)
        return nullptr;
    const size_t width = xsdt ? 8 : 4;
    const size_t count = (root->length - sizeof(struct sdt_header)) / width;
    const uint8_t* entries = (const uint8_t*)(root + 1);
    for (size_t i = 0; i < count; i++) {
        uint64_t phys = 0;
        memcpy(&phys, entries + i * width, width);
        const struct sdt_header* table = map_table(phys);
        if (table && !memcmp((void*)table->signature, "APIC", 4))
            return (const struct madt*)table;
    }
    return nullptr;
}

int acpi_find_cpus(uint8_t* apic_ids, const int max, uint64_t* lapic_phys)
{
    const struct madt* madt = find_madt();
    if (!madt)
        return 0;
    *lapic_phys = madt->lapic;

    int count = 0;
    const uint8_t* p = madt->entries;
    const uint8_t* end = (const uint8_t*)madt + madt->header.length;
    while (p + 2 <= end && p[1] >= 2) {
        if (p[0] == MADT_LOCAL_APIC && (p[4] & (MADT_ENABLED | MADT_ONLINE_CAPABLE)) && count < max)
            apic_ids[count++] = p[3];
        else if (p[0] == MADT_LAPIC_OVERRIDE)
            memcpy(lapic_phys, p + 4, sizeof(uint64_t));
        p += p[1];
    }
    return count;
}
