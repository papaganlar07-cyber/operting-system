/* MacawOS kernel — physical & virtual memory manager API */
#ifndef MC_MM_H
#define MC_MM_H
#include "mc/types.h"

/* ---- multiboot ---- */
typedef struct {
    u64  total_ram;        /* usable bytes                            */
    u64  mmap_entries;     /* number of usable regions                */
    u64  high_mem_start;   /* first usable addr >= 1 MiB              */
    char cmdline[256];
    char loader[128];
} mb_info_t;
void mb_parse(u64 mbi_phys, mb_info_t *out);

/* ---- PMM: bitmap frame allocator (manages frames < 1 GiB window) -- */
void  pmm_init(const mb_info_t *mb);
u64   pmm_alloc_frame(void);                 /* returns PHYSICAL addr  */
void  pmm_free_frame(u64 phys);
u64   pmm_total_frames(void);
u64   pmm_used_frames(void);

/* ---- VMM: 4-level page tables on top of PMM ---------------------- */
#define PAGE_SIZE   0x1000ULL
#define HUGE_2M     0x200000ULL

#define PTE_PRESENT   0x1ULL
#define PTE_WRITE     0x2ULL
#define PTE_USER      0x4ULL
#define PTE_PWT       0x8ULL
#define PTE_PCD      0x10ULL
#define PTE_ACCESSED 0x20ULL
#define PTE_DIRTY    0x40ULL
#define PTE_HUGE     0x80ULL
#define PTE_GLOBAL  0x100ULL
#define PTE_NX   0x8000000000000000ULL
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

typedef struct { u64 *pml4; } addr_space_t;

addr_space_t *vmm_kernel_space(void);
void vmm_init(addr_space_t *as);
/* map a single 4K page (allocates intermediate tables as needed) */
int  vmm_map_page(addr_space_t *as, u64 virt, u64 phys, u64 flags);
int  vmm_map_range(addr_space_t *as, u64 virt, u64 phys, u64 size, u64 flags);
void vmm_unmap(addr_space_t *as, u64 virt);
void vmm_switch(addr_space_t *as);
u64  vmm_translate(addr_space_t *as, u64 virt);   /* 0 = not present */
addr_space_t *vmm_clone_kernel_base(void);        /* new AS w/ kernel+HHDM half */

/* ---- kmalloc heap ------------------------------------------------- */
void  heap_init(void);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void  kfree(void *p);
void  heap_stats(u64 *used, u64 *free_bytes);

#endif
