/* ============================================================
 * AMC OS - Bağımsız Mikrokernelsi (Hybrid: modüller + IPC mesajları)
 * kernel/main.c — ana giriş noktası
 *
 * Mimari:
 *   - Tamamen bağımsız çekirdek (Linux/FreeBSD kodu TİCARİ/LGPL
 *     kaynaklarından bağımsız, sıfırdan yazılmıştır).
 *   - Modüler sürücü sistemi: her sürücü .ko benzeri dinamik modül.
 *   - Kullanıcı alanı servisleri IPC (mesaj kuyruğu) ile çalışır.
 *   - SMP (çok çekirdek) desteği: per-CPU veriler + IPI.
 * ============================================================ */

#include <stdint.h>
#include <stdbool.h>
#include "kernel.h"
#include "mm/pmm.h"       /* fiziksel sayfa yöneticisi      */
#include "mm/vmm.h"       /* sanal bellek / page table       */
#include "ipc/msgq.h"     /* mesaj kuyruğu (IPC)             */
#include "drivers/driver.h"
#include "drivers/audio/realtek_hda.h"  /* Realtek HD Audio ailesi */
#include "drivers/video/gpu_common.h"   /* GPU işlekçi (scheduler) */
#include "drivers/usb/xhci.h"           /* USB 3.0 host controller */
#include "drivers/usb/ehci.h"           /* USB 2.0 host controller */
#include "drivers/net/rtl8169.h"        /* Realtek RTL8169 Ethernet */
#include "fs/amcfs.h"                   /* AMC FS (journaling)      */
#include "sched/scheduler.h"            /* O(1) öncelikli zamanlayıcı */

#define KERNEL_VERSION "AMC-Kernel 0.4.0-rc1 (bağımsız/hybrid)"

static struct boot_info g_boot;
static uint64_t         g_cpu_count;

/* ---- Erken konsol (VGA metin buffer + serial 16550) ---- */
void console_init(void);
void kprintf(const char *fmt, ...);

/* ---- PCI taraması: tüm cihazları bulup sürücü eşleştirmesi ---- */
static void pci_enumerate(void) {
    kprintf("[pci] tarama basladi (bus 0..255)\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint16_t dev = 0; dev < 32; dev++) {
            for (uint16_t fn = 0; fn < 8; fn++) {
                uint32_t vid = pci_read_vendor(bus, dev, fn);
                if (vid == 0xFFFFFFFF || vid == 0) continue;
                uint32_t did = pci_read_device(bus, dev, fn);
                uint32_t cls = pci_read_class(bus, dev, fn);
                driver_match_and_bind(bus, dev, fn, vid, did, cls);
            }
        }
    }
}

/* ---- Sürücü modüllerini yükle (.ko dosyaları AMC FS'den) ---- */
static void load_driver_modules(void) {
    /* Öncelik sırasıyla: önce depolama+USB, sonra ses/video/net */
    const char *boot_modules[] = {
        "usb_ehci.ko",    /* USB 2.0 */
        "usb_xhci.ko",    /* USB 3.0/3.1 */
        "ahci.ko",        /* SATA */
        "nvme.ko",        /* NVMe SSD */
        "amcfs.ko",       /* dosya sistemi */
        NULL
    };
    const char *late_modules[] = {
        "realtek_hda.ko", /* ALC887/ALC892/ALC1150/ALC1220/ALC4080... */
        "intel_gpu.ko",   /* i915/igc benzeri kendi işlekçimiz */
        "amdgpu.ko",      /* amdgpu benzeri DC display core */
        "nvidia_stub.ko", /* nouveau tarzı açık stub (Temperary) */
        "rtl8169.ko",     /* Realtek GbE */
        "rtl8192.ko",     /* Realtek WiFi (MCC uyumlu çerçeve) */
        NULL
    };
    module_load_list(boot_modules);
    module_load_list(late_modules);
}

void kernel_main(struct boot_info bi) {
    g_boot = bi;

    console_init();
    kprintf("\n");
    kprintf("=========================================\n");
    kprintf("   %s\n", KERNEL_VERSION);
    kprintf("   Hedef: masaüstü + günlük kullanım + yazilimci\n");
    kprintf("=========================================\n");

    /* 1. Bellek alt yapısı */
    pmm_init(bi.memmap, bi.memmap_entries);
    vmm_init();
    kprintf("[mm ] fiziksel havuz: %llu MB, sanal uzay: 128TB (4-seviye PT)\n",
            pmm_total_pages() * 4096ULL / (1024*1024));

    /* 2. Kesmeler + Zamanlayıcı + SMP */
    idt_init();
    timer_init(1000);              /* 1000 Hz tick */
    g_cpu_count = smp_probe_bsp(); /* AP'leri uyandır (INIT-SIPI-SIPI) */
    kprintf("[cpu] aktif cekirdek sayisi: %llu\n", g_cpu_count);

    /* 3. Zamanlayıcı ve IPC */
    sched_init(g_cpu_count);
    ipc_init();                    /* mesaj kuyrukları + senkronize RPC */

    /* 4. Donanım keşfi */
    acpi_parse_tables();           /* MADT/TCPA -> CPU, PIC, HPET */
    pci_enumerate();
    load_driver_modules();

    /* 5. Kök dosya sistemi */
    if (amcfs_mount_root() == 0) {
        kprintf("[fs ] AMCFS kok mount edildi (journaling aktif)\n");
    } else {
        kprintf("[fs ] kok disk bulunamadi -> RAM disk (initramfs)\n");
        initramfs_extract(bi.initrd_ptr, bi.initrd_size);
    }

    /* 6. İlk kullanıcı süreçleri: init -> oturum -> masaüstü */
    pid_t init_pid = proc_spawn("/bin/init", PROC_USER | PROC_EXEC);
    kprintf("[proc] /bin/init pid=%d calisiyor\n", init_pid);

    /* Çekirdek artık scheduler'a tam yetki verir; idle döngüsü */
    sched_enable_preemption();
    for (;;) cpu_idle();           /* hlt tabanlı idle, kesmelerle uyanır */
}

/* ---- Idle döngüsü: güç tasarrufu (C-state) ---- */
static inline void cpu_idle(void) {
    __asm__ volatile ("sti; hlt");
}
