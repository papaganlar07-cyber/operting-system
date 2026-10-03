/* ============================================================
 * AMC OS — USB Alt Sistemi: xHCI (USB 3.0/3.1/3.2) + EHCI (USB 2.0)
 * drivers/usb/xhci.c  +  ehci.c birleşik gösterimi
 *
 * Özellikler:
 *   - Host denetleyici: xHCI 1.1 (SuperSpeed 5Gbps, USB3.2 Gen2 10Gbps)
 *     ve EHCI 1.0 (High-Speed 480Mbps), UHCI/OHCI yedek (Full/Low)
 *   - Transfer ring'leri: TRB zincirleri, doorbell, event queue
 *   - Aygıt sınıfları:
 *       usb-storage (BOT / UAS)      -> USB disk, SSD, flash bellek
 *       hid (boot keyboard/mouse)    -> masaüstü girdi aygıtları
 *       hub (tt split, multi-TT)     -> kablolu kablosuz çoğaltma
 *       audio class (UAC1/UAC2)      -> USB ses kartları
 *       net (cdc-ncm / rndis)        -> USB ethernet/4G adaptörleri
 *   - Sıcak takma (hotplug) + port power yönetimi
 * ============================================================ */

#include "xhci.h"
#include "../../kernel.h"

#define XHCI_HCSPCAPS_OFF   0x000
#define XHCI_HCVERSION      0x010
#define XHCI_USBCMD         0x000
#define XHCI_USBSTS         0x004
#define XHCI_DNCTRL         0x018
#define XHCI_PORTSC(p)      (0x200 + 0x10*(p))

struct xhci_host {
    volatile uint32_t *op_reg;     /* Operational registers  */
    volatile uint32_t *rt_reg;     /* Runtime registers      */
    volatile uint32_t *cap_reg;    /* Capability registers   */
    uint8_t  num_ports;
    uint8_t  num_intrs;
    struct xhci_slot  slots[256];  /* adres -> slot          */
    struct xhci_eps   eps[256][32];/* endpoint state machines*/
};

static struct xhci_host g_xhci;

/* ================== Denetleyici başlatma ================== */

static int xhci_controller_reset(struct xhci_host *h) {
    /* HCS.POR: Power On bitini bekle (max 1sn) */
    for (int i = 0; i < 1000; i++) {
        if (h->cap_reg[0] & BIT(24)) return 0;
        mdelay(1);
    }
    return -ETIMEDOUT;
}

static int xhci_init(struct pci_dev *dev) {
    struct xhci_host *h = &g_xhci;
    h->op_reg = ioremap(dev->bar[0], 0x10000);
    h->cap_reg = h->op_reg;             /* capability BAR0 başında */
    h->rt_reg  = h->op_reg + ((readl(&h->cap_reg[1]) >> 16) & 0xFFFF) * 4;

    if (xhci_controller_reset(h) < 0) return -ENODEV;

    uint32_t hcsparams = readl(&h->cap_reg[4]);
    h->num_ports = hcsparams & 0xFF;
    h->num_intrs = (hcsparams >> 8) & 0x7FF;

    /* Event / Command DMA halkaları */
    main_event_ring_alloc(h, SZ(MiB(1)));
    command_ring_alloc(h, SZ(MiB(1)));

    /* DCPORTPMSC ile port güç yönetimi; HDC enabled */
    writel(&h->op_reg[XHCI_USBCMD/4], USBCMD_HCE | USBCMD_RUN_STOP);
    while (!(readl(&h->op_reg[XHCI_USBSTS/4]) & USBSTS_HCH)) cpu_relax();

    pci_enable_msi(dev, xhci_irq_handler);
    kprintf("[usb] xHCI %u port, USB %u.%u (SuperSpeed)\n",
            h->num_ports, h->cap_reg[XHCI_HCVERSION/4] & 0xFF,
            (h->cap_reg[XHCI_HCVERSION/4] >> 8) & 0xFF);

    usbcore_register_host(&xhci_ops, h);
    return 0;
}

/* ================== Port durum değişimi (hotplug) ================== */

static void xhci_port_status_change(struct xhci_host *h, uint8_t port) {
    uint32_t sc = readl(&h->op_reg[XHCI_PORTSC(port)/4]);
    bool connected = sc & PORTSC_CCS;
    uint8_t speed  = (sc >> PORTSC_SPEED_SHIFT) & 0xF;

    static const char *speed_names[] = {
        "Full/Low", "Low", "Full", "High(USB2.0)",
        "Super(USB3.0)", "Super+Gen2(10Gbps)"
    };

    if (connected) {
        kprintf("[usb] port%u: aygit baglandi -> %s\n",
                port, speed_names[speed > 5 ? 0 : speed]);
        usb_new_device(h, port, speed);   /* enumeration başlat */
    } else {
        kprintf("[usb] port%u: aygit cikarildi\n", port);
        usb_disconnect_device(h, port);
    }
    /* CC change biti temizle (W1C) */
    writel(&h->op_reg[XHCI_PORTSC(port)/4], sc | PORTSC_CSC);
}

/* ================== Enumeration (SETUP paketleri) ================== */

