/* MacawOS kernel — Realtek RTL8168/8111/8125 Ethernet driver (legacy mode) */
#include "mc/rtl8169.h"
#include "mc/pci.h"
#include "mc/mm.h"
#include "mc/io.h"
#include "mc/vga.h"

/* register map — canonical RTL8169/RTL8168 legacy layout */
#define R_TXFIFOD   0x20     /* TDSA low (32-bit chips)                  */
#define R_RXFIFOD   0x24     /* RSA low                                  */
#define R_IMR       0x38     /* Interrupt Mask Register                  */
#define R_ISR       0x3A     /* Interrupt Status Register (W1C)          */
#define R_TCTR      0x40     /* TX config                                */
#define R_RCR       0x44     /* RX config                                */
#define R_EXTCC     0x45     /* extended chip control                    */
#define R_EAR       0x4D     /* EEPROM access                            */
#define R_MDIORW    0x64     /* MDIO read/write (legacy PHY)             */
#define R_PHYAR     0x68     /* PHY access reg (8169+)                   */
#define R_D9346CR   0x50
#define R_CONFIG1   0x51
#define R_MISCCRS   0x53
#define R_CCPAR     0x54
#define R_PINCSID   0x58
#define R_CONFIG2   0x59
#define R_CR        0xE0     /* command register                         */
#define R_TPPOLL    0xA8     /* TX poll demand                           */
#define R_TDESC_LO  0x6C     /* TDSA (64-bit pair at +4)                 */
#define R_RDESC_LO  0x74     /* RSA  (64-bit pair at +4)                 */
#define R_IMR_8125  0x3C     /* newer chips keep same mask semantics     */

static const struct { u16 dev; const char *name; } chips[] = {
    { 0x8136, "RTL8105E/RTL8136" }, { 0x8139, "RTL8139" },
    { 0x8169, "RTL8169SB" },        { 0x8168, "RTL8168" },
    { 0x4300, "RTL8111/RTL8168" },  { 0x8125, "RTL8125" },
    { 0x3000, "RTL8125B" },         { 0x8117, "RTL8117" },
    { 0x8126, "RTL8126" },          { 0x8401, "RTL8401" },
};

static rtl_nic_t g_nic;
static volatile u8 *g_mm;               /* unified access window          */
static u16 g_io_base;
static bool g_use_mmio;

static inline u8  r8(u32 off) { return g_use_mmio ? *(volatile u8 *)(g_mm + off) : inb((u16)(g_io_base + off)); }
static inline u16 r16(u32 off){ return g_use_mmio ? *(volatile u16 *)(g_mm + off) : inw((u16)(g_io_base + off)); }
static inline u32 r32(u32 off){ return g_use_mmio ? *(volatile u32 *)(g_mm + off) : infl((u16)(g_io_base + off)); }
static inline void w8(u32 off, u8 v)  { if (g_use_mmio) *(volatile u8 *)(g_mm + off) = v; else outb((u16)(g_io_base + off), v); }
static inline void w16(u32 off, u16 v){ if (g_use_mmio) *(volatile u16 *)(g_mm + off) = v; else outw((u16)(g_io_base + off), v); }
static inline void w32(u32 off, u32 v){ if (g_use_mmio) *(volatile u32 *)(g_mm + off) = v; else outl((u16)(g_io_base + off), v); mfence(); }

/* ---- descriptor rings -------------------------------------------------- */
#define NUM_TX 64
#define NUM_RX 64
struct rx_desc { u32 status; u32 buf_addr_lo; u32 buf_addr_hi; u32 reserved; } PACKED ALIGNED(256);
struct tx_desc { u32 res0; u32 buf_lo; u32 buf_hi; u16 len; u16 opts; } PACKED ALIGNED(256);

static struct rx_desc *rx_ring ALIGNED(256);
static struct tx_desc *tx_ring ALIGNED(256);
static u8  *rx_bufs[NUM_RX];
static u8  *tx_bufs[NUM_TX];
static u32 g_rx_idx, g_tx_idx;

