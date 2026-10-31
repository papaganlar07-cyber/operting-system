/* MacawOS kernel — 4-level page tables (PML4) on top of the PMM.
 * The boot PML4 already contains: [0]=identity low1GiB, [261]=HHDM,
 * [511]=kernel higher-half. We adopt it and extend on demand. */
#include "mc/mm.h"
#include "mc/io.h"
#include "mc/vga.h"

extern char pml4_final[];                 /* boot/loader.asm (.bss)        */

static addr_space_t g_kernel_as;
static bool g_vmm_ready;

static u64 *virt_of_phys(u64 phys) { return (u64 *)PHYS_TO_VIRT(phys); }

/* pml4_final lives in .bss whose LMA = VMA - KERNEL_VBASE (linker.ld AT()),
 * and the low 1 GiB is identity-mapped, so its physical addr == that LMA. */
void vmm_init(addr_space_t *as)
{
    u64 phys = (u64)(uintptr_t)pml4_final - KERNEL_VBASE;
    g_kernel_as.pml4 = virt_of_phys(phys);
    if (as) as->pml4 = g_kernel_as.pml4;
    g_vmm_ready = true;
}

addr_space_t *vmm_kernel_space(void) { return &g_kernel_as; }

static u64 *next_table(u64 *entry, u64 flags)
{
    if (*entry & PTE_PRESENT) return virt_of_phys(*entry & PTE_ADDR_MASK);
    u64 f = pmm_alloc_frame();
    memset(virt_of_phys(f), 0, PAGE_SIZE);
    *entry = f | flags | PTE_PRESENT | PTE_WRITE;
    return virt_of_phys(f);
}

int vmm_map_page(addr_space_t *as, u64 virt, u64 phys, u64 flags)
{
    if (!g_vmm_ready) return -1;
    if (virt >> 48) return -1;                    /* canonical check        */
    u64 *e = as->pml4;
    u64 idx[4] = { (virt >> 39) & 0x1FF, (virt >> 30) & 0x1FF,
                   (virt >> 21) & 0x1FF, (virt >> 12) & 0x1FF };
    for (int i = 0; i < 3; i++) e = next_table(&e[idx[i]], flags & PTE_USER);
    e[idx[3]] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    __builtin_ia32_tlbinvlpg((void *)virt);       /* single-page TLB flush  */
    return 0;
}

int vmm_map_range(addr_space_t *as, u64 virt, u64 phys, u64 size, u64 flags)
{
    if ((virt | phys | size) & (PAGE_SIZE - 1)) return -1;
    for (u64 off = 0; off < size; off += PAGE_SIZE)
        if (vmm_map_page(as, virt + off, phys + off, flags) != 0) return -1;
    return 0;
}

void vmm_unmap(addr_space_t *as, u64 virt)
{
    u64 *e = as->pml4;
    u64 idx[4] = { (virt >> 39) & 0x1FF, (virt >> 30) & 0x1FF,
                   (virt >> 21) & 0x1FF, (virt >> 12) & 0x1FF };
    for (int i = 0; i < 3; i++) {
        if (!(e[idx[i]] & PTE_PRESENT)) return;
        e = virt_of_phys(e[idx[i]] & PTE_ADDR_MASK);
    }
    if (e[idx[3]] & PTE_PRESENT) {
        pmm_free_frame(e[idx[3]] & PTE_ADDR_MASK);
        e[idx[3]] = 0;
        __builtin_ia32_tlbinvlpg((void *)virt);
    }
}

u64 vmm_translate(addr_space_t *as, u64 virt)
{
    u64 *e = as->pml4;
    u64 idx[4] = { (virt >> 39) & 0x1FF, (virt >> 30) & 0x1FF,
                   (virt >> 21) & 0x1FF, (virt >> 12) & 0x1FF };
    for (int i = 0; i < 4; i++) {
        if (!(e[idx[i]] & PTE_PRESENT)) return 0;
        u64 pe = e[idx[i]];
        if (i == 2 && (pe & PTE_HUGE)) return (pe & 0xFFFFFFFE00000ULL) + (virt & 0x1FFFFF);
        if (i == 3) return (pe & PTE_ADDR_MASK) + (virt & 0xFFF);
        e = virt_of_phys(pe & PTE_ADDR_MASK);
    }
    return 0;
}

void vmm_switch(addr_space_t *as)
{
    write_cr3(VIRT_TO_PHYS((uintptr_t)as->pml4));
}

/* New address space sharing only the upper half (kernel+HHDM). Lower half
 * (userland window) starts empty — perfect for the ELF/PE/Mach-O loaders. */
addr_space_t *vmm_clone_kernel_base(void)
{
    addr_space_t *as = kzalloc(sizeof(*as));
    if (!as) kpanic("vmm_clone: OOM");
    u64 pml4_phys = pmm_alloc_frame();
    u64 *pml4 = virt_of_phys(pml4_phys);
    memset(pml4, 0, PAGE_SIZE);
    for (int i = 256; i < 512; i++) pml4[i] = g_kernel_as.pml4[i];  /* high half */
    as->pml4 = pml4;
    return as;
}
