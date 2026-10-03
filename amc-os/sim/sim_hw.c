/* ============================================================
 * AMC OS v0.5 — Sanal donanım (simülasyon cihaz yığını)
 * sim/sim_hw.c
 *
 * QEMU'da görünecek gerçekçi bir PC yapılandırmasını bellekte
 * taklit eder:
 *   00:14.0 Intel HDA bus controller + Realtek ALC892 codec
 *   00:1f.3 Intel i219-V? hayır → Realtek RTL8168/8111 GbE
 *   00:14.x xHCI (USB3, 4 port: 2x SS, 2x HS) + EHCI (USB2)
 *   01:00.0 Intel UHD 770 (i915-style MMIO)
 *   02:00.0 AMD RDNA2 (amdgpu-style MMIO)
 *   03:00.0 Samsung NVMe SSD
 *   04:00.0 Realtek RTL8822CE WiFi
 * Register semantiği minimum ama DOĞRU: verb bitirme biti,
 * portsc connect bits, CAPLE/STS handshake, MSI enable...
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "posix_compat.h"

typedef uint8_t  u8;  typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

uint8_t sim_iop[0x10000];
uint8_t sim_mmio[0x40000];

/* ---------------- PCI konfig alanı ---------------- */
#define MAXFN ((32*32*8)) /* bus*dev*fn tam yuva tablosu */
struct pcidev {
    int used; uint16_t vid, did; uint32_t classrev;
    uint32_t bar[6]; uint32_t size[6];   /* BAR boyutu (size probing) */
    uint16_t subsys_vid, subsys_id;
    uint8_t  capptr;                     /* yetenek listesi başı */
    uint16_t msi_off; uint8_t msi_en;
    uint32_t cmd, status;
};
static struct pcidev devs[MAXFN];
static int slot(uint16_t b,uint16_t d,uint16_t f){ return (b*32+d)*8+f; }

static void add_dev(uint16_t b,uint16_t d,uint16_t f,uint16_t vid,uint16_t did,
                    uint32_t cls,const char *name){
    int i=slot(b,d,f); struct pcidev *p=&devs[i];
    p->used=1; p->vid=vid; p->did=did; p->classrev=cls;
    p->status=0x0010; /* capabilities list */
    printf("  [sim-hw] %-28s %02x:%02x.%d  %04x:%04x\n", name,b,d,f,vid,did);
}

uint32_t sim_pci_read(uint16_t b,uint16_t d,uint16_t f,uint16_t o){
    struct pcidev *p=&devs[slot(b,d,f)];
    if(!p->used){ if(o==0) return 0xFFFFFFFF; return 0; }
    switch(o){
    case 0x00: return (uint32_t)p->vid | ((uint32_t)p->did<<16);
    case 0x04: return p->cmd;
    case 0x06: return (p->status<<16)|((o&3)?0:0); /* status */
    case 0x08: return p->classrev;
    case 0x34: return p->capptr;
    case 0x2c: return (uint32_t)p->subsys_vid|((uint32_t)p->subsys_id<<16);
    default:
        if(o>=0x10 && o<0x28){ int i=(o-0x10)/4;
            /* BAR sizing: yazılan maskeye göre 0 döner */
            if(p->size[i]) return p->bar[i];
            return p->bar[i];
        }
        return 0;
    }
}
void sim_pci_write(uint16_t b,uint16_t d,uint16_t f,uint16_t o,uint32_t v){
    struct pcidev *p=&devs[slot(b,d,f)];
    if(!p->used) return;
    if(o==0x04) p->cmd=v;
    else if(o>=0x10&&o<0x28){ int i=(o-0x10)/4;
        /* sizing probe: en düşük bitleri sıfırlayan yazım */
        if(v==0 && p->size[i]) p->bar[i]=~(p->size[i]-1)+1; /* mask */
        else p->bar[i]=v;
    }
    else if(p->msi_off && o>=p->msi_off && o<p->msi_off+12){
        if(o==p->msi_off+2) p->msi_en = (v>>16)&1; /* message control */
    }
}

