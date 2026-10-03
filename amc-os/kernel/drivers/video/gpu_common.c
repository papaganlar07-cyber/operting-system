/* ============================================================
 * AMC OS — GPU İşlekçisi (Command Scheduler) + Ekran Kartı Sürücüleri
 * drivers/video/gpu_common.c
 *
 * Desteklenen aileler (çoğu masaüstü/laptop GPU'su):
 *   Intel : HD Graphics 400..770, Iris Xe, Arc A-series (i915/igc benzeri)
 *   AMD   : RDNA/RDNA2/RDNA3, GCN (radeonsi benzeri DC - display core)
 *   NVIDIA: open-GPU kanal (nvidia-open çekirdek arayüzü tarzı stub)
 *           + nouveau-benzeri temel framebuffer (NVE/NVC0 model numarası)
 *
 * Ortak çerçeve:
 *   - GEM benzeri buffer yönetimi (tiled/linear, dma-bol paylaşımı)
 *   - Ring buffer komut işleme + GPU side-band kesmeleri
 *   - VBlank zamanlaması ile triple-buffering (tear-free masaüstü)
 *   - KMS benzeri mod setting: 640x480 .. 4K@120Hz, çoklu monitör
 * ============================================================ */

#include "gpu_common.h"
#include "../../kernel.h"

/* ================== Ortak DRM benzeri yapılar ================== */

struct gpu_device {
    enum gpu_family family;
    struct pci_dev *pdev;
    volatile uint8_t *mmio;
    size_t          mmio_size;

    /* Komut halkası (ring buffer) */
    void   *ring_virt;
    uint64_t ring_phys;
    uint32_t ring_size;        /* bayt; her zaman 2^n     */
    uint32_t ring_wr, ring_rd; /* write/read indeksleri    */

    /* Framebuffer / KMS */
    struct display_mode mode;
    fb_info            front, back;   /* double buffering   */
    int                crtc_count;
    bool               vsync_pending;

    spinlock_t         ring_lock;
};

static struct gpu_device g_gpus[MAX_GPUS];
static int g_gpu_count;

/* ================== Mod ayarlama (KMS benzeri) ================== */

int gpu_set_mode(int gpu_idx, uint16_t w, uint16_t h, uint16_t refresh) {
    struct gpu_device *g = &g_gpus[gpu_idx];
    struct edid_info edid;

    if (read_edid(g, &edid) < 0)
        return -ENODEV;

    /* EDID destek listesinde en yakın modu seç */
    const struct display_mode *m = edid_best_mode(&edid, w, h, refresh);
    if (!m) return -EINVAL;

    g->mode = *m;
    /* PLL kurulumu aileye özel (intel ddi / amd dc / nv pior) */
    switch (g->family) {
    case GPU_INTEL:  intel_crtc_enable(g, m);  break;
    case GPU_AMD:    amdgpu_dc_enable(g, m);   break;
    case GPU_NVIDIA: nv_disp_enable(g, m);     break;
    }
    /* Çizgi süresi, blank aralıkları ve scanout adresini yaz */
    program_scanout(g, g->front.phys_addr, m);
    kprintf("[gpu] mod: %ux%u@%u Hz (%s)\n", m->hactive, m->vactive,
            m->vrefresh, gpu_family_name(g->family));
    return 0;
}

/* ================== Komut halkası (ring submission) ================== */

/* Kullanıcı alanı (masaüstü compositor / oyun) buradan komut basar */
int gpu_submit_cmds(int gpu_idx, const uint32_t *cmds, uint32_t count) {
    struct gpu_device *g = &g_gpus[gpu_idx];
    scoped_spinlock(&g->ring_lock);

    uint32_t space = (g->ring_rd - g->ring_wr - 8) % g->ring_size;
    if (space < count * 4) return -EAGAIN;   /* halka dolu: throttle */

    for (uint32_t i = 0; i < count; i++) {
        ring_write32(g, cmds[i]);
    }
    ring_flush(g);                    /* WC write-combining flush */
    doorbell_kick(g);                 /* MMIO tail register → GPU uyanır */
    return 0;
}

/* Flip: back buffer'ı scanout yap, VBlank'te tamamla */
int gpu_page_flip(int gpu_idx, uint64_t new_fb_phys, struct fence **out) {
    struct gpu_device *g = &g_gpus[gpu_idx];
    struct fence *f = fence_create();
    queue_vblank_callback(g, f, new_fb_phys);
    *out = f;
    return 0;
}

/* ================== VBlank kesmesi ================== */

