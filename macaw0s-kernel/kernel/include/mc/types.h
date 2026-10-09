/* MacawOS kernel — fundamental freestanding types & HHDM address helpers */
#ifndef MC_TYPES_H
#define MC_TYPES_H

typedef signed char        i8;
typedef unsigned char      u8;
typedef signed short       i16;
typedef unsigned short     u16;
typedef signed int         i32;
typedef unsigned int       u32;
typedef signed long long   i64;
typedef unsigned long long u64;
typedef u64                size_t;
typedef i64                ssize_t;
typedef u64                uintptr_t;
typedef i64                intptr_t;

typedef _Bool              bool;
#define true  1
#define false 0

#define NULL ((void *)0)

/* Kernel virtual base (linker.ld) and Higher-Half Direct Map base.
 * HHDM: virt = phys + HHDM_BASE for the first BOOT_MAP_GIB of RAM. */
#define KERNEL_VBASE   0xFFFFFFFF80000000ULL
#define HHDM_BASE      0xFFFF800000000000ULL
#define HHDM_SIZE      0x40000000ULL        /* 1 GiB identity-window bytes */
#define BOOT_MAP_GIB   1ULL                 /* low 1 GiB mapped at boot    */

#define PHYS_TO_VIRT(p) ((void *)((uintptr_t)(p) + HHDM_BASE))
#define VIRT_TO_PHYS(v) ((u64)((uintptr_t)(v) - HHDM_BASE))

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((typeof(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((typeof(x))(a) - 1))
#define ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))

#define PACKED   __attribute__((packed))
#define NORETURN __attribute__((noreturn))
#define ALIGNED(n) __attribute__((aligned(n)))
#define UNUSED(x) ((void)(x))

#endif /* MC_TYPES_H */