/* ---------------- HDA codec simülasyonu ----------------
 * Verb işlenir: GET_PARAMETER(0x0f), SET_PARAMETER(0x1c)...
 * Response: bit31=0 (verb cevapları için) + data. */
static uint32_t hda_codec_resp(uint32_t verb){
    uint16_t nid = (verb>>16)&0x7f;
    uint8_t  cmd = verb & 0xff;
    uint32_t idx = (verb>>8)&0xff;
    switch(cmd){
    case 0x0f: /* GET_PARAMETER */
        if(nid==0x00){ /* function group: vendor id */
            if(idx==0x00) return 0x10ec0269; /* REALTEK ALC269 ailesi */
            if(idx==0x02) return 0x00010001; /* revision */
            if(idx==0x04) return 0x00000000; /* subvendor */
        }
        if(nid==0x01) return 0x00000018;     /* audio output widget */
        if(nid==0x02) return 0x400000f0;     /* pin: HP out, present */
        if(nid==0x03) return 0x00000001;     /* input amp left */
        if(nid==0x04) return 0x011701f0;     /* vendor widget: ALC269 rev1 */
        return 0x00000000;
    case 0x00: /* NODE_ID */      return ((uint32_t)nid<<16)|0x00900000;
    case 0x05: /* WIDGET_CAP */   return 0x00000000;
    case 0x0d: /* GET_CONN_LIST */return 0x00010200; /* 1 conn: node 2 */
    case 0x17: /* PIN_SENSE */    return 0x00000001; /* jack plugged */
    case 0x18: /* CONNECT_SELECT*/return 0;
    default: return 0x40000000; /* ok, data=0 */
    }
}

/* CORB/RIRB motoru: HDA sanal MMIO'sunda basit state machine */
#define HDA_BASE 0x1000
static void hda_poll(void){
    uint32_t corb_wp = mmio_read32((void*)(uintptr_t)(HDA_BASE+0x48));
    uint32_t corb_rp = mmio_read32((void*)(uintptr_t)(HDA_BASE+0x4c));
    if(corb_wp!=corb_rp){
        uint32_t verb = mmio_read32((void*)(uintptr_t)(HDA_BASE+0x50+(corb_rp%8)*4));
        uint32_t resp = hda_codec_resp(verb);
        uint32_t rirb_wp = mmio_read32((void*)(uintptr_t)(HDA_BASE+0x68));
        mmio_write32((void*)(uintptr_t)(HDA_BASE+0x70+(rirb_wp%8)*4), resp);
        mmio_write32((void*)(uintptr_t)(HDA_BASE+0x68), rirb_wp+1);
        /* RIRB status: valid bit */
        mmio_write32((void*)(uintptr_t)(HDA_BASE+0x64), 1);
        mmio_write32((void*)(uintptr_t)(HDA_BASE+0x4c), corb_rp+1);
    }
}

/* ---------------- xHCI port simülasyonu ----------------
 * PortSC'lerde gerçek bağlantı durumunu tutar; sürücü reset
 * bitini (PR) çektiğinde hiz secimi tamamlanmış gibi görünür. */
#define XHCI_BASE 0x4000
static const uint32_t xhci_ports[] = {
    0x02A100E1, /* port0: USB3 SuperSpeed connected+enabled */
    0x02A100E0, /* port1: USB3 connected */
    0x04200000, /* port2: USB2 high-speed connected */
    0x002000A0, /* port3: empty, powered */
};
static void xhci_init_regs(void){
    mmio_write32((void*)(uintptr_t)(XHCI_BASE+0x00),0x01000001); /* CAPLENGTH=0x10? low byte */
    mmio_write32((void*)(uintptr_t)(XHCI_BASE+0x04),0x00010000); /* HCIVERSION 1.0 */
    uint32_t hcs = 4 | (0<<4) | (1<<8) | (0<<12) | (1<<16) | (0<<24);
    mmio_write32((void*)(uintptr_t)(XHCI_BASE+0x08),hcs);  /* maxports=4 */
    for(int i=0;i<4;i++)
        mmio_write32((void*)(uintptr_t)(XHCI_BASE+0x200+i*0x10),xhci_ports[i]);
}
static void xhci_poll(void){
    for(int i=0;i<4;i++){
        uintptr_t a=XHCI_BASE+0x200+i*0x10;
        uint32_t sc=mmio_read32((void*)a);
        if(sc & (1u<<4)){          /* PORT_RESET çekildi -> tamamlandı */
            sc &= ~(3u<<4);        /* clear PR + PLS */
            sc |= (1u<<5)|(1u<<2); /* reset done + enabled */
            mmio_write32((void*)a,sc);
        }
        if(sc & (1u<<9)){          /* warm reset */
            sc &= ~(1u<<9); sc |= (1u<<10);
            mmio_write32((void*)a,sc);
        }
    }
}