void gpu_vblank_irq(struct gpu_device *g) {
    g->vsync_pending = false;
    fence_signal_all(g);              /* bekleyen flip'leri tamamla */
    ipc_post_msg(SVC_COMPOSITOR, GPU_EVT_VBLANK, g - g_gpus, 0);
}

/* ================== Buffer yönetimi (GEM benzeri) ================== */

uint32_t gpu_bo_create(int gpu_idx, size_t size, uint32_t flags) {
    /* Tiled mı linear mi? Intel gen X tiling, AMD row-interleave... */
    struct bo *bo = bo_alloc(size, flags);
    if (flags & BO_SCANOUT)
        bo_map_scanout(&g_gpus[gpu_idx], bo);
    return bo->handle;
}

/* dma-buf: aynı buffer'ı ses/video/USB aracılarla sıfır-kopya paylaş */
int gpu_prime_export(int gpu_idx, uint32_t handle, int *fd_out) {
    *fd_out = dmabuf_create_from_bo(handle);
    return 0;
}

/* ================== Aile özgü başlatmalar ================== */

static int intel_gpu_init(struct pci_dev *dev) {
    struct gpu_device *g = gpu_register(GPU_INTEL, dev);
    g->mmio = ioremap(dev->bar[0], MiB(16));   /* GTT + MMIO region */
    g->mmio_size = MiB(16);
    intel_ring_init(g, SZ(MiB(8)));
    intel_gt_clocks_setup(g);                  /* RC6 güç yönetimi açık */
    g->crtc_count = intel_num_display_pipes(g);
    kprintf("[gpu] Intel %s bulundu, %d pipe\n",
            intel_codename(dev->device), g->crtc_count);
    return 0;
}

static int amdgpu_init(struct pci_dev *dev) {
    struct gpu_device *g = gpu_register(GPU_AMD, dev);
    g->mmio = ioremap(dev->bar[2], MiB(16));   /* MMIO APERTURE */
    amdgpu_ring_init(g, GFX_RING, SZ(MiB(8)));
    amdgpu_dc_early_init(g);                   /* Display Core */
    smu_load_firmware(g);                      /* güç/fan kontrolü */
    kprintf("[gpu] AMD %s (DC hazir)\n", amdgpu_asic_name(dev->device));
    return 0;
}

static int nvidia_init(struct pci_dev *dev) {
    struct gpu_device *g = gpu_register(GPU_NVIDIA, dev);
    g->mmio = ioremap(dev->bar[1], MiB(16));
    /* Açık kanaldan temel push buffer; kapalı MME için stub bırakılır */
    nvkm_engine_init(g, ENGINE_PGRAPH | ENGINE_DISP);
    return 0;
}

/* ---- PCI ID tabloları (her aile için geniş liste) ---- */

static const struct pci_device_id intel_ids[] = {
    { 0x8086, 0x0412 }, /* HD 4600 Haswell      */
    { 0x8086, 0x0A16 }, /* HD Graphics mobile   */
    { 0x8086, 0x1912 }, /* Skylake HD 530       */
    { 0x8086, 0x3E98 }, /* UHD 630 Coffee Lake  */
    { 0x8086, 0x46A6 }, /* Alder Lake UHD 770   */
    { 0x8086, 0x56A0 }, /* Arc A770             */
    {}
};
static const struct pci_device_id amd_ids[] = {
    { 0x1002, 0x67DF }, /* RX 580 Polaris       */
    { 0x1002, 0x731F }, /* RX 6800 XT Navi21    */
    { 0x1002, 0x163E }, /* RX 7900 XTX Navi31   */
    { 0x1002, 0x13C1 }, /* Ryzen 780M iGPU      */
    {}
};
static const struct pci_device_id nv_ids[] = {
    { 0x10DE, 0x2204 }, /* RTX 3060             */
    { 0x10DE, 0x2684 }, /* RTX 4070             */
    { 0x10DE, 0x2A05 }, /* RTX 5070 (Blackwell) */
    {}
};

DEFINE_PCI_DRIVER(intel_gpu, intel_ids, intel_gpu_init,
                  MODULE_DESCRIPTION("Intel iGPU/dGPU command scheduler"));
DEFINE_PCI_DRIVER(amdgpu, amd_ids, amdgpu_init,
                  MODULE_DESCRIPTION("AMD GCN/RDNA display+compute driver"));
DEFINE_PCI_DRIVER(nvidia_stub, nv_ids, nvidia_init,
                  MODULE_DESCRIPTION("NVIDIA open-channel base driver"));
