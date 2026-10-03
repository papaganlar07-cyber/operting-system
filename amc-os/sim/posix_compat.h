/* ============================================================
 * AMC OS v0.5 — Simülasyon katmanı
 * sim/posix_compat.h
 *
 * Çekirdek/donanım kodunu GERÇEK'TE derlenir ve çalışır kılar.
 * Freestanding kernel kodu "sanal makine" üzerinde (POSIX host)
 * koşar:
 *   - inb/outb/mmio  → bellekte sahte donanım register'ları
 *   - PCI config     → simüle cihaz yığını (Realtek HDA, RTL8169,
 *                      xHCI, Intel/AMD GPU, NVMe)
 *   - IRQ/timer      → gerçek zamanlı tik üreteci
 * Böylece Realtek verb motoru, xHCI port state machine, GPU
 * scheduler ve CFS zamanlayıcı gerçekten test edilebilir.
 * ============================================================ */
#ifndef AMC_POSIX_COMPAT_H
#define AMC_POSIX_COMPAT_H

#ifdef AMC_SIM
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- sanal I/O port uzayı (64KB) + MMIO region (256KB) ---- */
extern uint8_t sim_iop[0x10000];
extern uint8_t sim_mmio[0x40000];

static inline uint8_t  inb(uint16_t p)  { return sim_iop[p]; }
static inline void     outb(uint16_t p, uint8_t v) { sim_iop[p] = v; }
static inline uint16_t inw(uint16_t p)  { return (uint16_t)(sim_iop[p] | (sim_iop[p+1] << 8)); }
static inline void     outw(uint16_t p, uint16_t v){ sim_iop[p]=(uint8_t)v; sim_iop[p+1]=(uint8_t)(v>>8); }
static inline uint32_t inl(uint16_t p)  { uint32_t v; memcpy(&v,&sim_iop[p],4); return v; }
static inline void     outl(uint16_t p, uint32_t v){ memcpy(&sim_iop[p],&v,4); }

/* ---- sanal MMIO erişimi (sürücülerin api'si) ---- */
static inline uint32_t mmio_read32(volatile void *a){
    uint32_t v; memcpy(&v,(char*)sim_mmio+(((uintptr_t)a)&0x3FFFF),4); return v; }
static inline void mmio_write32(volatile void *a, uint32_t v){
    memcpy((char*)sim_mmio+(((uintptr_t)a)&0x3FFFF),&v,4); }
static inline uint64_t mmio_read64(volatile void *a){
    uint64_t v; memcpy(&v,(char*)sim_mmio+(((uintptr_t)a)&0x3FFFF),8); return v; }
static inline void mmio_write64(volatile void *a, uint64_t v){
    memcpy((char*)sim_mmio+(((uintptr_t)a)&0x3FFFF),&v,8); }

/* ---- PCI konfigürasyon alanı (simüle cihazlar, sim_hw.c) ---- */
uint32_t sim_pci_read (uint16_t b,uint16_t d,uint16_t f,uint16_t o);
void     sim_pci_write(uint16_t b,uint16_t d,uint16_t f,uint16_t o,uint32_t v);

/* ---- kprintf -> stdout ---- */
#define kprintf printf

/* ---- panic -> temiz hata çıkışı ---- */
#define PANIC(msg) do{ printf("\nPANIC: %s\n",(msg)); exit(1);}while(0)

#endif /* AMC_SIM */
#endif /* AMC_POSIX_COMPAT_H */
