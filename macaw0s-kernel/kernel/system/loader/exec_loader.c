/* MacawOS kernel — Universal Execution Layer: ELF64 / PE32+ / Mach-O64 probe+load.
 * Images are read from fully-mapped buffers (kernel heap or HHDM); segments
 * are copied into fresh anonymous pages inside the target address space. */
#include "mc/loader.h"
#include "mc/mm.h"
#include "mc/io.h"
#include "mc/vga.h"

static void errf(exec_image_t *out, const char *msg)
{
    strncpy(out->err, msg, sizeof(out->err) - 1);
    out->err[sizeof(out->err) - 1] = 0;
}

exec_format_t exec_probe(const u8 *img, size_t len)
{
    if (len < 16) return EXEC_UNKNOWN;
    if (img[0] == 0x7F && img[1] == 'E' && img[2] == 'L' && img[3] == 'F')
        return EXEC_ELF64;
    if (img[0] == 'M' && img[1] == 'Z')
        return EXEC_PE64;                            /* MZ stub            */
    u32 m = *(const u32 *)img;
    if (m == 0xFEEDFACEu || m == 0xFEEDFACFu || m == 0xCAFEBABEu ||
        m == 0xBEBAFECAu || m == 0x0B1D0006u ||
        __builtin_bswap32(m) == 0xFEEDFACFu || __builtin_bswap32(m) == 0xCAFEBABEu)
        return EXEC_MACHO64;
    return EXEC_UNKNOWN;
}

static int alloc_and_copy(addr_space_t *as, u64 va, const u8 *src,
                          u64 filesz, u64 memsz, bool writable, bool exec)
{
    u64 start = ALIGN_DOWN(va, PAGE_SIZE);
    u64 end   = ALIGN_UP(va + memsz, PAGE_SIZE);
    if (end <= start) return 0;
    u64 flags = PTE_PRESENT | (writable ? PTE_WRITE : 0) | PTE_USER |
                (exec ? 0 : PTE_NX);
    for (u64 v = start; v < end; v += PAGE_SIZE) {
        u64 phys = pmm_alloc_frame();
        u8 *dst = PHYS_TO_VIRT(phys);
        memset(dst, 0, PAGE_SIZE);
        u64 lo = v > va ? v : va;                    /* intersection with image */
        u64 hi = (v + PAGE_SIZE) < (va + filesz) ? (v + PAGE_SIZE) : (va + filesz);
        if (hi > lo) memcpy(dst + (lo - v), src + (lo - va), hi - lo);
        if (vmm_map_page(as, v, phys, flags) != 0) return -1;
    }
    return 0;
}

/* ---- ELF64 ------------------------------------------------------------- */
struct elf_ident { u8 magic[4]; u8 class; u8 data; u8 version; u8 osabi; u8 pad[8]; } PACKED;
struct elf_hdr {
    struct elf_ident ident;
    u16 type, machine; u32 version;
    u64 entry, phoff, shoff;
    u32 flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} PACKED;
struct elf_phdr {
    u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align;
} PACKED;

#define PT_LOAD 1
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

int elf64_load(addr_space_t *as, const u8 *img, size_t len, exec_image_t *out)
{
    out->format = EXEC_ELF64; out->fmt_name = "ELF64";
    const struct elf_hdr *eh = (const struct elf_hdr *)img;
    if (len < sizeof(*eh) || eh->ident.class != 2 || eh->ident.data != 1) {
        errf(out, "not ELF64 LSB"); return -1;
    }
    if (eh->phnum > MAX_LOAD_SEGMENTS) { errf(out, "too many PH"); return -1; }
    out->is_pie = (eh->type == 3);                   /* ET_DYN             */
    out->image_base = eh->entry & ~0x1FFFFFULL;      /* heuristic base     */
    out->reloc_delta = 0;

    if (eh->phoff + (u64)eh->phnum * sizeof(struct elf_phdr) > len) {
        errf(out, "PH table beyond image"); return -1;
    }
    const struct elf_phdr *ph = (const struct elf_phdr *)(img + eh->phoff);
    u32 seg_n = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        if (ph[i].offset + ph[i].filesz > len) { errf(out, "PH beyond image"); return -1; }
        u64 va = ph[i].vaddr;
        if (alloc_and_copy(as, va, img + ph[i].offset, ph[i].filesz, ph[i].memsz,
                           !!(ph[i].flags & PF_W), !!(ph[i].flags & PF_X)) != 0) {
            errf(out, "map fail in PT_LOAD"); return -1;
        }
        load_segment_t s;
        s.vaddr = va; s.filesz = ph[i].filesz; s.memsz = ph[i].memsz;
        s.writable = !!(ph[i].flags & PF_W); s.executable = !!(ph[i].flags & PF_X);
        out->segs[seg_n++] = s;
    }
    out->seg_count = seg_n;
    out->entry = eh->entry;
    out->ok = true;
    return 0;
}

