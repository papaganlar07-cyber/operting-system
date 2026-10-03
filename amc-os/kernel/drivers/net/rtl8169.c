/* ============================================================
 * AMC OS — RTL8169/RTL8125 Realtek Ethernet Sürücüsü (özet çekirdek)
 * kernel/drivers/net/rtl8169.c
 *   - GbE (8111/8168/8411) + 2.5/5GbE (8125/8126) ailesi
 *   - RX/TX DMA halkaları, jumbo frame, TSO/checksum offload
 *   - PHY yönetimi: autoneg, kablo tak/çıkar olayı → netd'ye IPC
 * ============================================================ */

#include "rtl8169.h"
#include "../../kernel.h"

#define RTL_TOKRING_SZ   256
#define RTL_MAX_FRAME    9000          /* jumbo */

struct rtl8169_priv {
    volatile void *mmio;
    struct rx_desc rx_ring[RTL_TOKRING_SZ] __aligned(256);
    struct tx_desc tx_ring[RTL_TOKRING_SZ] __aligned(256);
    uint16_t rx_idx, tx_idx;
    struct net_device *netdev;
    int chip;                          /* RTL_GIGA / RTL_8125 ... */
};

static const struct rtl_chip_info chips[] = {
    { RTL8168, "RTL8168 Gigabit",   .has_4k_dma = 1 },
    { RTL8111, "RTL8111 GbE",       .has_4k_dma = 1 },
    { RTL8411, "RTL8411 Combo",     .has_4k_dma = 1 },
    { RTL8125, "RTL8125 2.5GbE",    .speed_max = SPEED_2500 },
    { RTL8126, "RTL8126 5GbE",      .speed_max = SPEED_5000 },
    {}
};

static int rtl_open(struct net_device *ndev) {
    struct rtl8169_priv *tp = ndev->priv;
    rtl_hw_reset(tp);
    rtl_init_rings(tp);                    /* descriptor + sk_buff havuzu */
    rtl_set_mac_address(ndev, ndev->mac);  /* MAC register programla */
    rtl_start_phy(tp);                     /* autoneg başlat */
    tp->netdev->flags |= IFF_UP;
    return 0;
}

/* Kesme: RX dolu mu / TX tamamlandı mı / link değişti mi? */
static irqreturn_t rtl_interrupt(int irq, void *data) {
    struct rtl8169_priv *tp = data;
    uint32_t ints = rtl_intr_status(tp);

    if (ints & RTL_RX_OK)  rtl_rx_poll(tp, 64);      /* paketleri al */
    if (ints & RTL_TX_OK)  rtl_tx_clean(tp);         /* tamponları serbest */
    if (ints & RTL_RX_ERR) stats.rx_errors++;
    if (ints & RTL_LINK_CHG) {                      /* kablo tak/çıkar */
        bool up = rtl_phy_link_up(tp);
        netif_carrier(ndev, up);
        ipc_post_msg(SVC_NETD, NET_EVT_LINK, ndev->ifindex, up);
        kprintf("[net] %s: link %s (%d Mbps)\n", ndev->name,
                up ? "YUKARI" : "ASAGI", rtl_link_speed(tp));
    }
    rtl_intr_mask_write(tp, INTR_DEFAULT);
    return IRQ_HANDLED;
}

/* Gönderim: skb → TX descriptor zinciri (TSO varsa segment GPU... eh, NIC!) */
static netdev_tx_t rtl_start_xmit(struct sk_buff *skb, struct net_device *ndev) {
    struct rtl8169_priv *tp = ndev->priv;
    struct tx_desc *d = &tp->tx_ring[tp->tx_idx];

    d->addr = cpu_to_le64(virt_to_dma(skb->data));
    d->opts = cpu_to_le32(skb->len | (skb_is_gso(skb) ? DESC_TSO : 0)
                                | DESC_OWN_HW);
    tp->tx_idx = (tp->tx_idx + 1) % RTL_TOKRING_SZ;
    writel(TX_RING_ADDR(tp->tx_idx), tp->mmio + TXPOLL);  /* kapıya vur */
    return NETDEV_TX_OK;
}

static int rtl_probe(struct pci_dev *pdev) {
    struct rtl8169_priv *tp = devm_alloc(pdev);
    tp->mmio = ioremap_align_64bit(pdev->bar[0], SZ(KiB(256)));
    tp->chip = rtl_find_chip_id(pdev);

    /* OTP/PROM'dan MAC oku */
    pdev->dev_addr = eeprom_read_mac(tp);

    netdev = alloc_netdev(rtl_ops);
    netdev->features = NETIF_F_RXCSUM | NETIF_F_TSO | NETIF_F_JUMBO;
    register_netdev(netdev);               /* "eth0"/"enp3s0" doğar */
    pci_enable_msi_msix(pdev, rtl_interrupt, tp);
    kprintf("[net] %s bulundu\n", chips[tp->chip].name);
    return 0;
}

static const struct pci_device_id rtl8169_ids[] = {
    { 0x10EC, 0x8169 }, { 0x10EC, 0x8168 }, { 0x10EC, 0x8167 },
    { 0x10EC, 0x8163 }, { 0x10EC, 0x8136 }, { 0x10EC, 0x3000 }, /* 8125 */
    { 0x10EC, 0x5000 }, { 0x10EC, 0x8125 }, { 0x10EC, 0x3061 }, /* 8126 */
    {}
};

DEFINE_PCI_DRIVER(rtl8169, rtl8169_ids, rtl_probe,
                  MODULE_DESCRIPTION("Realtek RTL8168/8111/8125 Ethernet"));
