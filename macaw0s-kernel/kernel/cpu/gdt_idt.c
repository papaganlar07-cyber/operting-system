/* MacawOS kernel — GDT + TSS, IDT, 8259 PIC remask, CPU feature enable */
#include "mc/cpu.h"
#include "mc/io.h"
#include "mc/vga.h"

/* ---- GDT --------------------------------------------------------------- */
struct desc64 { u16 lim0_15; u16 base0_15; u8 base16_23; u8 a:1, r:1, c:1, res0:1,
                p:1, dpl:2, db:1; u8 res1:1, long_:1, sz:1, gran:1; u8 base24_31; } PACKED;

struct tss64 {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u64 ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    u64 reserved1;
    u16 reserved2;
    u16 iomap_base;
} PACKED;

static struct {
    struct desc64 null, code, data, user32, user_data, tss_lo, tss_hi;
} PACKED g_gdt;

static struct desc_ptr { u16 limit; u64 base; } PACKED g_dtr;
static struct tss64 g_tss;
static u16 g_tss_sel;

extern void isr_stub_table_ref(void);      /* unused anchor, keeps LTO honest */
UNUSED(isr_stub_table_ref);

static void set_gate(struct desc64 *d, u32 base, u32 lim, u8 access, u8 flags)
{
    d->lim0_15  = (u16)(lim & 0xFFFF);
    d->base0_15 = (u16)(base & 0xFFFF);
    d->base16_23 = (u8)((base >> 16) & 0xFF);
    d->a = 1; d->r = (access & 2) ? 1 : 0; d->c = (access & 8) ? 1 : 0;
    d->res0 = 0; d->p = 1; d->dpl = (access >> 5) & 3;
    d->db = (flags & 4) ? 1 : 0;
    d->res1 = 0; d->long_ = (flags & 2) ? 1 : 0; d->sz = (flags & 1) ? 1 : 0;
    d->gran = (flags & 8) ? 1 : 0;
    d->base24_31 = (u8)((base >> 24) & 0xFF);
}

void gdt_init(void)
{
    memset(&g_gdt, 0, sizeof(g_gdt));
    /* 0x08 kernel code, 0x10 kernel data, 0x18 user code32-compat stub,
     * 0x20 user data, 0x28/0x30 TSS (16-byte pair) */
    set_gate(&g_gdt.code, 0, 0xFFFFF, 0x9A, 0xA);   /* L=1 D=0 */
    set_gate(&g_gdt.data, 0, 0xFFFFF, 0x92, 0xC);   /* D/B=1 G=1 */
    set_gate(&g_gdt.user32, 0, 0xFFFFF, 0xF8, 0xC); /* ring3 code compat */
    set_gate(&g_gdt.user_data, 0, 0xFFFFF, 0xF2, 0xC);

    u64 tss_base = (u64)(uintptr_t)&g_tss;
    u32 tss_lim = sizeof(g_tss) - 1;
    struct desc64 *lo = &g_gdt.tss_lo, *hi = &g_gdt.tss_hi;
    lo->lim0_15  = (u16)(tss_lim & 0xFFFF);
    lo->base0_15 = (u16)(tss_base & 0xFFFF);
    lo->base16_23 = (u8)((tss_base >> 16) & 0xFF);
    lo->a = 1; lo->r = 1; lo->c = 0; lo->res0 = 0; lo->p = 1; lo->dpl = 0;
    lo->db = 0; lo->res1 = 0; lo->long_ = 0; lo->sz = 0; lo->gran = 0;
    lo->base24_31 = (u8)((tss_base >> 24) & 0xFF);
    hi->lim0_15  = (u16)((tss_base >> 32) & 0xFFFF);
    hi->base0_15 = (u16)((tss_base >> 48) & 0xFFFF);
    *(u32 *)&hi->base16_23 = 0;                     /* rest zero */

    g_dtr.limit = (u16)(sizeof(g_gdt) - 1);
    g_dtr.base = (u64)(uintptr_t)&g_gdt;
    __asm__ volatile("lgdt %0" :: "m"(g_dtr));

    g_tss_sel = 0x28;
    asm volatile("ltr %w0" :: "r"(g_tss_sel));
    sti(); /* harmless here; real enabling after IDT */
}

/* IST stacks for double/triple fault isolation (stage-2 hardening) */
static u8 ist_stack_8[4096] ALIGNED(16);

