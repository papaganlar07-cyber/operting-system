/* ============================================================
 * AMC OS — Port Katmanı: Linux / FreeBSD / NetBSD / OpenBSD
 *                    ikili (binary) uyumluluk + kaynak port sistemi
 * ports/portlayer.c  (pkgd servisinin çekirdeği)
 *
 * İKİ KATMAN:
 *
 * A) Kaynak seviyesi port sistemi (Homebrew/ports tarzı):
 *    Her port bir "recipe" YAML'ıdır: kaynak indir → yama → derle.
 *    `amcport install firefox` → recipe'i çöz, bağımlılık grafiğini
 *    topolojik sırala, sandbox içinde derle, /opt/amc/pkg'e kur.
 *
 * B) İkili (binary) uyumluluk katmanları — syscall emülasyonu:
 *    linux_compat : Linux ELF + syscall tablosu (binfmt benzeri)
 *    freebsd_compat: FreeBSD ABI (sysarch, kqueue→epoll köprüsü)
 *    netbsd/openbsd_compat: benzer çerçeve (Rump kernel entegrasyonu)
 *    windows_compat: Wine-benzeri PE yükleyici + Win32 API şeması
 *                    (ntdll/kernel32/gdi32/user32 çağrı geçişleri)
 * ============================================================ */

#include <amc/syscall.h>
#include <amc/ipc.h>

/* ================== A) Recipe tabanlı port sistemi ================== */

struct port_recipe {
    char     name[64];
    char     version[32];
    char     source_url[256];
    char     checksum_sha256[65];
    char    *patches[16];          /* amc-özgün yamalar            */
    char     depends[32][64];      /* bağımlı port adları          */
    int      n_depends;
    enum { SRC_NATIVE, SRC_LINUX_ABI, SRC_FREEBSD_ABI } abi_target;
    char     build_cmd[512];       /* ./configure && make ...      */
};

/* ports ağacı: /var/ports/<kategori>/<isim>/recipe.yaml */
static struct port_recipe load_recipe(const char *category, const char *name);

/* Bağımlılık çözümleyici: döngü kontrolü + topolojik sıra */
static int resolve_deps(struct port_recipe *r, struct dep_graph *g) {
    for (int i = 0; i < r->n_depends; i++) {
        struct port_recipe sub = load_recipe_by_name(r->depends[i]);
        if (graph_has_edge(g, sub.name, r->name))
            return -ECIRCULAR;                 /* döngüsel bağımlılık */
        graph_add_node(g, &sub);
        if (resolve_deps(&sub, g) < 0) return -1;
    }
    return 0;
}

/* Kurulum: izole dizinde derle, sonra atomik taşı */
static int build_port(struct port_recipe *r) {
    char workdir[256];
    snprintf(workdir, sizeof(workdir), "/var/ports/work/%s-%s",
             r->name, r->version);
    mkdir_p(workdir, 0755);

    /* 1. indir + doğrula */
    if (download_file(r->source_url, workdir) < 0)   return -EIO;
    if (verify_sha256(workdir, r->checksum_sha256))  return -EBADMSG;

    /* 2. aç + yama */
    extract_archive(workdir);
    for (int i = 0; i < 16 && r->patches[i]; i++)
        apply_patch(workdir, r->patches[i]);

    /* 3. derle (yalnızca okunabilir sistem + yazılabilir workdir
          chroot sandbox'ta; ağ kapalı, CPU/RAM kota zorunlu) */
    sandbox_exec(workdir, r->build_cmd,
                 &(struct sandbox_cfg){ .net = false, .mem_mb = 4096 });

    /* 4. paketle + imzala + kur */
    pkg_sign(workdir, keyring_get("amc-ports"));
    return pkg_install(workdir, "/opt/amc/pkg");
}

int cmd_amcport_install(const char *spec) {   /* "firefox>=120" vb. */
    struct dep_graph g;
    struct port_recipe root = parse_spec(spec, &g);
    if (resolve_deps(&root, &g) < 0) {
        fprintf(stderr, "Dongusel bagimlilik cozulemedi!\n");
        return 1;
    }
    for_each_topo(g, p) {
        printf("[port] %s surumu %s derleniyor...\n", p->name, p->version);
        if (build_port(p) < 0) return rollback_and_fail(p);
    }
    db_record_install(&root);                  /* amcport list için */
    return 0;
}

/* ================== B) Linux ikili uyumluluk katmanı ================== */