/* ---------------- NVMe 1.4 sanal denetleyici (Samsung PM9A1) ----------------
 * v0.7 ADRES ANLASMASI (surucu kernel/drivers/storage/nvme.c ile birebir):
 *   surucu REG(r) -> map(r) -> sim_mmio[(r & 0x3FFFF)] olarak okur/yazar.
 *   Bu yuzden donanim modeli de AYNI kaydirma ile erisir:
 *      register  r  =>  sim_mmio + (r & 0x3FFFF)
 *   Admin kuyruklari surucunun kendi statik yapisiyla PAYLASILIR:
 *      ASQ fiziksel adresi 0x1000 -> g_nvme.sq[] ile ayni alan
 *      ACQ fiziksel adresi 0x2000 -> g_nvme.cq[] ile ayni alan
 *   PRP1 adresleri de surucunun buffer'lari (identify sayfasi / SMART
 *   struct'i) oldugu icin dogrudan kullanilir — golge/kaydirma yok.
 */
#define NVME_REG_BASE 0u                       /* kaydirmasiz: surucu ile ayni harita */
extern unsigned char sim_mmio[];               /* posix_compat tanimi */

/* surucunun kuyruk alanlariyla PAYLASILIR tamponlar: sim_main.c'de
 * g_nvme.sq/cq bu dizilere yerlestirilir (ayni adres, ayni layout) —
 * boylece ASQ/ACQ "fiziksel" adresleri ile surucu pointer'lari celissiz. */
struct nvme_sqe_sim { u8 opcode; u8 flags; u16 cid; u32 nsid; u64 rsvd; u64 mptr;
                      u64 prp1; u64 prp2; u32 cdw10,cdw11,cdw12,cdw13,cdw14,cdw15; };
struct nvme_cqe_sim { u32 result; u32 rsvd; u16 sq_head; u16 sq_id; u16 cid; u16 status_phase; };
extern struct nvme_sqe_sim sim_admin_sq[64];   /* tanim: sim_main.c */
extern struct nvme_cqe_sim sim_admin_cq[64];
#define g_nv_sq sim_admin_sq                   /* ASQ=0x1000 -> bu dizi */
#define g_nv_cq sim_admin_cq                   /* ACQ=0x2000 -> bu dizi */

