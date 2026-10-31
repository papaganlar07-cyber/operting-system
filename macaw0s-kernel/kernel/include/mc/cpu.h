/* MacawOS kernel — CPU: GDT/IDT/TSS, IRQ routing, scheduling hooks */
#ifndef MC_CPU_H
#define MC_CPU_H
#include "mc/types.h"

typedef struct PACKED {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 rip, cs, rflags;              /* pushed by stub / CPU        */
    u64 rsp, ss;                      /* x86_64 exception frame tail */
} regs_t;

void gdt_init(void);
void idt_init(void);
void pic_remap(u8 master_base, u8 slave_base);
void pic_mask(u8 irq, bool masked);
void pic_eoi(u8 irq);
void isr_install(u8 vec, void (*handler)(regs_t *));
u64  timer_ticks(void);               /* PIT ticks since boot        */
void sleep_ms(u32 ms);                /* PIT-based busy sleep        */
u64  cpu_rdtsc(void);

/* scheduler (ipc/sched.c) */
struct thread;
void sched_init(void);
void schedule(regs_t *ctx);            /* called from timer IRQ      */
u32  spawn_thread(void (*fn)(void *), void *arg, const char *name);
void yield(void);
u32  current_tid(void);
#endif
