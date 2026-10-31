/* MacawOS kernel — kmalloc: first-fit free-list heap with coalescing.
 * GC-friendly design notes (stage-2): every block header carries a magic +
 * size, so a conservative mark-scan can walk live ranges; regions are grown
 * page-by-page from the PMM via vmm_map_page into the kernel VA window.
 * NOTE: this allocator serves ONLY the kernel heap band (below KERNEL_VBASE);
 * userland pages live in per-process address spaces created by the loaders. */
#include "mc/mm.h"
#include "mc/io.h"
#include "mc/vga.h"

#define HEAP_MAGIC  0x4D414336u            /* "MAC6"                       */
#define MIN_CHUNK   32ULL
#define REGION_STEP 0x40000ULL             /* grow in 256 KiB steps         */

struct blk {
    u32 magic;
    u32 flags;                 /* bit0 = allocated                        */
    u64 size;                  /* payload bytes                           */
    struct blk *next;          /* physical order within region list       */
};

static struct blk *g_head;              /* sorted by address              */
static u64 g_used, g_free_bytes, g_region_next_virt, g_region_end;
static bool g_heap_ready;

/* append a free block describing [base,size) to the address-ordered list */
static void region_add(u64 base, u64 size)
{
    struct blk *nb = (struct blk *)base;
    nb->magic = HEAP_MAGIC; nb->flags = 0;
    nb->size = size - sizeof(struct blk);
    nb->next = NULL;
    if (!g_head || nb < g_head) { nb->next = g_head; g_head = nb; return; }
    struct blk *p = g_head;
    while (p->next && p->next < nb) p = p->next;
    nb->next = p->next; p->next = nb;
    g_free_bytes += nb->size;
}

void heap_init(void)
{
    /* carve the initial arena out of fresh frames, mapped at a dedicated
     * kernel VA band just under the higher-half base */
    g_region_next_virt = ALIGN_UP(KERNEL_VBASE - 0x10000000ULL, PAGE_SIZE);
    u64 phys = pmm_alloc_frame();                        /* 4 KiB bootstrap  */
    vmm_map_page(vmm_kernel_space(), g_region_next_virt, phys,
                 PTE_WRITE | PTE_NX);
    region_add(g_region_next_virt, PAGE_SIZE);
    g_region_next_virt += PAGE_SIZE;
    g_region_end = g_region_next_virt;
    g_used = 0;
    g_heap_ready = true;
}

static void heap_grow(u64 need)
{
    u64 want = ALIGN_UP(need + sizeof(struct blk) + REGION_STEP, PAGE_SIZE);
    if (want < REGION_STEP) want = REGION_STEP;
    for (u64 off = 0; off < want; off += PAGE_SIZE) {
        u64 phys = pmm_alloc_frame();
        vmm_map_page(vmm_kernel_space(), g_region_next_virt + off, phys,
                     PTE_WRITE | PTE_NX);
    }
    region_add(g_region_next_virt, want);
    g_region_next_virt += want;
    g_region_end = g_region_next_virt;
}

void *kmalloc(size_t n)
{
    if (!g_heap_ready) return NULL;
    if (n == 0) return NULL;
    n = ALIGN_UP(n, 16);
retry:;
    struct blk *cur = g_head;
    while (cur) {
        if (!(cur->flags & 1u) && cur->size >= n) {
            /* split if leftover is worth it */
            if (cur->size >= n + MIN_CHUNK + sizeof(struct blk)) {
                struct blk *ns = (struct blk *)((u8 *)cur + sizeof(struct blk) + n);
                ns->magic = HEAP_MAGIC; ns->flags = 0;
                ns->size = cur->size - n - sizeof(struct blk);
                ns->next = cur->next;
                cur->next = ns;
                cur->size = n;
                g_free_bytes += ns->size;
            }
            cur->flags |= 1u;
            g_used += cur->size;
            if (g_free_bytes >= cur->size) g_free_bytes -= cur->size;
            return (u8 *)cur + sizeof(struct blk);
        }
        cur = cur->next;
    }
    if (g_region_end == 0) kpanic("kmalloc before heap_init");
    heap_grow(n);
    goto retry;
}

void *kzalloc(size_t n)
{
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p)
{
    if (!p) return;
    struct blk *b = (struct blk *)((u8 *)p - sizeof(struct blk));
    if (b->magic != HEAP_MAGIC) kpanic("kfree: bad pointer %p", p);
    if (!(b->flags & 1u)) kpanic("kfree: double free %p", p);
    b->flags &= ~1u;
    g_used -= b->size;
    g_free_bytes += b->size;
    /* coalesce with following free neighbours */
    while (b->next && !(b->next->flags & 1u)) {
        b->size += sizeof(struct blk) + b->next->size;
        b->next = b->next->next;
    }
}

void heap_stats(u64 *used, u64 *free_bytes)
{
    if (used) *used = g_used;
    if (free_bytes) *free_bytes = g_free_bytes;
}