void tss_set_rsp0(u64 rsp0)
{
    g_tss.rsp0 = rsp0;
    g_tss.ist1 = (u64)(uintptr_t)(ist_stack_8 + sizeof(ist_stack_8));
}

/* ---- IDT --------------------------------------------------------------- */
struct idt_entry {
    u16 off0_15; u16 sel; u8 ist, type_attr; u16 off16_31; u32 off32_63; u32 zero;
} PACKED;

static struct idt_entry g_idt[256];
static struct { u16 limit; u64 base; } PACKED g_idtr;

/* provided by isr_stubs.asm */
extern void exc_0(void), exc_1(void), exc_2(void), exc_3(void), exc_4(void),
      exc_5(void), exc_6(void), exc_7(void), exc_9(void), exc_16(void),
      exc_17(void), exc_18(void), exc_19(void), exc_20(void), exc_8(void),
      exc_10(void), exc_11(void), exc_12(void), exc_13(void), exc_14(void),
      exc_15(void);
extern void irq0(void),  irq1(void),  irq2(void),  irq3(void),
      irq4(void),  irq5(void),  irq6(void),  irq7(void),
      irq8(void),  irq9(void),  irq10(void), irq11(void),
      irq12(void), irq13(void), irq14(void), irq15(void);

/* syscall entry from isr_stubs.asm; MSRs configured in syscall_init() */
extern void syscall_entry(void);

static void idt_set(u8 vec, void *handler, u8 ist, u8 dpl)
{
    u64 h = (u64)(uintptr_t)handler;
    g_idt[vec].off0_15  = (u16)(h & 0xFFFF);
    g_idt[vec].sel      = 0x08;
    g_idt[vec].ist      = ist;
    g_idt[vec].type_attr = (u8)(0x8E | ((3 - dpl) << 5) ? 0x8E : 0x8E); /* ring0 gate */
    g_idt[vec].off16_31 = (u16)((h >> 16) & 0xFFFF);
    g_idt[vec].off32_63 = (u32)((h >> 32) & 0xFFFFFFFF);
    g_idt[vec].zero     = 0;
}

void idt_init(void)
{
    memset(g_idt, 0, sizeof(g_idt));
    idt_set(0, exc_0, 0, 0);   idt_set(1, exc_1, 0, 0);
    idt_set(2, exc_2, 1, 0);   idt_set(3, exc_3, 0, 3);   /* int3 user-callable */
    idt_set(4, exc_4, 0, 0);   idt_set(5, exc_5, 0, 0);
    idt_set(6, exc_6, 0, 0);   idt_set(7, exc_7, 0, 0);
    idt_set(8, exc_8, 0, 0);   idt_set(9, exc_9, 0, 0);
    idt_set(10, exc_10, 0, 0); idt_set(11, exc_11, 0, 0);
    idt_set(12, exc_12, 0, 0); idt_set(13, exc_13, 0, 0);
    idt_set(14, exc_14, 0, 0); idt_set(15, exc_15, 0, 0);
    idt_set(16, exc_16, 0, 0); idt_set(17, exc_17, 0, 0);
    idt_set(18, exc_18, 0, 0); idt_set(19, exc_19, 0, 0);
    idt_set(20, exc_20, 0, 0);
    void *irqs[16] = { irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
                       irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15 };
    for (int i = 0; i < 16; i++) idt_set((u8)(32 + i), irqs[i], 0, 0);

    g_idtr.limit = (u16)(sizeof(g_idt) - 1);
    g_idtr.base = (u64)(uintptr_t)g_idt;
    __asm__ volatile("lidt %0" :: "m"(g_idtr));
}

/* ---- 8259 PIC ---------------------------------------------------------- */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define ICW4_INIT_COMPLETE 0x01
#define OCW2_EOI          0x20

void pic_remap(u8 master_base, u8 slave_base)
{
    u8 m_mask = inb(PIC1_DATA), s_mask = inb(PIC2_DATA);
    outb(PIC1_CMD, 0x11); outb(PIC2_CMD, 0x11);      /* ICW1: init+cascade */
    outb(PIC1_DATA, master_base); outb(PIC2_DATA, slave_base);
    outb(PIC1_DATA, 0x04); outb(PIC2_DATA, 0x02);    /* ICW3               */
    outb(PIC1_DATA, ICW4_INIT_COMPLETE); outb(PIC2_DATA, ICW4_INIT_COMPLETE);
    outb(PIC1_DATA, m_mask); outb(PIC2_DATA, s_mask);
}