static void sim_nvme_reset(void){
    uint8_t *r = sim_mmio + NVME_REG_BASE;
    memset(r, 0, 0x2000);                     /* bolgeyi temizle (CC/CSTS/doorbell dahil) */
    uint64_t cap = (31ull<<52)|(1ull<<48)|(15ull<<32)|(3ull<<24)|(0ull<<16)|(255ull<<0);
    memcpy(r+0x00,&cap,8);                    /* CAP: TO=255,CQR=1,MPSMIN=0,NSSRS=1,SQES/CQES=6 */
    *(uint32_t*)(r+0x04) |= (1u<<4);          /* CAP.CSS: NVM command set bit */
    *(uint32_t*)(r+0x14) = 0x007f00ff;        /* VS 1.4.0 */
    *(uint32_t*)(r+0x1C) = 0;                 /* INTMS */
    /* surucu CC.EN oncesi AQA/ASQ/ACQ yazar; sim varsayilanlari da koyar */
    *(uint32_t*)(r+0x28) = 0x1000;            /* ASQ varsayilan */
    *(uint32_t*)(r+0x2C) = 0x2000;            /* ACQ varsayilan */
    *(uint32_t*)(r+0x18) = (63u<<16)|63u;     /* AQA: 64 derinlik */
    /* CC=0 → CSTS.RDY=0; surucu ASENKRON ACILMA modelini uygular:
       CC.EN yazilinca bir sonraki tickte RDY=1 olur (spec §3.1.4). */
}
#define NV_R(o) (*(volatile uint32_t*)(sim_mmio + NVME_REG_BASE + ((o) & 0x3FFFFu)))
#define NV_CC    NV_R(0x04)
#define NV_CSTS  NV_R(0x0C)
#define NV_AQA   NV_R(0x18)
#define NV_ASQ   NV_R(0x28)
#define NV_ACQ   NV_R(0x2C)
#define NV_DB_TAIL NV_R(0x1080)              /* SQ0 tail doorbell (surucu ile ayni sozlesme) */
static uint16_t g_nv_sq_seen = 0;   /* islenen admin SQ tail */
static uint16_t g_nv_cq_tail = 0;   /* uretilen CQE ring pozisyonu */
static uint16_t g_nv_phase   = 1;   /* ilk tur phase=1 (spec: CQE phase bit) */
static int      g_nvme_sim   = 0;   /* surucunun "fiziksel" PRP adreslerini sim_mmio'ya kaydirma bayragi */

/* Yardimci: PRP1'i surucunun paylasimli alanina cevirir. Sim'de
 * "fiziksel" adres = surucu pointer'i (identity map); kuyruk adresleri
 * (0x1000/0x2000) g_nv_sq/g_nv_cq dizilerine, surucu buffer pointer'lari
 * dogrudan kendilerine isaret eder.  */
/* surucuya ait anlasamali tamponlar (nvme.c'te tanimli) */
extern uint8_t g_ident_page[4096];
struct sim_smart { uint8_t crit_warn; uint16_t temp_kelvin; uint8_t avail_spare;
                   uint8_t pct_used; uint8_t spare_thresh; uint8_t reserved;
                   uint64_t data_units_rw[2]; uint64_t power_on_hours[2];
                   uint32_t unsafe_shutdowns; uint32_t media_errors; };
extern struct sim_smart g_smart_page;

/* Kuyruk adres sozlesmesi: surucu kuyruklari su statik dizilerde durur
 * (sim_main.c bunlari nvme.c'nin g_nvme.yapisindan erisilebilir yapar). */
extern struct nvme_sqe_sim sim_admin_sq[64];
extern struct nvme_cqe_sim sim_admin_cq[64];
/* ASQ/ACQ degeri 64B/16B kaydirmadir (surucu >>6 / >>4 yazar). */
static struct nvme_sqe_sim *nv_sq_at(u64 asq_scaled, u32 idx){
    u64 base = asq_scaled ? (asq_scaled << 6) : (u64)(uintptr_t)sim_admin_sq;
    if (base == (u64)(uintptr_t)sim_admin_sq) return &sim_admin_sq[idx];
    return (struct nvme_sqe_sim*)(uintptr_t)base + idx;
}
static struct nvme_cqe_sim *nv_cq_at(u64 acq_scaled, u32 idx){
    u64 base = acq_scaled ? (acq_scaled << 4) : (u64)(uintptr_t)sim_admin_cq;
    if (base == (u64)(uintptr_t)sim_admin_cq) return &sim_admin_cq[idx];
    return (struct nvme_cqe_sim*)(uintptr_t)base + idx;
}