static int usb_enumerate_device(struct usb_device *ud) {
    struct usb_device_descriptor dd;
    /* 1) Default kontrol EP0 üzerinden GET_DESCRIPTOR(device) */
    int r = ctrl_transfer(ud, REQ_STD | REQ_IN, GET_DESCRIPTOR,
                          DESC_DEVICE << 8, 0, &dd, sizeof(dd));
    if (r < 0) return r;
    kprintf("[usb] cihaz: vid=0x%04x pid=0x%04x class=0x%02x\n",
            dd.idVendor, dd.idProduct, dd.bDeviceClass);

    /* 2) Konfigürasyon oku, interface'leri sınıf sürücülerine eşle */
    struct usb_config_descriptor cfg;
    ctrl_transfer(ud, REQ_STD | REQ_IN, GET_DESCRIPTOR,
                  DESC_CONFIG << 8, 0, &cfg, sizeof(cfg));
    usb_class_bind(ud, &cfg);   /* storage/hid/audio/net seçimi */

    /* 3) Adres ata ve SET_CONFIGURATION */
    ctrl_transfer(ud, REQ_STD, SET_ADDRESS, ud->address, 0, NULL, 0);
    ctrl_transfer(ud, REQ_STD, SET_CONFIGURATION, cfg.bConfigurationValue,
                  0, NULL, 0);
    return 0;
}

/* ================== Sınıf bağlama tablosu ================== */

static const struct usb_class_driver class_drivers[] = {
    { USB_CLASS_MASS_STORAGE, &usb_storage_ops },  /* BOT + UAS */
    { USB_CLASS_HID,          &hid_ops          },  /* klavye/fare */
    { USB_CLASS_AUDIO,        &uac_ops          },  /* UAC1/UAC2 ses */
    { USB_CLASS_COMM,         &usbnet_ops       },  /* CDC-NCM/RNDIS */
    { USB_CLASS_HUB,          &hub_ops          },  /* çoklu-TT hub */
    {}
};

void usb_class_bind(struct usb_device *ud, struct usb_config_descriptor *cfg) {
    for (auto *c = class_drivers; c->probe; c++) {
        if (matches_class(cfg, c->class_code)) {
            c->driver->probe(ud, cfg);
            return;
        }
    }
    kprintf("[usb] sinif surucusu bulunamadi (class=0x%02x)\n",
            cfg->bInterfaceClass);
}

/* ================== usb-storage: SCSI Bot + UAS ================== */

static int uss_probe(struct usb_device *ud, struct usb_config_descriptor *cfg) {
    /* Bulk-Only Transport: CBW/CSW protokolü */
    struct scsi_device *sd = scsi_alloc_host(ud);
    sd->scan = usb_scsi_scan;      /* INQUIRY + READ CAPACITY */
    block_register(sd);            /* /dev/sdX olarak görünür */
    return 0;
}

/* UAS (USB Attached SCSI) — SuperSpeed için komut pipelining */
static int uas_probe(struct usb_device *ud, ...) {
    struct scsi_device *sd = uas_alloc(ud);
    /* status pipe + 32 komut stream'i -> NCQ benzeri paralellik */
    block_register(sd);
    return 0;
}

/* ================== HID boot klavye/fare ================== */

static void hid_keyboard_report(uint8_t *report, int len) {
    /* 8-byte boot protocol: modifier + reserved + 6 keycode */
    for (int i = 2; i < 8; i++) {
        if (report[i]) input_key_press(hid_keymap_us[report[i]]);
    }
}

static void hid_mouse_report(int8_t *dx, int8_t *dy, uint8_t buttons) {
    input_mouse_move(dx, dy);
    input_mouse_buttons(buttons);
}

/* ================== EHCI (USB 2.0 yalnız modülü) ================== */

/* xHCI yoksa eski makinelerde yüksek hız için ayrı EHCI sürücüsü */
static int ehci_init(struct pci_dev *dev) {
    volatile uint32_t *mmio = ioremap(dev->bar[0], 0x100);
    /* COMMAND REGISTER: RUN bit + async period list / periodic list */
    writel(mmio + EHCI_CMD, EHCI_CMD_RUN);
    writel(mmio + EHCI_PERIODIC_LIST, virt_to_phys(qhd_periodic));
    writel(mmio + EHCI_ASYNC_LIST_ADDR, virt_to_phys(qhd_async));
    kprintf("[usb] EHCI (USB 2.0, 480Mbps) hazir\n");
    return 0;
}

static const struct pci_device_id xhci_ids[] = {
    { PCI_ANY_VENDOR, PCI_ANY_DEV, .class = 0x0C0330 }, /* xHCI class */
    {}
};
static const struct pci_device_id ehci_ids[] = {
    { PCI_ANY_VENDOR, PCI_ANY_DEV, .class = 0x0C0320 }, /* EHCI class */
    {}
};

DEFINE_PCI_DRIVER(xhci, xhci_ids, xhci_init,
                  MODULE_DESCRIPTION("xHCI USB 3.x host controller"));
DEFINE_PCI_DRIVER(ehci, ehci_ids, ehci_init,
                  MODULE_DESCRIPTION("EHCI USB 2.0 host controller"));
