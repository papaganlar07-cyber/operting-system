/* MacawOS kernel — ACPI table discovery (RSDP → XSDT/RSDT) */
#ifndef MC_ACPI_H
#define MC_ACPI_H
#include "mc/types.h"

typedef struct PACKED {
    u8   signature[8];      /* "RSD PTR "                            */
    u8   checksum;
    u8   oem_id[6];
    u8   revision;
    u32  rsdt_address;
    u32  length;            /* ACPI 2.0+                             */
    u64  xsdt_address;      /* ACPI 2.0+                             */
    u8   ext_checksum;
    u8   reserved[3];
} acpi_rsdp_t;

typedef struct PACKED {
    u8   signature[4];      /* 'RSDT' or 'XSDT' etc.                 */
    u32  length;
    u8   revision;
    u8   checksum;
    u8   oem_id[6];
    u8   oem_table_id[8];
    u32  oem_revision;
    u8   creator_id[4];
    u32  creator_revision;
} acpi_hdr_t;

typedef struct {
    bool found;
    const acpi_rsdp_t *rsdp;
    u32  table_count;
    u64  table_phys[64];    /* physical addresses of each entry      */
} acpi_info_t;

void acpi_init(void);
const acpi_info_t *acpi_get(void);
const void *acpi_find_table(const char sig[4], u32 *out_len, u64 *out_phys);
u8 acpi_checksum(const volatile void *tbl, u32 len);
#endif
