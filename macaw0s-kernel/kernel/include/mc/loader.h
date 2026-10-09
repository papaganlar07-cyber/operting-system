/* MacawOS kernel — Universal Execution Layer: ELF64 / PE32+ / Mach-O loader */
#ifndef MC_LOADER_H
#define MC_LOADER_H
#include "mc/types.h"

typedef enum {
    EXEC_UNKNOWN = 0,
    EXEC_ELF64,      /* Linux / native Macaw binaries               */
    EXEC_PE64,       /* Windows .exe (PE32+)                        */
    EXEC_MACHO64     /* macOS / iOS fat or thin Mach-O              */
} exec_format_t;

#define MAX_LOAD_SEGMENTS 16

typedef struct {
    u64 vaddr;        /* destination virtual address                */
    u64 filesz;       /* bytes copied from image                    */
    u64 memsz;        /* total size (filesz..memsz zero-filled)     */
    bool writable;
    bool executable;
} load_segment_t;

typedef struct {
    exec_format_t format;
    const char   *fmt_name;
    load_segment_t segs[MAX_LOAD_SEGMENTS];
    u32          seg_count;
    u64          entry;         /* resolved entry point             */
    u64          image_base;    /* preferred/base VA                */
    i64          reloc_delta;   /* applied PIE/ASLR slide           */
    bool         is_pie;
    bool         ok;
    char         err[96];
} exec_image_t;

exec_format_t exec_probe(const u8 *img, size_t len);
int  exec_load(addr_space_t *as, const u8 *img, size_t len, exec_image_t *out);
void exec_dump(const exec_image_t *im);

/* per-format internal entry points (exposed for tests) */
int  elf64_load(addr_space_t *as, const u8 *img, size_t len, exec_image_t *out);
int  pe64_load (addr_space_t *as, const u8 *img, size_t len, exec_image_t *out);
int  macho64_load(addr_space_t *as, const u8 *img, size_t len, exec_image_t *out);
#endif
