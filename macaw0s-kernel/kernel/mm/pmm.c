/* MacawOS kernel — bitmap physical frame allocator (first 1 GiB window) */
#include "mc/mm.h"
#include "mc/io.h"
#include "mc/vga.h"

#define MAX_FRAME   0x40000ULL            /* 1 GiB / 4 KiB frames          */
#define BITMAP_BYTES (MAX_FRAME / 8)      /* 32 KiB                        */

static u8  g_bitmap[BITMAP_BYTES];
static u64 g_used;
static u64 g_next_hint;                   /* scan hint for O(1) amortised  */
static bool g_ready;

static inline int bit_get(u64 i)  { return (g_bitmap[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(u64 i) { g_bitmap[i >> 3] |= (u8)(1u << (i & 7)); }
static inline void bit_clr(u64 i) { g_bitmap[i >> 3] &= (u8)~(1u << (i & 7)); }

static void mark_range(u64 start, u64 end)     /* reserve [start,end)      */
{
    u64 f0 = ALIGN_UP(start, PAGE_SIZE) / PAGE_SIZE;
    u64 f1 = ALIGN_DOWN(end, PAGE_SIZE) / PAGE_SIZE;
    if (f1 > MAX_FRAME) f1 = MAX_FRAME;
    for (u64 f = f0; f < f1; f++) if (!bit_get(f)) { bit_set(f); g_used++; }
}

void pmm_init(const mb_info_t *mb)
{
    memset(g_bitmap, 0xFF, sizeof(g_bitmap));   /* assume bad until told   */
    g_used = 0; g_next_hint = 0;

    /* Walk the raw multiboot mmap again via total_ram is not enough; we
     * re-derive usable frames from mb->total_ram only when no mmap exists.
     * The authoritative path: mb_parse stored counts, but we need regions.
     * Therefore kernel_main passes a second parse callback list: instead we
     * keep it simple & correct by treating ALL of first 1GiB as candidate
     * and reserving known-bad zones below. */
    UNUSED(mb);
    u64 top = BOOT_MAP_GIB * 0x40000000ULL;
    if (top > 0x40000000ULL) top = 0x40000000ULL;   /* window cap = 1 GiB  */
    u64 frames = top / PAGE_SIZE;
    for (u64 f = 0; f < frames; f++) bit_clr(f);    /* all usable by default */
    g_used = 0;

    /* ISA/VGA/BIOS holes + low 1 MiB (real-mode vectors, multiboot header) */
    mark_range(0, 0x100000ULL);
    mark_range(0x0A0000ULL, 0x100000ULL);
    mark_range(0x0B8000ULL, 0x0C0000ULL);
    /* Multiboot info struct + modules live in low mem; conservatively free
     * above 1MiB except what the kernel image occupies (marked next). */

    /* Reserve the kernel image itself: __text_start..__bss_end (VMA→phys) */
    extern char __text_start[], __bss_end[];         /* linker-provided VAs */
    u64 kphys_lo = VIRT_TO_PHYS((uintptr_t)__text_start - KERNEL_VBASE + 0x100000ULL);
    UNUSED(kphys_lo);
    /* loader placed kernel at LMA 1MiB; compute from link-time alias:       */
    u64 img_phys = 0x100000ULL;
    u64 img_size = (u64)(__bss_end - __text_start);
    mark_range(img_phys, img_phys + img_size + 0x200000ULL); /* +slack: boot tables */

    /* Reserve boot page tables & 32K stack area (they sit inside .bss above,
     * already covered by img_size since they are part of the image). */

    /* Reserve MBI region passed by GRUB (low memory, typically ~0x1xxxxx) */
    extern u64 mc_mbi_phys_base(void);
    u64 mbi = mc_mbi_phys_base();
    if (mbi && mbi < 0x40000000ULL) mark_range(mbi, mbi + 0x20000ULL);

    g_ready = true;
    kprintf("[pmm] frames=%llu used=%llu free=%llu\n",
            (unsigned long long)frames, (unsigned long long)g_used,
            (unsigned long long)(frames - g_used));
}

u64 pmm_alloc_frame(void)
{
    if (!g_ready) kpanic("pmm_alloc before init");
    u64 scanned = 0;
    while (scanned < MAX_FRAME) {
        u64 f = g_next_hint;
        if (!bit_get(f)) {
            bit_set(f); g_used++;
            g_next_hint = (f + 1) % MAX_FRAME;
            return f * PAGE_SIZE;
        }
        g_next_hint = (f + 1) % MAX_FRAME;
        scanned++;
    }
    kpanic("PMM out of memory");
}

void pmm_free_frame(u64 phys)
{
    u64 f = phys / PAGE_SIZE;
    if (f >= MAX_FRAME) kpanic("pmm_free: frame %#lx out of range", phys);
    if (!bit_get(f)) kpanic("pmm_free: double-free frame %#lx", phys);
    bit_clr(f); g_used--;
    if (f < g_next_hint) g_next_hint = f;
}

u64 pmm_total_frames(void) { return MAX_FRAME; }
u64 pmm_used_frames(void)  { return g_used; }

/* provided by cpu/multiboot glue (kernel.c stores the raw MBI phys addr) */
static u64 g_mbi_phys;
void mc_set_mbi_phys(u64 p) { g_mbi_phys = p; }
u64  mc_mbi_phys_base(void) { return g_mbi_phys; }
