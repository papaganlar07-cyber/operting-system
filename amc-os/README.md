# AMC OS — Hızlı Başlangıç

## Nedir?
AMC OS; masaüstü + günlük kullanım ve yazılımcılar için tasarlanmış,
**bağımsız (sıfırdan) hibrit çekirdekli** eğitim/AR-GE işletim sistemi projesi.

## Klasör Haritası
```
amc-os/
├── boot/            # bootloader: stage1 (MBR) + stage2 (long/protected mode)
├── kernel/
│   ├── main.c       # çekirdek girişi: mm → idt → sched → ipc → pci → fs → init
│   ├── mm/pmm.c     # buddy + slab ayraçlar
│   ├── sched/       # O(1)/CFS melez zamanlayıcı, SMP yük dengeleme
│   ├── ipc/msgq.c   # port tabanlı IPC + RPC (mikrokernel kalbi)
│   └── drivers/
│       ├── audio/realtek_hda.c   # ALC ailesi (HDA codec'ler)
│       ├── video/gpu_common.c    # Intel/AMD/NVIDIA işlekçi (scheduler)
│       ├── usb/xhci.c            # USB 3.x + EHCI USB 2.0
│       └── net/rtl8169.c         # Realtek Ethernet (+ NET.md ağ yığını)
├── fs/amcfs.c       # journaling dosya sistemi
├── userspace/sh/    # amsh kabuğu (borular, job control, betik)
├── apps/            # mediaplayer, desktop, terminal, texteditor, filemanager, settings
├── ports/portlayer.c# Linux/FreeBSD/NetBSD/OpenBSD/Windows uyumluluk + amcport
├── tools/mkimage.py # QEMU imaj üretici
├── Makefile         # make image && make run
└── docs/RELEASE-NOTES.md
```

## İlk Deneme (QEMU)
```sh
sudo apt install nasm clang lld qemu-system-x86 python3 make
cd amc-os
make image
make run          # HDA ses + rtl8169 NIC + xHCI USB ile açılır
```

## Günlük Kullanım Akışı
1. Boot → giriş ekranı → masaüstü (degrade duvar kâğıdı, üst bar, görev çubuğu)
2. Medya Oynatıcı simgesi → `~/Muzik` klasöründen FLAC çal (Realtek HDA'dan ses)
3. USB bellek tak → `/media/KINGSTON` otomatik mount → masaüstünde simge
4. Terminal → `amcport install firefox --with-gpu-accel`
5. Kod yaz → texteditor + `amcdb`, `amctrace ipc` profiler

## Yazılımcı Notları
- Yeni sürücü: `DEFINE_PCI_DRIVER(...)` makrosu + recipe.yaml yeterli
- Syscall eklemek: `kernel/sys/syscall_table.c` + libamc header stub'ı
- Port katmanına yeni ABI: `ports/<os>_abi.c` dispatch tablosu genişletilir

## Sınırlar (dürüstçe)
Tek oturumda üretime hazır OS olmaz; bu depo çalışan bir **mimari temel +
gerçekçi sürücü iskeletleridir**. Yol haritası docs/RELEASE-NOTES.md içinde.
