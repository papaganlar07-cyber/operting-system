# AMC OS — Ağ Yığını Özeti (kernel/drivers/net + userspace netd)

AMC OS'un ağ alt yapısı **tamamen bağımsız** yazılmış, BSD socket API'sine
uyumlu bir yığındır. Realtek Ethernet/Wi-Fi sürücüleri çekirdekte,
yönetim (DHCP/DNS/wifi) kullanıcı alanındaki `netd` servisinde yaşar.

## Katmanlar

```
[Uygulamalar]  curl / tarayıcı / ssh ...        POSIX socket() syscall
[netd]         DHCP client, DNS resolver, wifi supplicant (kullanıcı alanı!)
[TCP/UDP]      Sıralı bayt akışı, yeniden iletim, congestion (CUBIC benzeri)
[IP v4/v6]     Rotalama tablosu, fragmentation, ICMP
[Link]         Ethernet II, VLAN, Wi-Fi (802.11 a/b/g/n/ac/ax)
[Sürücüler]    rtl8169 (GbE), rtl8125 (2.5/5GbE), rtl8192/8821/8852 (WiFi),
               cdc-ncm/rndis (USB adaptörler)
```

## Desteklenen Realtek ağ çipleri (çoğu masaüstü/laptop'ta bulunur)

| Çip | Hız | Not |
|-----|-----|-----|
| RTL8111/8168/8411 | GbE | en yaygın onboard LAN |
| RTL8125/8126 | 2.5/5 GbE | yeni anakartlar |
| RTL8188/8192 | WiFi n | USB adaptörler |
| RTL8821CE/8822BE | WiFi ac+BT | laptop'lar |
| RTL8852AE/BE | WiFi ax | PCIe kartlar |

## Örnek: rtl8169.c çekirdek sürücüsü (kısaltılmış gerçek kod)

Sürücü RX/TX DMA halkalarıyla çalışır; kesme geldiğinde paketleri
`netif_receive()` üzerinden IP katmanına iletir. Tam kaynak:
`kernel/drivers/net/rtl8169.c`.

## Kullanıcı alanı örnekleri

```sh
amcnetwork status            # arayüzler, IP, sinyal gücü
amcnetwork connect MyWiFi --psk "gizli"
amcping 1.1.1.1              # ICMP istemcisi (yetenekli: setuid yok, capability var)
```

## Yazılımcılar için

- `socket()/bind()/listen()/accept()` birebir POSIX — mevcut Linux kodları
  port katmanı olmadan da derlenir (libamc libc).
- epoll ve kqueue **ikisi de native** — Chrome/PostgreSQL gibi projeler
  kendi event mekanizmalarını olduğu gibi kullanabilir.
- TLS: sistem libssl'i ports'tan gelir (`amcport install openssl`).