static void *nv_prp_ptr(uint64_t prp1){
    /* kimlik dogrulamali adresler: surucunun kendi tamponlari */
    if (prp1 == (uint64_t)(uintptr_t)g_ident_page)  return g_ident_page;
    if (prp1 == (uint64_t)(uintptr_t)&g_smart_page) return &g_smart_page;
    if (prp1 >= 0x10000ull)                          /* host pointer'i: dogrudan */
        return (void*)(uintptr_t)prp1;
    /* sahte/kucuk adres: surucunun xlate_checked kuraliyla AYNI esleme */
    return sim_mmio + 0x20000u + (uint32_t)(prp1 & 0x3FFFu);
}

static void nvme_poll(void){
    /* acilma/kapanma semantigi (NVMe spec 3.1.4) — sim senkron varyanti:
       CC.EN=1 → RDY=1; CC.EN=0 ya da SHN!=0 → RDY=0 + kuyruk sifirla */
    uint8_t shn = (uint8_t)((NV_CC >> 10) & 3u);
    if((NV_CC & 1u) && !shn){ NV_CSTS |= 1u; }
    else if (NV_CSTS & 1u) { NV_CSTS &= ~1u; g_nv_sq_seen=g_nv_cq_tail=0; g_nv_phase=1; }
    if(!(NV_CC & 1u)) return;

    uint16_t depth = (uint16_t)((NV_AQA & 0xFFFF) + 1);
    uint16_t tail  = (uint16_t)NV_DB_TAIL;
    while (g_nv_sq_seen != tail) {
        struct nvme_sqe_sim *sqe = nv_sq_at((u64)NV_ASQ, g_nv_sq_seen);
        uint8_t  opc   = sqe->opcode;
        uint16_t cid   = sqe->cid;
        uint64_t prp1  = sqe->prp1;
        uint32_t cdw10 = sqe->cdw10;
        uint32_t result = 0; uint16_t status = 0;

        if (opc == 0x06) {                       /* Identify */
            uint8_t *dst = (uint8_t*)nv_prp_ptr(prp1);
            memset(dst, 0, 4096);
            if (cdw10 == 1) {                    /* CNS=1 controller */
                memcpy(dst, "Macaw SSD PM9A1 1TB", 20);   /* MN@0 */
                memcpy(dst+24, "AMCSIM0001", 10);         /* SN@24 */
                *(uint16_t*)(dst+512) = 1;                /* NN namespace sayisi */
                result = 0;
            } else if (cdw10 == 0) {             /* CNS=0 namespace */
                uint64_t nsze = 234441984ull;             /* ~1TB @4KB LBA */
                memcpy(dst,     &nsze, 8);                /* NSZE */
                memcpy(dst+8,   &nsze, 8);                /* NCAP */
                memcpy(dst+16,  &nsze, 8);                /* NUSE */
                dst[28] = 0;                              /* FLBAS: LBAF0 aktif (4KB) */
                result = 1;
            }
        } else if (opc == 0x02 && (cdw10 & 0xFF) == 0x02) { /* Admin Get Log Page: SMART */
            /* NVMe spec layout'u surucunun struct'ina birebir cevirerek yaz */
            struct sim_smart *sm = (struct sim_smart*)nv_prp_ptr(prp1);
            memset(sm, 0, sizeof *sm);
            sm->crit_warn = 0;
            sm->temp_kelvin = 38 + 273;                   /* 38C */
            sm->avail_spare = 97; sm->pct_used = 4;
            sm->data_units_rw[0] = 0x4e20;                /* ~100 TB yazilan */
            sm->power_on_hours[0] = 812;
            sm->unsafe_shutdowns = 2; sm->media_errors = 0;
        } else if (opc == 0x00 || opc == 0x01 || opc == 0x02 ||
                   opc == 0x05 || opc == 0x09 || opc == 0x0a || opc == 0x0c) {
            /* Flush / IO Read / IO Write / Create-CQ / Set-Get Features / Create-SQ:
               sim RAM-disk oldugu icin no-op basari */
        } else { status = 0; }

        struct nvme_cqe_sim *cqe = nv_cq_at((u64)NV_ACQ, g_nv_cq_tail);
        cqe->result       = result;
        cqe->sq_head      = g_nv_sq_seen;
        cqe->sq_id        = 0;
        cqe->cid          = cid;
        cqe->status_phase = (uint16_t)((status << 1) | g_nv_phase);
        g_nv_cq_tail = (uint16_t)((g_nv_cq_tail + 1) % depth);
        if (g_nv_cq_tail == 0) g_nv_phase ^= 1;
        g_nv_sq_seen = (uint16_t)((g_nv_sq_seen + 1) % depth);
    }
}

