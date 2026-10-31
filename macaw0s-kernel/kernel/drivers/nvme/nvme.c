/* MacawOS kernel — NVMe 1.4 host driver (admin queue bring-up + Identify) */
#include "mc/nvme.h"
#include "mc/pci.h"
#include "mc/mm.h"
#include "mc/io.h"
#include "mc/vga.h"

/* controller register offsets */
#define NVME_CAP    0x00
#define NVME_VS     0x08
#define NVME_INTMS  0x0C
#define NVME_INTMC  0x10
#define NVME_CC     0x14
#define NVME_CSTS   0x1C
#define NVME_AQA    0x24
#define NVME_ASQ    0x28
#define NVME_ACQ    0x30

struct nvme_sqe {
    u8  opc; u8  flags; u16 cid;
    u32 nsid; u32 rsvd[2];
    u64 mptr; u64 prp1; u64 prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} PACKED ALIGNED(64);

struct nvme_cqe {
    u32 result; u32 rsvd; u16 sq_head; u16 sq_id; u16 cid; u16 status_phase;
} PACKED ALIGNED(16);

static volatile u8 *g_regs;               /* MMIO base                    */
static struct nvme_sqe *g_asq;
static struct nvme_cqe *g_acq;
static u64 g_asq_phys, g_acq_phys;
static u16 g_cid;
static u8  g_phase;                        /* expected CQE phase bit      */
static nvme_ctrl_t g_ctrl;

#define QSIZE 32                           /* admin queue entries          */

static inline u32 mmio_r32(u64 off) { return *(volatile u32 *)(g_regs + off); }
static inline u64 mmio_r64(u64 off) { return *(volatile u64 *)(g_regs + off); }
static inline void mmio_w32(u64 off, u32 v) { *(volatile u32 *)(g_regs + off) = v; mfence(); }
static inline void mmio_w64(u64 off, u64 v) { *(volatile u64 *)(g_regs + off) = v; mfence(); }

static int wait_bit(u64 reg_off, u32 mask, u32 want, u32 timeout_ms)
{
    for (u32 t = 0; t < timeout_ms; t++) {
        if ((mmio_r32(reg_off) & mask) == want) return 0;
        sleep_ms(1);
    }
    return -1;
}

/* submit one admin command and poll its completion (synchronous boot path) */
static int admin_cmd(u8 opc, u32 nsid, u64 prp1, u64 prp2,
                     u32 cdw10, u32 cdw11, void *result_out)
{
    u16 slot = g_cid % QSIZE;
    struct nvme_sqe *sqe = &g_asq[slot];
    memset(sqe, 0, sizeof(*sqe));
    sqe->opc = opc; sqe->cid = g_cid; sqe->nsid = nsid;
    sqe->prp1 = prp1; sqe->prp2 = prp2;
    sqe->cdw10 = cdw10; sqe->cdw11 = cdw11;
    sfence();
    mmio_w32(0x1000, (u32)(g_cid / QSIZE + 1));       /* SQ0 tail doorbell */

    u16 head = 0;
    for (u32 spin = 0; spin < 20000000u; spin++) {    /* bounded busy-wait */
        CPU_RELAX();
        struct nvme_cqe *cqe = &g_acq[head];
        if (((cqe->status_phase >> 8) & 1u) != g_phase &&
            (cqe->status_phase & 1u) == g_phase) {
            u16 done_cid = cqe->cid;
            head = (u16)((head + 1) % QSIZE);
            if (head == 0) g_phase ^= 1;
            mmio_w32(0x1004, head);                   /* CQ0 head doorbell */
            if (done_cid != g_cid) continue;
            int st = (cqe->status_phase >> 1) & 0xFF;
            if (result_out) *(u32 *)result_out = cqe->result;
            g_cid++;
            return st ? -st : 0;
        }
    }
    return -0x7001;                                    /* completion timeout */
}

