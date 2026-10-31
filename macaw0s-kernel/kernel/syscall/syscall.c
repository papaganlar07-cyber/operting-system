/* MacawOS kernel — syscall dispatch + demo userland image (ring-3 handoff) */
#include "mc/syscall.h"
#include "mc/loader.h"
#include "mc/mm.h"
#include "mc/cpu.h"
#include "mc/io.h"
#include "mc/vga.h"

static volatile u64 g_syscall_count;

u64 syscall_dispatch(u64 nr, u64 a1, u64 a2, u64 a3)
{
    g_syscall_count++;
    switch (nr) {
    case SYS_WRITE: {
        const char *s = (const char *)a2;             /* kernel VA in stage-1 */
        u64 n = a3 ? a3 : strlen(s);
        for (u64 i = 0; i < n; i++) kputc(s[i]);
        return (i64)n;
    }
    case SYS_GETPID:   return 1;
    case SYS_EXIT:     kprintf("[syscall] user process exited code=%lu\n",
                               (unsigned long)a1);
                       for (;;) hlt();
    case SYS_INFO:     return pmm_used_frames();
    default:           return (u64)-1;                /* ENOSYS             */
    }
}

u64 syscall_total(void) { return g_syscall_count; }

/* ---- tiny hand-assembled x86-64 user program ----------------------------
 * _start @ 0x401000 (one R+X PT_LOAD covering file 0..0x1030):
 *   mov rax,0 ; mov rdi,1 ; lea rsi,[rip+msg] ; mov rdx,15 ; syscall
 *   mov rax,1 ; mov rdi,0 ; syscall ; jmp .
 * msg "Hello from ring3\n" at 0x401020.
 * ------------------------------------------------------------------------ */
static const u8 elf_head[] = {
    /* ELF64 header (offsets: e_type@16, e_machine@18, e_entry@24, e_phoff@32,
       e_phentsize@54, e_phnum@56) */
    0x7f,'E','L','F',2,1,1,0, 0,0,0,0, 0,0,0,0,
    0x02,0x00, 0x3e,0x00, 0x01,0x00,0x00,0x00,
    0x00,0x10,0x40,0x00,0x00,0x00,0x00,0x00,   /* e_entry = 0x401000 */
    0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,   /* e_phoff = 0x40     */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,   /* e_shoff = 0        */
    0x00,0x00,0x00,0x00,
    0x40,0x00, 0x38,0x00, 0x01,0x00,           /* ehsize,phentsize,phnum */
    0x00,0x00, 0x00,0x00, 0x00,0x00,           /* shentsize,shnum,shstrndx */
    /* program header @0x40 */
    0x01,0x00,0x00,0x00, 0x05,0x00,0x00,0x00,   /* PT_LOAD, R+X         */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,    /* p_offset = 0         */
    0x00,0x00,0x40,0x00,0x00,0x00,0x00,0x00,    /* p_vaddr  = 0x400000  */
    0x00,0x00,0x40,0x00,0x00,0x00,0x00,0x00,    /* p_paddr              */
    0x30,0x10,0x00,0x00,0x00,0x00,0x00,0x00,    /* p_filesz = 0x1030    */
    0x30,0x10,0x00,0x00,0x00,0x00,0x00,0x00,    /* p_memsz  = 0x1030    */
    0x00,0x00,0x20,0x00,0x00,0x00,0x00,0x00,    /* p_align  = 2MiB      */
};

#define USER_CODE_OFF 0x1000                      /* == VA 0x401000 (flat map) */

int exec_run_demo(void)
{
    size_t imglen = 0x1030;                       /* one page + headers   */
    u8 *img = kzalloc(imglen);
    if (!img) return -1;
    memcpy(img, elf_head, sizeof(elf_head));

    /* machine code at file offset 0x1000 → VA 0x401000 */
    u8 *c = img + USER_CODE_OFF;
    c[0]=0x48; c[1]=0xB8; *(u64*)(c+2)=SYS_WRITE;    /* mov rax,0          */
    c[10]=0xBF; *(u32*)(c+11)=1;                     /* mov rdi,1 (fd)     */
    c[15]=0x48; c[16]=0x8D; c[17]=0x35;              /* lea rsi,[rip+..]   */
    *(u32*)(c+18)=0x20-22;                           /* msg @ 0x401020     */
    c[22]=0x48; c[23]=0xC7; c[24]=0xC2; *(u32*)(c+25)=15; /* mov rdx,15   */
    c[29]=0x0F; c[30]=0x05;                          /* syscall            */
    memcpy(img + 0x1020, "Hello from ring3\n", 16);

    addr_space_t *as = vmm_clone_kernel_base();
    exec_image_t im;
    int rc = exec_load(as, img, imglen, &im);
    if (rc != 0) {
        kprintf("[exec] load failed: %s\n", im.err);
        kfree(img);
        return -2;
    }
    exec_dump(&im);
    kfree(img);                                    /* pages now own copies */

    /* user stack: 16 pages near top of the low window */
    u64 ust_top = 0x700000000ULL;
    for (u64 v = ust_top - 16 * PAGE_SIZE; v < ust_top; v += PAGE_SIZE) {
        u64 phys = pmm_alloc_frame();
        memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        vmm_map_page(as, v, phys, PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX);
    }
    tss_set_rsp0(KERNEL_VBASE + 0x100000ULL);        /* kernel scratch rsp0 */

    kprintf("[exec] handing off to ring-3 entry %#lx (CR3 switched)\n",
            (unsigned long)im.entry);
    vmm_switch(as);
    sfence();

    /* Enter ring 3 via iretq: push SS,RSP,RFLAGS,CS,RIP with RPL3 bits. */
    u64 user_cs = 0x18 | 3, user_ss = 0x20 | 3;
    u64 user_rsp = ust_top - 0x100;
    u64 user_flags = 0x202;                          /* IF + reserved      */
    __asm__ volatile(
        "push %0\n push %1\n push %2\n push %3\n push %4\n iretq"
        :: "r"(user_ss), "r"(user_rsp), "r"(user_flags),
           "r"(user_cs), "r"(im.entry) : "memory");
    /* not reached unless user returns badly */
    kpanic("ring-3 returned to kernel unexpectedly");
    return 0;
}