/* ---- MDIO over PHYAR ----------------------------------------------------- */
static u16 mdio_read(u8 reg)
{
    w32(R_PHYAR, 0x10000u | ((u32)reg << 8) & 0x1FE00u | 0x80000000u);
    for (int i = 0; i < 100000; i++) {
        u32 v = r32(R_PHYAR);
        if (v & 0x80000000u) return (u16)((v >> 16) & 0xFFFF);
        CPU_RELAX();
    }
    return 0xFFFF;
}

static void mdio_write(u8 reg, u16 val)
{
    w32(R_PHYAR, 0x90000000u | (((u32)reg << 8) & 0x1FE00u) | ((u32)val & 0xFFFF));
    for (int i = 0; i < 100000; i++) {
        if (r32(R_PHYAR) & 0x80000000u) return;
        CPU_RELAX();
    }
}

int rtl_init(void)
{
    pci_dev_t list[64];
    int n = pci_enumerate(list, 64);
    const pci_dev_t *d = NULL;
    for (int i = 0; i < n; i++)
        if (list[i].vendor == 0x10EC &&
            (list[i].class == 0x0200 || list[i].class == 0x02)) { d = &list[i]; break; }
    if (!d) { kprintf("[rtl] no Realtek NIC found\n"); return -1; }

    g_nic.vendor = d->vendor; g_nic.device = d->device;
    const char *nm = "RTL8169-family";
    for (unsigned i = 0; i < ARRAY_LEN(chips); i++)
        if (chips[i].dev == d->device) { nm = chips[i].name; break; }
    strncpy(g_nic.chip, nm, sizeof(g_nic.chip) - 1);

    kprintf("[rtl] %s @ %02x:%02x.%x vendor=%#x device=%#x\n",
            g_nic.chip, d->bus, d->dev, d->func, d->vendor, d->device);

    pci_bus_master(d, true);
    /* choose BAR: prefer IO BAR (8139-style) else first MMIO BAR ≥ 256 bytes */
    g_use_mmio = false;
    for (int b = 0; b < 6; b++) {
        if (d->bars[b] && !d->bar_mmio[b]) { g_io_base = (u16)d->bars[b]; break; }
    }
    if (!g_io_base) {
        for (int b = 0; b < 6; b++) {
            if (d->bars[b] && d->bar_mmio[b] && d->bar_sizes[b] >= 0x100) {
                g_mm = PHYS_TO_VIRT(d->bars[b]); g_use_mmio = true; break;
            }
        }
    }
    if (!g_use_mmio && !g_io_base) { kprintf("[rtl] no usable BAR\n"); return -2; }
    g_nic.mmio = g_use_mmio;

    /* soft reset */
    w8(R_CR, 0x10);
    for (int i = 0; i < 1000000 && (r8(R_CR) & 0x10); i++) CPU_RELAX();
    if (r8(R_CR) & 0x10) { kprintf("[rtl] reset timeout\n"); return -3; }

    /* MAC address from IDSR0..5 */
    for (int i = 0; i < 6; i++) g_nic.mac[i] = r8(0x70 + i);   /* IDR0..IDR5 */

    /* ring memory */
    rx_ring = kzalloc(sizeof(*rx_ring) * NUM_RX);
    tx_ring = kzalloc(sizeof(*tx_ring) * NUM_TX);
    if (!rx_ring || !tx_ring) { kprintf("[rtl] OOM rings\n"); return -4; }
    for (int i = 0; i < NUM_RX; i++) {
        rx_bufs[i] = kmalloc(2048 + 16);
        if (!rx_bufs[i]) { kprintf("[rtl] OOM rxbuf\n"); return -5; }
        memset(rx_bufs[i], 0, 2048 + 16);
        rx_ring[i].buf_addr_lo = (u32)VIRT_TO_PHYS((uintptr_t)rx_bufs[i]);
        rx_ring[i].buf_addr_hi = (u32)(VIRT_TO_PHYS((uintptr_t)rx_bufs[i]) >> 32);
        rx_ring[i].status = 0x0000FFFFu;              /* OWN bit set by HW */
    }
    for (int i = 0; i < NUM_TX; i++) {
        tx_bufs[i] = kmalloc(2048 + 16);
        if (!tx_bufs[i]) { kprintf("[rtl] OOM txbuf\n"); return -6; }
        memset(tx_bufs[i], 0, 2048 + 16);
        tx_ring[i].buf_lo = (u32)VIRT_TO_PHYS((uintptr_t)tx_bufs[i]);
        tx_ring[i].buf_hi = (u32)(VIRT_TO_PHYS((uintptr_t)tx_bufs[i]) >> 32);
    }

    w32(R_TDESC_LO, (u32)VIRT_TO_PHYS((uintptr_t)tx_ring));
    w32(R_TDESC_LO + 4, (u32)(VIRT_TO_PHYS((uintptr_t)tx_ring) >> 32));
    w32(R_RDESC_LO, (u32)VIRT_TO_PHYS((uintptr_t)rx_ring));
    w32(R_RDESC_LO + 4, (u32)(VIRT_TO_PHYS((uintptr_t)rx_ring) >> 32));
    w16(R_RCR, 0x008e | 0x0040);                      /* accept broadcast+unicast+mcast */
    w8(R_TCTR, 0x60 | 0x00);                          /* IFG 96bit default, threshold   */

    /* PHY: power up + autonegotiate */
    u16 bmcr = mdio_read(0);
    mdio_write(0, (u16)((bmcr & ~0x1000u) | 0x0000u)); /* clear isolate      */
    mdio_write(0, 0x1000 | 0x0200 | 0x0000);          /* reset+autoneg      */
    sleep_ms(200);
    mdio_write(0, 0x1200);                            /* AN enable, restart */

    /* link status after short wait */
    u16 bmsr = mdio_read(1);
    g_nic.link_up = (bmsr & 0x0004) != 0;
    g_rx_idx = 0; g_tx_idx = 0;
    w16(R_IMR, 0x0000);                           /* keep polling stage-1 */
    g_nic.ready = true;

    kprintf("[rtl] MAC %02x:%02x:%02x:%02x:%02x:%02x link=%s BMSR=%#x\n",
            g_nic.mac[0], g_nic.mac[1], g_nic.mac[2],
            g_nic.mac[3], g_nic.mac[4], g_nic.mac[5],
            g_nic.link_up ? "UP" : "down", bmsr);
    return 0;
}

