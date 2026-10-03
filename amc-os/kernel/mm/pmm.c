/* ============================================================
 * AMC OS — Fiziksel Sayfa Yöneticisi (PMM) + SLAB Bellek Ayırıcı
 * kernel/mm/pmm.c
 *   - Buddy allocator (2^0..2^10 sayfa blokları)
 *   - Boot memmap (E820/UEFI memory map'ten gelen harita)
 *   - SLAB: kmem_cache (kmalloc/kfree temeli) — yazılımcı araçları
 *     için hızlı küçük nesne ayırma (task_struct, inode, dentry...)
 * ============================================================ */

#include "pmm.h"
#include "../kernel.h"

#define MAX_MEMREGIONS 64
#define BUDDY_MAX_ORDER 10          /* en büyük blok 4MB */

struct free_area {
    struct list_head free[BUDDY_MAX_ORDER + 1];
    unsigned long    count[BUDDY_MAX_ORDER + 1];
};

static struct {
    uint64_t       total_pages;
    uint64_t       used_pages;
    struct page   *page_array;        /* her fiziksel sayfa için metadata */
    struct free_area areas[MAX_NUMA_NODES];
    spinlock_t     lock;
} pm;

/* ================== Başlatma (boot_info.memmap'dan) ================== */

void pmm_init(struct memory_region *mmap, int entries) {
    uint64_t max_frame = 0;
    for (int i = 0; i < entries; i++) {
        if (mmap[i].type == MEM_USABLE)
            max_frame = MAX(max_frame,
                            (mmap[i].base + mmap[i].size) / PAGE_SIZE);
    }
    pm.total_pages = max_frame;
    pm.page_array  = (struct page *)PAGE_ALIGN(end_of_kernel);

    /* Kullanılabilir bölgeleri buddy'ye ekle */
    for (int i = 0; i < entries; i++) {
        if (mmap[i].type != MEM_USABLE) continue;
        uint64_t start = ALIGN_UP(mmap[i].base, PAGE_SIZE) / PAGE_SIZE;
        uint64_t end   = ALIGN_DOWN(mmap[i].base + mmap[i].size, PAGE_SIZE) / PAGE_SIZE;
        for (uint64_t f = start; f < end; f++)
            buddy_free_page(f);
    }
    kprintf("[pmm] %llu sayfa (%llu MB) serbest\n",
            pm.total_pages - pm.used_pages,
            (pm.total_pages - pm.used_pages) * 4 / 1024);
}

/* ================== Buddy işlemleri ================== */

uint64_t pmm_alloc_pages(int order) {
    scoped_spinlock(&pm.lock);
    for (int o = order; o <= BUDDY_MAX_ORDER; o++) {
        if (!list_empty(&pm.areas[0].free[o])) {
            struct page *p = list_first_entry(&pm.areas[0].free[o],
                                              struct page, lru);
            list_del(&p->lru);
            pm.areas[0].count[o]--;
            /* Kalan parçayı alt seviyelere böl */
            while (o > order) {
                o--;
                struct page *buddy = p + (1 << o);
                list_add(&buddy->lru, &pm.areas[0].free[o]);
                pm.areas[0].count[o]++;
            }
            pm.used_pages += (1 << order);
            return page_to_pfn(p);
        }
    }
    return PFN_ERROR;              /* bellek tükendi → OOM uyarısı */
}

void pmm_free_pages(uint64_t pfn, int order) {
    scoped_spinlock(&pm.lock);
    struct page *p = pfn_to_page(pfn);
    /* Komşu boşsa yukarı birleştir (coalescing) */
    while (order < BUDDY_MAX_ORDER) {
        uint64_t buddy_pfn = pfn ^ (1ULL << order);
        struct page *b = pfn_to_page(buddy_pfn);
        if (page_is_free(b)) {
            list_del(&b->lru);
            pm.areas[0].count[order]--;
            pfn = MIN(pfn, buddy_pfn);
            p = pfn_to_page(pfn);
            order++;
        } else break;
    }
    list_add(&p->lru, &pm.areas[0].free[order]);
    pm.areas[0].count[order]++;
    pm.used_pages -= (1 << order);
}

/* ================== SLAB katmanı (kmalloc temeli) ================== */

struct kmem_cache {
    const char *name;
    size_t objsize, objper_slab;
    struct list_full slabs_partial, slabs_full, slabs_free;
    spinlock_t lock;
};

static struct kmem_cache *slab_caches[MAX_CACHES];

struct kmem_cache *kmem_cache_create(const char *name, size_t size) {
    struct kmem_cache *c = kmalloc_raw(sizeof(*c));
    c->name = name; c->objsize = ALIGN(size, 16);
    INIT_LIST(&c->slabs_partial), INIT_LIST(&c->slabs_full),
    INIT_LIST(&c->slabs_free);
    slab_caches[next_cache_id++] = c;
    return c;
}

void *kmem_cache_alloc(struct kmem_cache *c, gfp_t flags) {
    scoped_spinlock(&c->lock);
    struct slab *s = list_first_entry_or_null(&c->slabs_free, ...);
    if (!s) s = slab_grow(c, flags);      /* yeni fiziksel sayfa(lar) al */
    void *obj = freelist_pop(s);
    if (list_empty(&s->freelist))
        list_move(&s->lru, &c->slabs_full);
    else
        list_move_tail(&s->lru, &c->slabs_partial);
    if (flags & GFP_ZERO) memset(obj, 0, c->objsize);
    return obj;
}

/* Genel amaçlı ayırıcı: boyutu 2^n slab kovana yuvarlar */
void *kmalloc(size_t size, gfp_t flags) {
    if (size <= 96)   return kmem_cache_alloc(caches[size/16], flags);
    if (size <= 4096) return kmem_cache_alloc(caches_large, flags);
    /* Büyük istekler doğrudan buddy */
    int order = get_order(size);
    uint64_t pfn = pmm_alloc_pages(order);
    return pfn ? page_to_virt(pfn) : NULL;
}

void kfree(void *ptr) {
    struct page *p = virt_to_page(ptr);
    if (PageSlab(p)) slab_free_obj(p, ptr);
    else             pmm_free_pages(page_to_pfn(p), compound_order(p));
}

/* ================== İstatistik (görev yöneticisi için) ================== */

void pmm_get_stats(struct mem_stats *out) {
    out->total = pm.total_pages;
    out->used  = pm.used_pages;
    out->free  = pm.total_pages - pm.used_pages;
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++)
        out->buddy_counts[o] = pm.areas[0].count[o];
}
