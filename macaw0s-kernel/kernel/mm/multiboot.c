/* MacawOS kernel — Multiboot2 information-structure parser */
#include "mc/mm.h"
#include "mc/io.h"
#include "mc/vga.h"

#define MB2_MAGIC 0xE85250D6u

struct mb2_tag_hdr { u32 type; u32 size; };

void mb_parse(u64 mbi_phys, mb_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (mbi_phys == 0 || mbi_phys > 0xFFFFFFFFULL)
        kpanic("multiboot: invalid MBI phys addr %p", mbi_phys);

    const volatile u8 *base = (const volatile u8 *)PHYS_TO_VIRT(mbi_phys);
    u32 total = *(const volatile u32 *)(base + 0);
    if (*(const volatile u32 *)(base + 4) != MB2_MAGIC)
        kpanic("multiboot: bad MBI checksum/magic");

    bool got_mmap = false;
    u32 off = 8;
    while (off + 8 <= total) {
        const volatile struct mb2_tag_hdr *h =
            (const volatile struct mb2_tag_hdr *)(base + off);
        u32 tsz = h->size;
        if (tsz < 8 || off + tsz > total) break;
        switch (h->type) {
        case 1: { /* basic memory info — fallback if no mmap tag */
            u64 upper = *(const volatile u32 *)(base + off + 12);
            if (!got_mmap) out->total_ram = upper * 1024ULL;
            break; }
        case 4: { /* memory map: entry = size(4) base(8) len(8) type(4) */
            got_mmap = true;
            out->total_ram = 0;
            u32 eo = off + 8;
            while (eo + 4 <= off + tsz) {
                u32 esz = *(const volatile u32 *)(base + eo);
                if (esz < 24) break;
                u64 b = *(const volatile u64 *)(base + eo + 8);
                u64 l = *(const volatile u64 *)(base + eo + 16);
                u32 t = *(const volatile u32 *)(base + eo + 24);
                UNUSED(b);
                if (t == 1) {                       /* usable region */
                    out->total_ram += l;
                    out->mmap_entries++;
                }
                eo += esz;
            }
            break; }
        case 17: { /* command line */
            const char *cl = (const char *)(base + off + 8);
            strncpy(out->cmdline, cl, sizeof(out->cmdline) - 1);
            break; }
        case 2: { /* bootloader name */
            const char *bl = (const char *)(base + off + 8);
            strncpy(out->loader, bl, sizeof(out->loader) - 1);
            break; }
        default: break;
        }
        off += (tsz + 7u) & ~7u;
    }

    /* first usable >= 1MiB marker (kernel image end is handled by PMM itself) */
    out->high_mem_start = 0x100000ULL;
}