const rtl_nic_t *rtl_first(void) { return g_nic.ready ? &g_nic : NULL; }

int rtl_send_frame(const u8 *frame, u32 len)
{
    if (!g_nic.ready) return -1;
    if (len > 2048) return -2;
    u32 idx = g_tx_idx % NUM_TX;
    memcpy(tx_bufs[idx], frame, len);
    sfence();
    tx_ring[idx].len = (u16)len;
    tx_ring[idx].opts = 0x0000E000u | (len & 0xFFFu); /* OWN|FS|LS|TCP none */
    g_tx_idx++;
    w32(R_TPPOLL, 0x40);                              /* demand poll        */
    g_nic.tx_bytes += len; g_nic.tx_packets++;
    return 0;
}

u32 rtl_poll_rx(u8 *buf, u32 max_len)
{
    if (!g_nic.ready) return 0;
    u32 idx = g_rx_idx % NUM_RX;
    u32 st = rx_ring[idx].status;
    if (!(st & 0x80000000u)) return 0;                /* OWN still set      */
    u32 pkt_len = st & 0x1FFFu;
    if (pkt_len > 0 && pkt_len <= max_len) {
        memcpy(buf, rx_bufs[idx], pkt_len);
        g_nic.rx_bytes += pkt_len; g_nic.rx_packets++;
    }
    /* rearm descriptor */
    rx_ring[idx].status = 0x0000FFFFu;
    g_rx_idx++;
    return pkt_len;
}
