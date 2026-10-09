/* MacawOS kernel — x86_64 port I/O, MSRs, barriers, string ops */
#ifndef MC_IO_H
#define MC_IO_H
#include "mc/types.h"

static inline u8  inb(u16 port)  { u8 v;  __asm__ volatile("inb %w1,%0"   : "=a"(v) : "Nd"(port)); return v; }
static inline u16 inw(u16 port)  { u16 v; __asm__ volatile("inw %w1,%0"   : "=a"(v) : "Nd"(port)); return v; }
static inline u32 infl(u16 port) { u32 v; __asm__ volatile("inl %w1,%0"   : "=a"(v) : "Nd"(port)); return v; }

static inline void outb(u16 port, u8 v)  { __asm__ volatile("outb %0,%w1" : : "a"(v), "Nd"(port)); }
static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0,%w1" : : "a"(v), "Nd"(port)); }
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0,%w1" : : "a"(v), "Nd"(port)); }

static inline u64 rdmsr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 msr, u64 val) {
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"((u32)(val & 0xFFFFFFFFULL)),
                     "d"((u32)(val >> 32)));
}

#define CR0_PE  0x00000001ULL
#define CR0_WP  0x00010000ULL
#define CR0_PG  0x80000000ULL
#define CR4_PAE 0x00000010ULL
#define CR4_OSFXSR 0x00000200ULL
#define CR4_OSXMMEXCPT 0x00000400ULL
#define CR4_SMEP 0x00100000ULL
#define CR4_SMAP 0x00200000ULL
#define EFER_LME 0x00000100ULL
#define EFER_NXE 0x00000800ULL

static inline u64 read_cr0(void) { u64 v; __asm__ volatile("mov %%cr0,%0":"=r"(v)); return v; }
static inline u64 read_cr2(void) { u64 v; __asm__ volatile("mov %%cr2,%0":"=r"(v)); return v; }
static inline u64 read_cr3(void) { u64 v; __asm__ volatile("mov %%cr3,%0":"=r"(v)); return v; }
static inline u64 read_cr4(void) { u64 v; __asm__ volatile("mov %%cr4,%0":"=r"(v)); return v; }
static inline void write_cr0(u64 v) { __asm__ volatile("mov %0,%%cr0":"=r"(v):"r"(v)); }
static inline void write_cr3(u64 v) { __asm__ volatile("mov %0,%%cr3":"=r"(v):"r"(v)); }
static inline void write_cr4(u64 v) { __asm__ volatile("mov %0,%%cr4":"=r"(v):"r"(v)); }

static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }
static inline void pause(void) { __asm__ volatile("pause"); }

#define MFENCE() __asm__ volatile("mfence" ::: "memory")
#define LFENCE() __asm__ volatile("lfence" ::: "memory")
#define SFENCE() __asm__ volatile("sfence" ::: "memory")
#define CPU_RELAX() do { __asm__ volatile("pause" ::: "memory"); } while (0)

/* freestanding string helpers (implemented in mm/kstring.c) */
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strncpy(char *dst, const char *src, size_t n);

#endif /* MC_IO_H */
