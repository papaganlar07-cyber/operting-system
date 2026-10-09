/* MacawOS kernel — PCI/PCIe configuration-space enumeration */
#ifndef MC_PCI_H
#define MC_PCI_H
#include "mc/types.h"

typedef struct {
    u16 vendor, device;
    u16 class, subclass, progif;
    u8  revision;
    u8  hdr_type;
    u8  bus, dev, func;
    u8  irq_line;
    u8  bar_count;                     /* 6 for type0, 0 for type1    */
    u64 bars[6];                       /* decoded base addresses      */
    u32 bar_sizes[6];                  /* bytes (MMIO) or raw io size */
    bool bar_mmio[6];                  /* true = memory BAR           */
} pci_dev_t;

void          pci_init(void);
const pci_dev_t *pci_find(u16 vendor, u16 device);
int           pci_enumerate(pci_dev_t *out, int max);
u32           pci_cfg_read32(const pci_dev_t *d, u8 off);
void          pci_cfg_write32(const pci_dev_t *d, u8 off, u32 val);
void          pci_enable_msi(pci_dev_t *d, u8 vec);
void          pci_bus_master(pci_dev_t *d, bool on);
u8            pci_cap_offset(const pci_dev_t *d, u8 cap_id);
#endif
