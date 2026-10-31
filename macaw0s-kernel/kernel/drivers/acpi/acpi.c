/* MacawOS kernel — ACPI: EBDA/low-memory RSDP scan, XSDT/RSDT walk, checksum */
#include "mc/acpi.h"
#include "mc/mm.h"
#include "mc/io.h"
#include "mc/vga.h"

static acpi_info_t g_acpi;

u8 acpi_checksum(const volatile void *tbl, u32 len)
{
    const volatile u8 *p = (const volatile u8 *)tbl;
    u8 sum = 0;
    for (u32 i = 0; i < len; i++) sum = (u8)(sum + p[i]);
    return sum;
}

static const acpi_rsdp_t *scan_range(u64 start, u64 end)
{
    const u8 *p = (const u8 *)PHYS_TO_VIRT(start);
    for (u64 a = start; a + sizeof(acpi_rsdp_t) <= end; a += 16, p += 16) {
        if (memcmp(p, "RSD PTR ", 8) != 0) continue;
        const acpi_rsdp_t *r = (const acpi_rsdp_t *)p;
        if (acpi_checksum(r, 20) == 0) return r;              /* ACPI 1.0 ok */
        if (r->revision >= 2 && acpi_checksum(r, r->length) == 0) return r;
    }
    return NULL;
}

void acpi_init(void)
{
    memset(&g_acpi, 0, sizeof(g_acpi));

    /* 1) EBDA (first KiB of conventional memory holds pointer) */
    const u8 *ebda = (const u8 *)PHYS_TO_VIRT(0x40E);
    u16 ebda_seg = (u16)(ebda[0] | (ebda[1] << 8));
    const acpi_rsdp_t *rsdp = NULL;
    if (ebda_seg)
        rsdp = scan_range((u64)ebda_seg * 16, (u64)ebda_seg * 16 + 1024);
    /* 2) BIOS ROM C8000–EFFFF */
    if (!rsdp) rsdp = scan_range(0xC8000, 0xF0000);
    /* 3) legacy guarded fallback: 0xE0000–0xFFFFF covered above partially */
    if (!rsdp) rsdp = scan_range(0xE0000, 0x100000);

    if (!rsdp) { kprintf("[acpi] RSDP not found\n"); return; }
    g_acpi.rsdp = rsdp;
    g_acpi.found = true;
    kprintf("[acpi] RSDP @ %#lx rev=%u oem=%.6s\n",
            (unsigned long)VIRT_TO_PHYS((uintptr_t)rsdp), rsdp->revision,
            (const char *)rsdp->oem_id);

    const acpi_hdr_t *root;
    bool xsdt = false;
    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        root = (const acpi_hdr_t *)PHYS_TO_VIRT(rsdp->xsdt_address);
        xsdt = true;
    } else if (rsdp->rsdt_address) {
        root = (const acpi_hdr_t *)PHYS_TO_VIRT(rsdp->rsdt_address);
    } else { kprintf("[acpi] no root table ptr\n"); return; }

    if (memcmp(root->signature, xsdt ? "XSDT" : "RSDT", 4) != 0 ||
        acpi_checksum(root, root->length) != 0) {
        kprintf("[acpi] root table corrupt\n"); return;
    }

    u32 entries = (root->length - sizeof(acpi_hdr_t)) / (xsdt ? 8 : 4);
    if (entries > 64) entries = 64;
    const u8 *base = (const u8 *)root + sizeof(acpi_hdr_t);
    for (u32 i = 0; i < entries; i++) {
        u64 phys = xsdt ? *(const u64 *)(base + i * 8)
                        : (u64)*(const u32 *)(base + i * 4);
        if (!phys || phys >= 0x40000000ULL) continue;   /* outside window */
        g_acpi.table_phys[g_acpi.table_count++] = phys;
        const acpi_hdr_t *h = (const acpi_hdr_t *)PHYS_TO_VIRT(phys);
        kprintf("[acpi]   %.4s len=%u\n", (const char *)h->signature, h->length);
    }
}

const acpi_info_t *acpi_get(void) { return &g_acpi; }

const void *acpi_find_table(const char sig[4], u32 *out_len, u64 *out_phys)
{
    for (u32 i = 0; i < g_acpi.table_count; i++) {
        const acpi_hdr_t *h = (const acpi_hdr_t *)PHYS_TO_VIRT(g_acpi.table_phys[i]);
        if (memcmp(h->signature, sig, 4) == 0) {
            if (out_len) *out_len = h->length;
            if (out_phys) *out_phys = g_acpi.table_phys[i];
            return h;
        }
    }
    return NULL;
}