int nvme_init(void)
{
    const pci_dev_t *d = NULL;
    /* class 0x0108 (NVMe), progif 0x02 */
    pci_dev_t list[64];
    int n = pci_enumerate(list, 64);
    for (int i = 0; i < n; i++)
        if (list[i].class == 0x0108 && list[i].progif == 0x02) { d = &list[i]; break; }
    if (!d) { kprintf("[nvme] no NVMe controller found\n"); return -1; }

    kprintf("[nvme] ctrl @ %02x:%02x.%x vendor=%#x device=%#x bar0=%#lx size=%u\n",
            d->bus, d->dev, d->func, d->vendor, d->device,
            (unsigned long)d->bars[0], d->bar_sizes[0]);
    if (!d->bar_mmio[0]) { kprintf("[nvme] BAR0 not MMIO — abort\n"); return -2; }

    pci_bus_master(d, true);
    g_regs = PHYS_TO_VIRT(d->bars[0]);                 /* HHDM mapping      */

    u64 cap = mmio_r64(NVME_CAP);
    u32 to = (u32)((cap >> 24) & 0xFF);                /* CAP.TO in 500ms   */
    u32 timeout_ms = to ? to * 500u : 5000u;
    u32 dstrd = (u32)(cap & 0xFu);                     /* doorbell stride   */
    UNUSED(dstrd);                                     /* QSIZE*stride fits 4K */

    /* disable → wait RDY=0 */
    mmio_w32(NVME_CC, 0);
    if (wait_bit(NVME_CSTS, 1u, 0u, timeout_ms) != 0) {
        kprintf("[nvme] CSTS.RDY never cleared (timeout %u ms)\n", timeout_ms);
        return -3;
    }

    /* allocate admin queues: 2 pages each, contiguous via PMM frames */
    g_asq = kzalloc(QSIZE * sizeof(struct nvme_sqe));
    g_acq = kzalloc(QSIZE * sizeof(struct nvme_cqe));
    if (!g_asq || !g_acq) { kprintf("[nvme] OOM allocating admin queues\n"); return -4; }
    /* kmalloc memory is NX-mapped but that's fine for data; we need phys */
    g_asq_phys = VIRT_TO_PHYS((uintptr_t)g_asq);
    g_acq_phys = VIRT_TO_PHYS((uintptr_t)g_acq);

    mmio_w32(NVME_AQA, ((QSIZE - 1) << 16) | (QSIZE - 1));
    mmio_w64(NVME_ASQ, g_asq_phys);
    mmio_w64(NVME_ACQ, g_acq_phys);

    /* CC: enable, IOCQES=4 (16B), IOSQES=6 (64B), AMS round-robin, CSS=NVM */
    mmio_w32(NVME_CC, (0x6u << 16) | (0x4u << 20) | (0u << 11) |
                      (0u << 4) | (6u << 7) | 1u);
    if (wait_bit(NVME_CSTS, 1u, 1u, timeout_ms) != 0) {
        kprintf("[nvme] enable timeout, CSTS=%#x\n", mmio_r32(NVME_CSTS));
        return -5;
    }
    if (mmio_r32(NVME_CSTS) & 2u) { kprintf("[nvme] fatal status (CFS)\n"); return -6; }

    /* Identify Controller (opcode 0x06, CNS=0x01) */
    static u8 idbuf[4096] ALIGNED(4096);
    u64 idphys = VIRT_TO_PHYS((uintptr_t)idbuf);
    int rc = admin_cmd(0x06, 0, idphys, 0, 0x01, 0, NULL);
    if (rc != 0) { kprintf("[nvme] Identify Controller failed rc=%d\n", rc); return -7; }

    memcpy(g_ctrl.model, idbuf + 40, 40); g_ctrl.model[40] = 0;
    memcpy(g_ctrl.serial, idbuf + 104, 20); g_ctrl.serial[20] = 0;
    u32 nn = *(u32 *)(idbuf + 512);
    u64 snsize = *(u64 *)(idbuf + 3104);              /* SNSize field      */
    UNUSED(snsize);
    u8 mdts = idbuf[77];
    g_ctrl.max_xfer_kb = mdts ? (1u << (mdts - 1)) * 4u : 128u;
    g_ctrl.vendor = d->vendor; g_ctrl.device = d->device;
    g_ctrl.nsid = nn ? 1 : 0;

    if (nn) {
        /* Identify Namespace (CNS=0x00, NSID=1) */
        rc = admin_cmd(0x06, 1, idphys, 0, 0x00, 0, NULL);
        if (rc == 0) {
            u8 flbas = idbuf[26];
            u8 lba_fmt = flbas & 0x0F;
            u16 lbads = *(u16 *)(idbuf + 32 + lba_fmt * 4 + 2); /* LBA Data Substructuring */
            u64 nsze = *(u64 *)(idbuf + 0);
            g_ctrl.lba_size = 1u << (lbads ? lbads : 9);
            g_ctrl.capacity_bytes = nsze * g_ctrl.lba_size;
        }
    }
    g_ctrl.ready = true;
    kprintf("[nvme] OK model=\"%s\" serial=\"%s\" ns=%u cap=%llu MB lba=%u\n",
            (char *)g_ctrl.model, (char *)g_ctrl.serial, g_ctrl.nsid,
            (unsigned long long)(g_ctrl.capacity_bytes / (1024 * 1024)),
            g_ctrl.lba_size);
    return 0;
}

const nvme_ctrl_t *nvme_first(void) { return g_ctrl.ready ? &g_ctrl : NULL; }

/* stage-1 I/O path: synchronous PRP1 single-page transfers (≤ lba page size) */
static int io_cmd(u8 opc, u64 lba, u32 count, u64 data_phys, u64 data2)
{
    if (!g_ctrl.ready) return -1;
    return admin_cmd(opc, g_ctrl.nsid, data_phys, data2,
                     (u32)lba, (u32)(lba >> 32),
                     ((count - 1) << 0) | 0x00000000u);
}

int nvme_read_blocks(u64 lba, u32 count, void *buf)
{
    u64 p1 = VIRT_TO_PHYS((uintptr_t)buf);
    u64 p2 = (count * g_ctrl.lba_size > PAGE_SIZE)
             ? VIRT_TO_PHYS((uintptr_t)buf + PAGE_SIZE) : 0;
    return io_cmd(0x02, lba, count, p1, p2);
}

int nvme_write_blocks(u64 lba, u32 count, const void *buf)
{
    u64 p1 = VIRT_TO_PHYS((uintptr_t)buf);
    u64 p2 = (count * g_ctrl.lba_size > PAGE_SIZE)
             ? VIRT_TO_PHYS((uintptr_t)buf + PAGE_SIZE) : 0;
    return io_cmd(0x01, lba, count, p1, p2);
}
