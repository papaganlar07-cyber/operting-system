/* MacawOS kernel — PCI Mechanism #1 enumeration, BAR decode, capabilities */
#include "mc/pci.h"
#include "mc/io.h"
#include "mc/vga.h"

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

static inline u32 cfg_addr(u8 bus, u8 dev, u8 func, u8 off)
{
    return 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
           ((u32)func << 8) | (off & 0xFCu);
}

static u32 cfg_r32(u8 bus, u8 dev, u8 func, u8 off)
{
    outl(CFG_ADDR, cfg_addr(bus, dev, func, off));
    return infl(CFG_DATA);
}

static void cfg_w32(u8 bus, u8 dev, u8 func, u8 off, u32 val)
{
    outl(CFG_ADDR, cfg_addr(bus, dev, func, off));
    outl(CFG_DATA, val);
}

static u16 cfg_r16(u8 bus, u8 dev, u8 func, u8 off)
{
    u32 v = cfg_r32(bus, dev, func, off & ~3u);
    return (u16)(v >> ((off & 2u) * 8u));
}

#define MAX_DEVS 128
static pci_dev_t g_devs[MAX_DEVS];
static int g_ndev;

static bool probe_func(u8 bus, u8 dev, u8 func)
{
    u32 vd = cfg_r32(bus, dev, func, 0x00);
    if ((vd & 0xFFFFu) == 0xFFFFu) return false;
    pci_dev_t *d = &g_devs[g_ndev++];
    d->vendor = (u16)(vd & 0xFFFF);
    d->device = (u16)(vd >> 16);
    u32 revcls = cfg_r32(bus, dev, func, 0x08);
    d->revision = (u8)(revcls & 0xFF);
    d->class    = (u16)((revcls >> 16) & 0xFFFF);
    d->subclass = (u8)((revcls >> 16) & 0xFF);
    d->progif   = (u8)((revcls >> 24) & 0xFF);
    d->hdr_type = (u8)((cfg_r32(bus, dev, func, 0x0C) >> 16) & 0xFF);
    d->irq_line = (u8)(cfg_r32(bus, dev, func, 0x3C) & 0xFF);
    d->bus = bus; d->dev = dev; d->func = func;

    /* BAR decoding for type-0 headers */
    d->bar_count = 0;
    if ((d->hdr_type & 0x7F) == 0x00) {
        for (int i = 0; i < 6; i++) {
            u8 reg = (u8)(0x10 + i * 4);
            u32 raw = cfg_r32(bus, dev, func, reg);
            if (raw == 0) continue;
            d->bar_count++;
            if (raw & 1u) {                          /* I/O space         */
                d->bar_mmio[i] = false;
                cfg_w32(bus, dev, func, reg, 0xFFFFFFFFu);
                u32 szm = cfg_r32(bus, dev, func, reg);
                cfg_w32(bus, dev, func, reg, raw);
                szm &= 0xFFFFFFFCu;
                d->bar_sizes[i] = szm ? (~szm + 1u) : 0x100u;
                d->bars[i] = raw & 0xFFFFFFFCu;
            } else {                                 /* memory space      */
                d->bar_mmio[i] = true;
                u32 type = (raw >> 1) & 3u;          /* 0=32 2=64         */
                u64 base = raw & 0xFFFFFFF0u;
                u64 high = 0;
                if (type == 2u && i + 1 < 6) {
                    high = cfg_r32(bus, dev, func, (u8)(reg + 4));
                    i++;                             /* consumed next BAR */
                    d->bar_count++;
                }
                base |= (high << 32);
                cfg_w32(bus, dev, func, reg, 0xFFFFFFF0u);
                u32 szm = cfg_r32(bus, dev, func, reg);
                cfg_w32(bus, dev, func, reg, (u32)(base & 0xFFFFFFFF));
                if (type == 2u)
                    cfg_w32(bus, dev, func, (u8)(reg + 4), (u32)(base >> 32));
                u64 size = (u64)(~(szm & 0xFFFFFFF0u)) + 1u;
                d->bars[i] = base;
                d->bar_sizes[i] = (u32)(size > 0xFFFFFFFFu ? 0xFFFFFFFFu : size);
                if (type == 2u && i < 6) {           /* mark pair slot used */
                    d->bars[i] = 0; d->bar_mmio[i] = true; d->bar_sizes[i] = 0;
                }
            }
        }
    }
    return true;
}

void pci_init(void)
{
    g_ndev = 0;
    for (u8 bus = 0; bus < 256; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u32 hdr = cfg_r32(bus, dev, 0, 0x0C);
            if (hdr == 0xFFFFFFFFu) continue;        /* no device on line */
            u8 multifn = (u8)((hdr >> 16) & 0x80u);
            probe_func(bus, dev, 0);
            if (multifn)
                for (u8 func = 1; func < 8; func++) probe_func(bus, dev, func);
            if (g_ndev >= MAX_DEVS) return;
        }
    }
    kprintf("[pci] enumerated %d devices\n", g_ndev);
}

const pci_dev_t *pci_find(u16 vendor, u16 device)
{
    for (int i = 0; i < g_ndev; i++)
        if (g_devs[i].vendor == vendor &&
            (device == 0xFFFFu || g_devs[i].device == device))
            return &g_devs[i];
    return NULL;
}

int pci_enumerate(pci_dev_t *out, int max)
{
    int n = g_ndev < max ? g_ndev : max;
    memcpy(out, g_devs, (size_t)n * sizeof(pci_dev_t));
    return n;
}

u32 pci_cfg_read32(const pci_dev_t *d, u8 off)
{ return cfg_r32(d->bus, d->dev, d->func, off); }

void pci_cfg_write32(const pci_dev_t *d, u8 off, u32 val)
{ cfg_w32(d->bus, d->dev, d->func, off, val); }

u8 pci_cap_offset(const pci_dev_t *d, u8 cap_id)
{
    u8 ptr = (u8)(pci_cfg_read32(d, 0x34) & 0xFF);
    for (int guard = 0; guard < 48 && ptr >= 0x40; guard++) {
        u32 cc = pci_cfg_read32(d, ptr & ~3u);       /* cap id + next ptr */
        if ((u8)(cc & 0xFF) == cap_id) return ptr;
        ptr = (u8)((cc >> 8) & 0xFC);                /* aligned next link */
    }
    return 0;
}

void pci_bus_master(const pci_dev_t *d, bool on)
{
    u32 cmd = pci_cfg_read32(d, 0x04);
    cmd |= (1u << 1);                                /* Bus Master        */
    if (on) cmd |= (1u << 0) | (1u << 2);            /* IO + Mem spaces   */
    else    cmd &= ~((1u << 0) | (1u << 2));
    pci_cfg_write32(d, 0x04, cmd);
}

void pci_enable_msi(pci_dev_t *d, u8 vec)
{
    u8 off = pci_cap_offset(d, 0x05);
    if (!off) return;
    u32 mc = pci_cfg_read32(d, off + 2);              /* Message Control  */
    mc &= ~0x70000u;                                  /* 1 message        */
    mc |= 1u;                                         /* MSI enable       */
    pci_cfg_write32(d, off + 2, mc);
    pci_cfg_write32(d, off + 4, (u32)(vec & 0xFF));  /* addr low (LAPIC) */
    pci_cfg_write32(d, off + 8, 0);                  /* data             */
}