/* ---- PE32+ ------------------------------------------------------------- */
struct pe_dos_tail { u8 rest[60]; u32 e_lfanew; } PACKED;
struct pe_file_hdr { u16 machine, nsec; u32 tds; u32 sptr; u16 szopt, chars; } PACKED;
struct pe_opt_hdr64 { u16 magic; u8 llink, majl; u32 code, init, uv;
                      u64 entry; u32 baserva; u32 sectal, datarva;
                      u64 imagebase; u16 alsect, alimage; u16 osmaj, osmin;
                      u16 imgmaj, imgmin; u32 sizeofimg; u32 headres;
                      u32 nrsv; } PACKED;
struct pe_sec_hdr { u8 name[8]; u32 vsize; u32 vaddr; u32 rawsize; u32 rawptr;
                    u32 reloc, lnptr, nlreloc, nlname; u32 chars; } PACKED;

#define IMAGE_SCN_MEM_EXECUTE 0x20000000u
#define IMAGE_SCN_MEM_WRITE   0x80000000u

int pe64_load(addr_space_t *as, const u8 *img, size_t len, exec_image_t *out)
{
    out->format = EXEC_PE64; out->fmt_name = "PE32+";
    if (len < 0x40) { errf(out, "short DOS header"); return -1; }
    if (img[0] != 'M' || img[1] != 'Z') { errf(out, "no MZ"); return -1; }
    u32 nt = *(const u32 *)(img + 0x3C);
    if (nt + 4 + sizeof(struct pe_file_hdr) + 16 > len ||
        *(const u32 *)(img + nt) != 0x00004550u) {
        errf(out, "no PE signature"); return -1;
    }
    const struct pe_file_hdr *fh = (const struct pe_file_hdr *)(img + nt + 4);
    const struct pe_opt_hdr64 *oh = (const struct pe_opt_hdr64 *)(img + nt + 24);
    if (fh->szopt < 112 || nt + 24 + fh->szopt > len) { errf(out, "bad optional hdr"); return -1; }
    if (oh->magic != 0x20B) { errf(out, "not PE32+ (magic)"); return -1; }
    if (fh->machine != 0x8664) { errf(out, "not AMD64 machine"); return -1; }

    const struct pe_sec_hdr *sh = (const struct pe_sec_hdr *)
        ((const u8 *)oh + fh->szopt);
    u32 nsec = fh->nsec;
    if (nsec > MAX_LOAD_SEGMENTS) nsec = MAX_LOAD_SEGMENTS;
    if (nt + 24 + fh->szopt + nsec * sizeof(struct pe_sec_hdr) > len) {
        errf(out, "section table beyond image"); return -1;
    }
    out->image_base = oh->imagebase;
    out->is_pie = true;                              /* ASLR applied later  */
    out->reloc_delta = 0;
    u32 seg_n = 0;
    for (u32 i = 0; i < nsec; i++) {
        u64 vsz = sh[i].vsize ? sh[i].vsize : sh[i].rawsize;
        if (!vsz) continue;
        u64 va = out->image_base + sh[i].vaddr;
        u64 copy = sh[i].rawsize < vsz ? sh[i].rawsize : vsz;
        if ((u64)sh[i].rawptr + sh[i].rawsize > len) { errf(out, "section beyond image"); return -1; }
        bool w = (sh[i].chars & IMAGE_SCN_MEM_WRITE) != 0;
        bool x = (sh[i].chars & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (alloc_and_copy(as, va, img + sh[i].rawptr, copy, vsz, w, x) != 0) {
            errf(out, "map section failed"); return -1;
        }
        load_segment_t s = { va, copy, vsz, w, x };
        out->segs[seg_n++] = s;
    }
    out->seg_count = seg_n;
    out->entry = out->image_base + oh->entry;
    out->ok = true;
    return 0;
}

/* ---- Mach-O 64-bit ----------------------------------------------------- */
struct mach_hdr64 { u32 magic, cputype, cpusub, filetype;
                    u32 ncmds, sizeofcmds, flags; u32 reserved; } PACKED;
struct mach_cmd { u32 cmd, cmdsize; } PACKED;
struct seg_cmd64 { u32 cmd, cmdsize; char segname[16]; u64 vmaddr, vmsize;
                   u64 fileoff, filesize; u32 maxprot, initprot;
                   u32 nsects, flags; } PACKED;

#define LC_SEGMENT_64  0x19u
#define MH_MAGIC_64    0xFEEDFACFu
#define FAT_MAGIC_BE   0xCAFEBABEu
#define CPU_TYPE_X86_64_LE   0x01000007u
#define CPU_TYPE_X86_64_BE   0x07000001u

int macho64_load(addr_space_t *as, const u8 *img, size_t len, exec_image_t *out)
{
    out->format = EXEC_MACHO64; out->fmt_name = "Mach-O64";
    u32 m = *(const u32 *)img;
    const u8 *p = img; size_t pl = len;
    if (m == FAT_MAGIC_BE || m == __builtin_bswap32(FAT_MAGIC_BE)) {
        bool be = (m == FAT_MAGIC_BE);
        u32 nf = be ? __builtin_bswap32(*(const u32 *)(img + 4))
                    : *(const u32 *)(img + 4);
        if (nf == 0 || 8 + (u64)nf * 20 > len) { errf(out, "fat header corrupt"); return -1; }
        const u8 *arch = img + 8;                  /* first fat_arch entry  */
        u32 a_off = be ? __builtin_bswap32(*(const u32 *)(arch + 8))
                       : *(const u32 *)(arch + 8);
        u32 a_sz  = be ? __builtin_bswap32(*(const u32 *)(arch + 12))
                       : *(const u32 *)(arch + 12);
        if ((u64)a_off + a_sz > len) { errf(out, "fat slice out of range"); return -1; }
        p = img + a_off; pl = a_sz;
        m = *(const u32 *)p;
    }
    if (m != MH_MAGIC_64) { errf(out, "not thin Mach-O64"); return -1; }
    if (pl < sizeof(struct mach_hdr64)) { errf(out, "truncated mach header"); return -1; }
    const struct mach_hdr64 *mh = (const struct mach_hdr64 *)p;
    if (mh->cputype != CPU_TYPE_X86_64_LE && mh->cputype != CPU_TYPE_X86_64_BE) {
        errf(out, "Mach-O not x86_64"); return -1;
    }
    const u8 *cmd = p + sizeof(*mh);
    out->image_base = 0x0000000100000000ULL;         /* canonical text window */
    out->is_pie = true;
    u64 entry_off = 0; bool have_main = false;
    u32 seg_n = 0;
    for (u32 i = 0; i < mh->ncmds; i++) {
        if ((u64)(cmd - p) + 8 > pl) { errf(out, "LC overflow"); return -1; }
        const struct mach_cmd *c = (const struct mach_cmd *)(cmd);
        if (c->cmdsize < 8 || (u64)(cmd - p) + c->cmdsize > pl) {
            errf(out, "bad LC size"); return -1;
        }
        if (c->cmd == LC_SEGMENT_64) {
            const struct seg_cmd64 *sc = (const struct seg_cmd64 *)c;
            if (sc->filesize) {
                u64 va = sc->vmaddr;                 /* keep original VM addr */
                if ((u64)sc->fileoff + sc->filesize > pl) {
                    errf(out, "__TEXT beyond file"); return -1;
                }
                bool w = (sc->initprot & 2) != 0;
                bool x = (sc->initprot & 4) != 0;
                if (alloc_and_copy(as, va, p + sc->fileoff, sc->filesize,
                                   sc->vmsize, w, x) != 0) {
                    errf(out, "map segment failed"); return -1;
                }
                if (seg_n < MAX_LOAD_SEGMENTS) {
                    load_segment_t s = { va, sc->filesize, sc->vmsize, w, x };
                    out->segs[seg_n++] = s;
                }
            }
        } else if (c->cmd == 0x80000028u) {          /* LC_MAIN (reserved bit) */
            /* struct layout: cmd,cmdsize,entryoff(u64),stacksize(u64) */
            entry_off = *(const u64 *)(cmd + 8);
            have_main = true;
        }
        cmd += c->cmdsize;
    }
    out->seg_count = seg_n;
    out->reloc_delta = 0;
    out->entry = have_main ? (0x100000000ULL + entry_off) : 0;
    out->ok = true;
    return 0;
}

/* ---- dispatcher -------------------------------------------------------- */
int exec_load(addr_space_t *as, const u8 *img, size_t len, exec_image_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (exec_probe(img, len)) {
    case EXEC_ELF64:   return elf64_load(as, img, len, out);
    case EXEC_PE64:    return pe64_load(as, img, len, out);
    case EXEC_MACHO64: return macho64_load(as, img, len, out);
    default:
        errf(out, "unknown executable format");
        return -1;
    }
}

void exec_dump(const exec_image_t *im)
{
    kprintf("[exec] %s entry=%#lx base=%#lx pie=%d segs=%u\n",
            im->fmt_name ? im->fmt_name : "?",
            (unsigned long)im->entry, (unsigned long)im->image_base,
            im->is_pie, im->seg_count);
    for (u32 i = 0; i < im->seg_count; i++)
        kprintf("       seg%u va=%#lx f=%lu m=%lu %c%c\n", i,
                (unsigned long)im->segs[i].vaddr,
                (unsigned long)im->segs[i].filesz,
                (unsigned long)im->segs[i].memsz,
                im->segs[i].writable ? 'W' : '-',
                im->segs[i].executable ? 'X' : '-');
}
