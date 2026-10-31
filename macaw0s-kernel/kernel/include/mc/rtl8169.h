/* MacawOS kernel — Realtek RTL8168/8111/8125 Ethernet driver API */
#ifndef MC_RTL8169_H
#define MC_RTL8169_H
#include "mc/types.h"

typedef struct {
    u16 vendor, device;
    char chip[32];          /* human name e.g. "RTL8168H"            */
    u8  mac[6];
    u32 rx_bytes, tx_bytes, rx_packets, tx_packets;
    bool link_up;
    bool mmio;              /* true = MMIO BAR, false = PIO           */
    bool ready;
} rtl_nic_t;

int  rtl_init(void);                      /* PCI scan + bring-up      */
const rtl_nic_t *rtl_first(void);
int  rtl_send_frame(const u8 *frame, u32 len);
u32  rtl_poll_rx(u8 *buf, u32 max_len);   /* copies one frame, 0=none */
#endif