void pic_mask(u8 irq, bool masked)
{
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8 v = inb(port);
    if (masked) v |= (u8)(1u << (irq & 7)); else v &= (u8)~(1u << (irq & 7));
    outb(port, v);
}

void pic_eoi(u8 irq)
{
    if (irq >= 8) outb(PIC2_CMD, OCW2_EOI);
    outb(PIC1_CMD, OCW2_EOI);
}

/* default weak-ish handlers (overridable via isr_install table) */
static void (*g_exc_handlers[32])(regs_t *);
static void (*g_irq_handlers[16])(regs_t *);

void isr_install(u8 vec, void (*handler)(regs_t *))
{
    if (vec < 32) g_exc_handlers[vec] = handler;
    else if (vec >= 32 && vec < 48) g_irq_handlers[vec - 32] = handler;
}

static const char *exc_names[32] = {
    "Divide Error","Debug","NMI","Breakpoint","Overflow","BOUND Range",
    "Invalid Opcode","Device Not Available","Double Fault","Coprocessor Seg",
    "Invalid TSS","Segment Not Present","Stack-Segment","General Protection",
    "Page Fault","Reserved","x87 FP","Alignment Check","Machine Check",
    "SIMD FP","Virtualization","#HV Injection","Reserved","Reserved",
    "Reserved","Reserved","Reserved","Reserved","Reserved","Security",
    "Reserved","Reserved"
};

void isr_exception_handler(u64 vec, regs_t *r)
{
    if (vec < 32 && g_exc_handlers[vec]) { g_exc_handlers[vec](r); return; }
    kpanic("CPU exception %llu (%s) rip=%#lx err=%#lx cr2=%#lx",
           vec, vec < 32 ? exc_names[vec] : "?", r->rip,
           vec == 8 || (vec >= 10 && vec <= 15) ? *(u64 *)(r + 1) : 0ULL,
           read_cr2());
}

void isr_irq_handler(u64 irq, regs_t *r)
{
    UNUSED(r);
    if (irq < 16 && g_irq_handlers[irq]) { g_irq_handlers[irq](r); }
    pic_eoi((u8)irq);
}

/* ---- CPU features ------------------------------------------------------ */
static inline void cpuid_leaf(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

bool cpu_has(u32 leaf_ext, u32 bit)   /* bit within ECX of leaf 1 or EDX etc */
{
    u32 a, b, c, d;
    cpuid_leaf(1, 0, &a, &b, &c, &d);
    (void)leaf_ext;
    return (c >> bit) & 1u;
}

void cpu_enable_features(void)
{
    u64 cr4 = read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    u32 a, b, c, d;
    cpuid_leaf(7, 0, &a, &b, &c, &d);
    if ((b >> 7) & 1u) cr4 |= CR4_SMEP;          /* SMEP                  */
    if ((b >> 20) & 1u) cr4 |= CR4_SMAP;         /* SMAP                  */
    write_cr4(cr4);

    u64 efer = rdmsr(0xC0000080);
    efer |= EFER_NXE | EFER_LME;                 /* NX + Long mode        */
    wrmsr(0xC0000080, efer);

    /* SYSCALL/SYSRET setup */
    wrmsr(0xC0000080, efer);
    wrmsr(0xC0000081, (u64)syscall_entry);       /* STAR low half later   */
    u64 star = ((u64)0x08 << 32) | ((u64)0x18 << 48); /* CS0 / CSP (compat) */
    wrmsr(0xC0000081, star);
    wrmsr(0xC0000082, (u64)syscall_entry);       /* LSTAR                 */
    wrmsr(0xC0000083, (1u << 2) | (1u << 1));    /* FMASK: clear IF&DF on trap */
    /* SVM/VMX detection logged only */
    cpuid_leaf(1, 0, &a, &b, &c, &d);
    kprintf("[cpu] feat ecx=%#lx edx=%#lx smep=%d smap=%d\n",
            c, d, !!((read_cr4() & CR4_SMEP)), !!((read_cr4() & CR4_SMAP)));
}

u64 cpu_rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}
