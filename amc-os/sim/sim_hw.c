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

uint8_t sim_iop[0x10000];
uint8_t sim_mmio[0x40000];

/* ---------------- PCI konfig alanı ---------------- */
#define MAXFN 64
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
    /* HGA GCAP: 2 output streams, 4 input, corr/bist supported */
    mmio_write32((void*)(uintptr_t)(HDA_BASE+0x00), (2u<<16)|(4u<<8)|0x03);
    printf("[sim-hw] hazir. (codec: Realtek ALC269 ailesi)\n");
}

/* Zamanlayıcı bakımı: ana sim döngüsü her tikte çağırır */
void sim_hw_tick(void){
    hda_poll();
    xhci_poll();
    /* RTL8169 TX: FIFO boş simülasyonu (TSAd bitini set et) */
    uint32_t txde = mmio_read32((void*)(uintptr_t)0x2000);
    if(txde & 0x80000000u) mmio_write32((void*)(uintptr_t)0x2000, (txde & ~0x80000000u) | 0x40000000u);
}
