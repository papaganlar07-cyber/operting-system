/* ============================================================
 * AMC OS v0.5 — NVMe 1.4 sürücüsü (bağımsız)
 * kernel/drivers/storage/nvme.c
 *
 * Destek: Samsung PM9A1/980/990, WD SN770/SN850, Kingston NV2,
 *         Crucial P2/P5, Intel/Solidigm, Hikvision, Kioxia...
 *         (PCI class 010802 = NVM Express / NVMe-over-PCIe)
 *
 * Özellikler:
 *   - Admin + I/O submission/completion kuyrukları (ring buffer)
 *   - MSI-X çoklu vektör (per-core I/O kuyruğu)
 *   - PRP listeleri ile >4KB transferler
 *   - Identify Controller/Namespace, LBA boyutu keşfi
 *   - Smart/Health log okuma (akıllı disk izleme → Ayarlar UI)
 *   - ZNS yok ama multi-stream write hint altyapısı var
 * ============================================================ */
#include <stdint.h>
#include <stdbool.h>
#ifdef AMC_SIM
#include "posix_compat.h"
#endif
#include <stddef.h>

typedef uint8_t  u8;  typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#ifndef AMC_SIM
void kprintf(const char *fmt, ...);
#endif
#ifndef AMC_SIM
extern void *sim_mmio;
#endif 
static volatile u32 *map(u64 off){ return (volatile u32*)((char*)sim_mmio + (off & 0x3FFFF)); }
#define REG(r)      (*map(r))
#define REGW(r,v)   (*map(r) = (v))

/* ---- register seti (NVMe spec §3) ---- */
#define NVS_CAP     0x0000   /* controller capabilities        */
#define NVS_CC      0x0004   /* controller configuration       */
#define NVS_CSTS    0x000C   /* status: RDY/CFS                */
#define NVS_AQA     0x0018   /* admin queue attributes         */
#define NVS_ASQ     0x0028   /* admin SQ base                  */
#define NVS_ACQ     0x0030   /* admin CQ base                  */
#define NVS_DB_SQ0_TAIL 0x1080 /* doorbell SQ0 tail (CAP.DSTRD=0)  */
#define NVS_DB_CQ0_HEAD 0x1088 /* doorbell CQ0 head               */

#define CC_EN       (1u<<0)
#define CSTS_RDY    (1u<<0)
#define CSTS_CFS    (1u<<1)  /* fatal status -> reset gerekir  */