/* binfmt kaydı: ELF header magic + interpreter = /compat/linux/ld.so */
static const struct { int nr; void *handler; } linux_syscall_map[] = {
    [0]   = { SYS_restart },
    [1]   = { SYS_getcwd },
    [2]   = { SYS_fork },        [3]   = { SYS_read },
    [9]   = { SYS_mmap },        [10]  = { SYS_mprotect },
    [39]  = { SYS_epoll_create },[232] = { SYS_epoll_wait },
    [56]  = { SYS_openat },      [72]  = { SYS_poll },
    [202] = { SYS_futex },       [218] = { SYS_set_tid_address },
    /* ~350 numara birebir eşlenir; AMC native numaralardan ayrılmış
       alan kullanılır ki aynı syscall farklı anlamlar taşımasın */
};

int linux_abi_dispatch(struct regs *r) {
    int nr = r->rax;
    if (nr >= 0 && nr < LINUX_NR_MAX && linux_syscall_map[nr].handler)
        return translate_args_then_call(linux_syscall_map[nr].handler, r);
    /* desteğimiz olmayan numara: logla + ENOSYS (sessiz çökme yok) */
    compat_log_unimplemented("linux", nr);
    return -ENOSYS;
}

/* ================== FreeBSD / NetBSD / OpenBSD katmanları ================== */

/* FreeBSD: sysarch(FSBASE/GSBASE), kqueue yerel! (biz de kqueue native) */
static int freebsd_abi_dispatch(struct regs *r) {
    switch (r->rax) {
    case FBSDT_KEVENT:  return native_kqueue(r);      /* birebir */
    case FBSDT_SYSARCH: return handle_sysarch(r);     /* TLS ayarla */
    case FBSDT_JAIL:    return -ENOSYS;               /* planlandı */
    default: return posix_common_translate(r);       /* POSIX ortak küme */
    }
}

/* NetBSD/OpenBSD: büyük çoğunluk POSIX → ortak çeviri katmanı.
   OpenBSD pledge()/unveil() güvenlik çağrıları bizim capability
   sistemimize birebir haritalanır (doğal uyum!). */
static int openbsd_abi_dispatch(struct regs *r) {
    switch (r->rax) {
    case OBSY_PLEDGE:  return cap_restrict_current(r->rdi); /* bizde var */
    case OBSY_UNVEIL:  return fs_visible_set(r->rdi, r->rsi);
    default: return posix_common_translate(r);
    }
}

/* ================== Windows (PE) uyumluluk çerçevesi ================== */

/* Amaç: basılı araçlar ve küçük oyunlar için Wine benzeri in-process
   şema. Tam kapsam hedef DEĞİL; çekirdek tarafında yalnızca:
     - PE/COFF yükleyici (image base relocation, SEH tablosu)
     - TEB/PEB kurulumu (gs segmenti ile!)
     - ntdll!Nt* çağrılarının NTSTATUS↔errno köprüsü */
static int pe_loader_load(const char *path, struct win_ctx *ctx) {
    struct image_nt_headers *nt = pe_parse_header(path);
    if (!nt) return -ENOEXEC;
    ctx->base = vm_reserve(nt->OptionalHeader.SizeOfImage);
    pe_reloc_apply(ctx, nt);              /* taban taşımalarını uygula */
    pe_tls_init(ctx);
    teb_peb_setup(ctx, gs_base_alloc());  /* x86-64 GS:TLS standardı */
    return 0;
}

static long win_stub_CreateFileA(const char *p, uint32_t access, ...) {
    int flags = win_access_to_posix(access);       /* GENERIC_READ→O_RDONLY */
    int fd = open(p, flags);
    return fd < 0 ? ntstatus_from_errno(fd) : (long)fd_as_handle(fd);
}

const struct win32_api_map {
    const char *dll, *fn; void *impl;
} win32_map[] = {
    { "kernel32", "CreateFileA",  win_stub_CreateFileA },
    { "kernel32", "ReadFile",     win_stub_ReadFile    },
    { "kernel32", "GetVersionEx", win_stub_GetVersion  },
    { "user32",   "MessageBoxA",  win_stub_MessageBox  }, /* X-benzeri */
    {}
};

/* ================== Servis döngüsü (pkgd) ================== */

int main(void) {
    uint32_t port = ipc_register_service("pkgd");
    register_binfmt("elf-linux",   linux_abi_dispatch);
    register_binfmt("elf-freebsd", freebsd_abi_dispatch);
    register_binfmt("elf-netbsd",  posix_common_translate);
    register_binfmt("elf-openbsd", openbsd_abi_dispatch);
    register_binfmt("pe-windows",  win_pe_dispatch);

    struct ipc_msg_hdr hdr; char buf[4096];
    for (;;) {
        ssize_t n = ipc_recv(port, &hdr, buf, sizeof(buf), 0);
        dispatch_pkg_request(hdr.code, buf, n);   /* install/remove/update */
    }
}