/* ---------------- kurulum / bakım ---------------- */
void sim_hw_init(void){
    printf("[sim-hw] sanal anakart yukleniyor:\n");
    /* classrev = class<<16|progif<<8|rev */
    add_dev(0,0x1f,3, 0x8086,0xa1c8,0x0403000a,"Intel HDA bus ctrl (PCH)");
    devs[slot(0,0x1f,3)].bar[0]=0xfed00000|2; devs[slot(0,0x1f,3)].size[0]=0x4000;
    devs[slot(0,0x1f,3)].capptr=0x50;
    add_dev(0,0x1f,6, 0x10ec,0x8168,0x02000006,"Realtek RTL8168 GbE");
    devs[slot(0,0x1f,6)].bar[0]=0xfeb00000|1;  devs[slot(0,0x1f,6)].size[0]=0x256;
    devs[slot(0,0x1f,6)].bar[2]=0xfeb01000|2;  devs[slot(0,0x1f,6)].size[2]=0x4000;
    add_dev(0,0x14,0, 0x8086,0x1e31,0x0c033003,"Intel Panther xHCI (USB3)");
    devs[slot(0,0x14,0)].bar[0]=0xfec00000|2; devs[slot(0,0x14,0)].size[0]=0x10000;
    add_dev(0,0x1d,7, 0x8086,0x8c2d,0x0c032000,"Intel EHCI (USB2)");
    devs[slot(0,0x1d,7)].bar[0]=0xfed40000|2; devs[slot(0,0x1d,7)].size[0]=0x400;
    add_dev(1,0,0,    0x8086,0x4680,0x0300000c,"Intel UHD 770 (GPU-A)");
    devs[slot(1,0,0)].bar[0]=0xc0000000|2; devs[slot(1,0,0)].size[0]=0x4000000;
    devs[slot(1,0,0)].bar[2]=0xf8000000|2; devs[slot(1,0,0)].size[2]=0x200000;
    add_dev(2,0,0,    0x1002,0x73bf,0x03000000,"AMD RX 6800 RDNA2 (GPU-B)");
    devs[slot(2,0,0)].bar[0]=0xd0000000|2; devs[slot(2,0,0)].size[0]=0x2000000;
    add_dev(3,0,0,    0x144d,0xa808,0x01080202,"Samsung PM9A1 NVMe");
    devs[slot(3,0,0)].bar[0]=0xfa000000|2; devs[slot(3,0,0)].size[0]=0x4000;
    add_dev(4,0,0,    0x10ec,0x8822,0x02800000,"Realtek RTL8822CE WiFi");
    devs[slot(4,0,0)].bar[0]=0xfb000000|2; devs[slot(4,0,0)].size[0]=0x100000;
    xhci_init_regs();
    sim_nvme_reset();
    /* HGA GCAP: 2 output streams, 4 input, corr/bist supported */
    mmio_write32((void*)(uintptr_t)(HDA_BASE+0x00), (2u<<16)|(4u<<8)|0x03);
    printf("[sim-hw] hazir. (codec: Realtek ALC269 ailesi)\n");
}

/* Zamanlayıcı bakımı: ana sim döngüsü her tikte çağırır */
void sim_hw_tick(void){
    hda_poll();
    xhci_poll();
    nvme_poll();
    /* RTL8169 TX: FIFO boş simülasyonu (TSAd bitini set et) */
    uint32_t txde = mmio_read32((void*)(uintptr_t)0x2000);
    if(txde & 0x80000000u) mmio_write32((void*)(uintptr_t)0x2000, (txde & ~0x80000000u) | 0x40000000u);
}