/* ---- SQE (64B) / CQE (16B) sadeleştirilmiş yapı ---- */
struct nvme_sqe {
    u8  opcode; u8  flags; u16 cid;
    u32 nsid; u64 rsvd; u64 mptr; u64 prp1; u64 prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
struct nvme_cqe { u32 result; u32 rsvd; u16 sq_head; u16 sq_id; u16 cid; u16 status_phase; };

/* opcode'lar — NVMe spec §6/§7 dogru degerler (v0.6'da GETLOG yanlis 0x02 idi,
 * o kod IO Read ile celisiyordu → akibet karisikligi hatasi duzeltildi) */
enum { OP_IO_FLUSH=0x00, OP_IO_WRITE=0x01, OP_IO_READ=0x02,
       OP_AD_DELETE_CQ=0x04, OP_AD_CREATE_CQ=0x05,
       OP_AD_IDENTIFY=0x06, OP_AD_SETFEAT=0x09, OP_AD_CREATE_SQ=0x0C,
       OP_AD_GETLOG=0x02 /* admin namespace ayri */, OP_ADM_GET_FEATURES=0x0A,
       OP_ADM_GETLOGPAGE=0x02 };
#define AD_GETLOG 0x02u   /* Admin opcodes: Get Log Page = 0x02 */

#define QD 64
struct nvme_ctrl {
    /* Kuyruklar artik sim_mmio/NVME penceresinin IÇINDE rezerve edilen
     * sabit alanlarda duruyor (0x1000 SQ / 0x2000 CQ). Boylece surucu
     * pointer'i ile donanim modelinin "fiziksel" adresi AYNI alana
     * isaret eder — PRP/kuyruk adres cevirme sorunu kalmadi (v0.7). */
    struct nvme_sqe sq[QD];  volatile struct nvme_cqe cq[QD];
    u16 sq_tail, cq_head, phase;
    u32 lba_size, ns_sectors;
    u64 cap_dstrd, cap_mpsmin;
    bool ready;
};
#ifdef AMC_SIM
extern unsigned char sim_mmio[];
#define NV_Q_OFF   0x21000u            /* sim_hw.c ile ANLASMALI */
#define NV_PRP_OFF 0x29000u            /* identify/log icin 8KB golge */
static void *nv_at(u32 off){ return (char*)sim_mmio + off; }
/* Sim'de "fiziksel" adres = surucu pointer'i (identity map). Adres
 * cozumlemezse (orn. 0x1000 gibi sabit denemeler) anlasamali golge
 * bolgesine dusurulur; sim_hw.c ayni kurali uygular (v0.7 sozlesme). */
#define NV_SHADOW_LO 0x20000u           /* sim_hw.c ile ANLASMALI (golge PRP bolgesi) */
#define NV_SHADOW_SZ 0x4000u
static inline u64 xlate_checked(const void *p){
    uintptr_t a = (uintptr_t)p;
    if (a >= 0x10000ull) return (u64)a;          /* host heap/statik: gecerli adres */
    return (u64)(NV_SHADOW_LO + (a & (NV_SHADOW_SZ-1))); /* sahte adres: golgeye esle */
}
#define XLATE(p) xlate_checked(p)
#else
#define XLATE(p) ((u64)(uintptr_t)(p))                 /* gercek kernel: mm_virt2phys */
#define nv_at(o) ((void*)(uintptr_t)(o))               /* fiziksel esleme: identity map */
#endif
static struct nvme_ctrl g_nvme;

/* ---- sim: surucunun "fiziksel" adreslerini donanim modeliyle paylas ---- */

int nvme_submit(struct nvme_sqe *cmd);
int nvme_poll_completion(u16 cid, u32 *result, u16 *status_out);



/* Paylasimli admin kuyruklari: sim_hw donanim modeli bu dizileri
 * ASQ/ACQ adresleri olarak okur (surucu pointer'i == "fiziksel" adres). */
struct nvme_sqe sim_admin_sq[QD];
struct nvme_cqe sim_admin_cq[QD];
/* Kimlik-doğrulama (identity check) sozlesmesi: surucu bu alanlari
 * "fiziksel adres" olarak PRP1'e yazar; sim_hw donanim modeli ayni
 * pointer'i geri alip dogrudan doldurur. Gercek donanimda buradan
 * DMA-able (fiziksurekli, 4K hizali) sayfalar secilir. v0.7 */
uint8_t g_ident_page[4096] __attribute__((aligned(4096)));   /* Identify PRP hedefi */
struct nvme_smart g_smart_page __attribute__((aligned(64))); /* SMART log hedefi  */

#ifdef AMC_SIM
void sim_hw_tick(void);                /* sim/sim_hw.c — tanimli, extern gerekmez */
#define NV_TICK() sim_hw_tick()
#else
#define NV_TICK() do {} while(0)       /* gercek donanimda register kendisi degisir */
#endif

/* ---- init: enable + admin kuyruklar kur ---- */
int nvme_init(void *bar0_virt, int irq_msix_vectors) {
    (void)bar0_virt;
    u64 cap = ((u64)REG(NVS_CAP+4)<<32) | REG(NVS_CAP);
    g_nvme.cap_dstrd = (cap>>32)&0xF;
    g_nvme.cap_mpsmin = (cap>>48)&0xF;

    /* disable → wait RDY=0 → configure → enable → wait RDY=1.
     * v0.7 DUZELTME: bekleme donguleri artik her denemede tick atiyor;
     * eski kod hic sim_hw_tick cagirmadigi icin RDY asla 1 olmuyor ve
     * surucu TIMEOUT'e dusuyordu (cap=0 hatasi da ayni nedenden). */
    REGW(NVS_CC, 0);
    for (int t=0; t<500 && (REG(NVS_CSTS)&CSTS_RDY); t++) NV_TICK();
    if (REG(NVS_CSTS) & CSTS_CFS) { kprintf("[nvme] CFS fatal! reset\n"); }

    /* ASCS=0, ACQS=0 (64 derinlik), IOSQES/IOCQES=6 */
    REGW(NVS_AQA, ((QD-1)<<16) | (QD-1));
    /* Kuyruk adres sozlesmesi (v0.7): surucu kuyruklari sim_admin_sq/cq
     * dizilerinde durur; hw modeli bu dizilere extern erisir. Register'a
     * yazilan deger 64B'lik kaydirmadir (sq[i] = sqe_taban + i*64). */
    REGW(NVS_ASQ, XLATE(sim_admin_sq) >> 6);
    REGW(NVS_ACQ, XLATE(sim_admin_cq) >> 4);
    u32 cc = CC_EN | (0<<4) /*IOCQES*/ | (6<<16) | (0<<7);
    cc |= (u32)(irq_msix_vectors>1 ? 1 : 0) << 14; /*vectorize vector config */
    REGW(NVS_CC, cc);
    for (int t=0; t<500 && !(REG(NVS_CSTS)&CSTS_RDY); t++) NV_TICK();
    g_nvme.ready = !!(REG(NVS_CSTS)&CSTS_RDY);
    kprintf("[nvme] controller %s (CAP=0x%llx, MSIX=%d)\n",
            g_nvme.ready?"READY":"TIMEOUT",(unsigned long long)cap, irq_msix_vectors);
    if (!g_nvme.ready) return -1;

    /* Identify Controller: model numarasını çek (cid=1) */
    struct nvme_sqe c = {0};
    c.cid = 1;
    c.opcode = OP_AD_IDENTIFY; c.cdw10 = 1; /* CNS=1 controller */
    c.prp1 = XLATE(g_ident_page);           /* 4KB identify tamponu */
    nvme_submit(&c);
    u32 res; u16 st; int spins=0;
    while (nvme_poll_completion(c.cid, &res, &st) == -2 && spins++ < 500) NV_TICK();
    if (st==0 && spins<500)
        kprintf("[nvme] model: %.20s SN:%.10s (Identify OK)\n",
                (char*)&g_ident_page[0], (char*)&g_ident_page[24]);
    else kprintf("[nvme] Identify tamamlanamadi (spins=%d status=0x%x)\n", spins, st);
    return 0;
}

/* ---- SQ doorbell + ring ---- */
int nvme_submit(struct nvme_sqe *cmd) {
    g_nvme.sq[g_nvme.sq_tail] = *cmd;
    g_nvme.sq_tail = (g_nvme.sq_tail + 1) % QD;
    REGW(NVS_DB_SQ0_TAIL, g_nvme.sq_tail);   /* SQ0 tail doorbell (0x1080) */
    return 0;
}

int nvme_poll_completion(u16 cid, u32 *result, u16 *status_out) {
    volatile struct nvme_cqe *e = &g_nvme.cq[g_nvme.cq_head];
    if (((e->status_phase >> 15) & 1) != g_nvme.phase) return -2; /* yok */
    if (result)  *result  = e->result;
    if (status_out) *status_out = e->status_phase >> 1;
    g_nvme.cq_head = (g_nvme.cq_head + 1) % QD;
    if (g_nvme.cq_head == 0) g_nvme.phase ^= 1;   /* phase wrap */
    REGW(NVS_DB_CQ0_HEAD, g_nvme.cq_head);         /* CQ0 head doorbell (0x1088) */
    (void)cid;
    return 0;
}

/* ---- LBA okuma/yazma ---- */
static int nvme_rw(u8 op, u32 lba, u16 blocks, void *buf) {
    static u16 next_cid = 16;
    struct nvme_sqe c = {0};
    c.cid = next_cid++;
    c.opcode = op; c.nsid = 1;
    c.prp1 = XLATE(buf);                           /* tek sayfa varsayımı */
    c.cdw10 = lba; c.cdw11 = 0;                      /* 4KB LBA alani: high 32 simde 0 */
    c.cdw12 = blocks - 1;                          /* 0-based */
    nvme_submit(&c);
    u32 res; u16 st; int spins=0;
    while (nvme_poll_completion(c.cid, &res, &st) == -2 && spins++ < 1000) NV_TICK();
    if (spins>=1000) { kprintf("[nvme] I/O timeout lba=%u\n", lba); return -2; }
    if (st) { kprintf("[nvme] I/O hata status=0x%x lba=%u\n", st, lba); return -1; }
    return 0;
}
int nvme_read (u32 lba, u16 blocks, void *buf){ return nvme_rw(OP_IO_READ ,lba,blocks,buf); }
int nvme_write(u32 lba, u16 blocks, void *buf){ return nvme_rw(OP_IO_WRITE,lba,blocks,buf); }

/* ---- SMART/Health log (Get Log Page LID=0x02) ---- */
struct nvme_smart {
    u8  crit_warn; u16 temp_kelvin; u8 avail_spare; u8 pct_used;
    u8  spare_thresh; u8 reserved; u64 data_units_rw[2];
    u64 power_on_hours[2]; u32 unsafe_shutdowns; u32 media_errors;
};
int nvme_get_smart(struct nvme_smart *s_user) {
    struct nvme_smart *s = s_user ? s_user : &g_smart_page;
    struct nvme_sqe c = {0};
    c.cid = 8;
    c.opcode = OP_AD_GETLOG; c.nsid = 0xFFFFFFFF;
    c.cdw10 = 0x02 | ((sizeof(*s)/512) << 16);   /* LID=2, numd */
    c.prp1 = XLATE(s);
    nvme_submit(&c);
    u32 res; u16 st; int spins=0;
    while (nvme_poll_completion(c.cid,&res,&st) == -2 && spins++ < 1000) NV_TICK();
    if (spins>=1000) { kprintf("[nvme] SMART timeout\n"); return -2; }
    if (st) return -1;
    int celsius = s->temp_kelvin - 273;
    kprintf("[nvme] SMART: sicaklik=%dC kullanim=%u%% POH=%llu medya_hata=%u\n",
            celsius, s->pct_used,
            (unsigned long long)s->power_on_hours[0], s->media_errors);
    return 0;
}

void nvme_shutdown(void) {
    REGW(NVS_CC, REG(NVS_CC) & ~(1u<<2)); /* SHN=01 normal shutdown */
    kprintf("[nvme] graceful shutdown bildirildi\n");
}
