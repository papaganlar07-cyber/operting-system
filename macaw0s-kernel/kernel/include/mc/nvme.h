/* MacawOS kernel — NVMe 1.4 host controller driver API */
#ifndef MC_NVME_H
#define MC_NVME_H
#include "mc/types.h"

typedef struct {
    u16 vendor, device;
    u8  model[41];          /* Identify Controller MN                */
    u8  serial[21];         /* SN                                      */
    u32 nsid;
    u64 capacity_bytes;
    u32 lba_size;
    u32 max_xfer_kb;        /* MDTS decoded                            */
    bool ready;
} nvme_ctrl_t;

int  nvme_init(void);                    /* PCI scan + bring-up       */
const nvme_ctrl_t *nvme_first(void);
int  nvme_read_blocks(u64 lba, u32 count, void *buf);
int  nvme_write_blocks(u64 lba, u32 count, const void *buf);
#endif
