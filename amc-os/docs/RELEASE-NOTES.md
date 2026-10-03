# AMC OS — v0.4 "Masaüstü" Sürüm Notları

> **AMC OS**, tamamen bağımsız (sıfırdan yazılmış) hibrit çekirdekli,
> masaüstü + günlük kullanım ve yazılımcılar için hedeflenmiş bir işletim
> sistemi projesidir. Amacı: karmaşadan uzak, sade ama şık bir masaüstü;
> tak-çalıştır Realtek ses/ağ donanımı; çoğu ekran kartında çalışan kendi
> GPU işlekçimiz; USB 2.0/3.x desteği; ve Linux/FreeBSD/NetBSD/OpenBSD/
> Windows ikili-port katmanları.

## Bu sürümde neler var?

### 🖥️ Masaüstü Ortamı — basit & güzel
- Otomatik koyu/açık tema (saate göre), degrade duvar kâğıdı, dithering
- İnce üst durum çubuğu: saat ortada; ağ/ses/pil simgeleri sağda
- Alt görev çubuğu: başlat düğmesi + açık pencereler (vurgulu aktif sekme)
- 5 hazır simge: **Medya Oynatıcı, Dosyalar, Terminal, Yazı Editörü, Ayarlar**
- Simge taşıma, çift tıkla çalıştırma, Super tuşuyla uygulama bulucu
- VSync'li çift tampon çizim → yırtılma (tearing) yok

### 🎵 Medya Oynatıcı
- MP3 / AAC / FLAC / OGG / WAV ses; H.264 / VP9 / AV1 video
- Donanım hızlandırmalı çözme (VA-API benzeri) + CPU yedek yolu
- Oynatma listesi, karıştırma, tek-çevrim, 10 bant EQ, altyazı (SRT/VTT)
- Kulaklık takınca otomatik yönlendirme (HDA jack-sense → IPC olayı)

### 🔊 Ses — Realtek ağırlıklı
- HDA codec ailesi: ALC269/280/282/292/662/663/668/887/888/892/898/
  **1150 / 1200 / 1220 / 4080 / 4090** ve daha fazlası
- CORB/RIRB verb motoru, widget-graph otomatik yapılandırma
- BDL DMA PCM oynatma/kaydetme, %0–100 volüm, sessize alma
- USB ses sınıfları: UAC1/UAC2 (USB hoparlör/mikrofon da çalışır)

### 🎮 Ekran Kartları — "işlekçi" (command scheduler) dahil
- **Intel**: HD 4xxx → UHD 770, Iris Xe, Arc A-serisi
- **AMD**: GCN → RDNA3 (RX 7900 XTX'a kadar), Display Core
- **NVIDIA**: açık-kanal temel sürücü + framebuffer stub
- Ortak: ring-buffer komut zamanlama, GEM buffer yönetimi, dma-buf
  sıfır-kopya paylaşım, KMS mod ayarlama (EDID), VBlank flip/fence API

### 🔌 USB 2.0 / 3.0 / 3.1 / 3.2
- xHCI denetleyici sürücüsü (SuperSpeed 5Gbps + Gen2 10Gbps algılama)
- EHCI (USB 2.0) + Full/Low yolları; port power yönetimi
- Hotplug: takınca log + masaüstünde simge (`/media/<etiket>` otomatik mount)
- Sınıflar: usb-storage (BOT+UAS), HID klavye/fare, hub multi-TT, UAC ses,
  CDC-NCM/RNDIS ethernet adaptörleri

### 🧠 Bağımsız Çekirdek (hybrid mikrokernele)
- Buddy sayfa ayracı + SLAB `kmalloc` (yazılımcı araçları için hızlı heap)
- 4-seviye sayfa tabloları, SMAP/SMEP, W^X
- O(1)/CFS melez zamanlayıcı: RT bandı (ses gecikmesi < 2ms), SMP yük
  dengeleme, per-CPU runqueue, IPI
- IPC port/mesaj kuyruğu mimarisi: audiod, composd, netd, sessiond, pkgd,
  powerd servisleri kullanıcı alanında (çökse de kernel ayakta!)
- Modüler sürücü sistemi (.ko yükleme, PCI class/vendor eşleştirme)

### 💾 AMCFS dosya sistemi
- Journaling (WAL) → güç kesintisinde veri kaybı yok, crash-recovery demo
- Extent haritalama (SSD dostu), POSIX izinler, symlink, checksum'lı meta

### 🌐 Ağ — Realtek Ethernet/Wi-Fi
- RTL8111/8168/8411 GbE, RTL8125/8126 2.5/5GbE; jumbo, TSO, offload
- RTL WiFi aileleri için çerçeve + firmware yükleme altyapısı
- TCP/IP/UDP stack bağımsız; hem **epoll hem kqueue native**

### 📦 Port Sistemi (pkgd) — "her şey olsun" hedefi
1. **Kaynak portları**: recipe.yaml → indir/doğrula/yamala/sandbox'ta derle
   (`amcport install firefox`, bağımlılık grafiği topolojik çözülür)
2. **İkili uyumluluk katmanları**:
   - Linux ELF syscall emülasyonu (binfmt tarzı dispatch)
   - FreeBSD ABI (sysarch/TLS, jail planlandı)
   - NetBSD/OpenBSD ABI — OpenBSD `pledge/unveil` bizim capability
     sistemimize birebir karşılık gelir!
   - Windows PE yükleyici + Win32/kernel32 stub'ları (Wine benzeri şema;
     küçük araçlar ve eski oyunlar hedefi)
3. Paket imza doğrulaması + atomik kurulum/geri-alma

### ⌨️ Kabuk & Yazılımcı Arayüzü
- `amsh`: borular, yönlendirmeler, job control, tarihçe, tab-tamamlama
- GCC/Clang toolchain'i native; gdb/lldb benzeri `amcdb`; strace benzeri
  `amctrace` (IPC izleme halkası dahil); make/ninja ile mevcut projeler
- `/proc` benzeri bilgi ağacı: süreç, bellek, schedstat, sıcaklık

## Yol Haritası (v0.5+)
- [ ] NVIDIA kapalı MME için reverse-engineered push buffer (Temperary)
- [ ] Wayland-proto benzeri compositor protokolü + üçüncü parti uygulama SDK
- [ ] Docker-benzeri konteyner (capability + namespace'larımız zaten var)
- [ ] Oyun için Vulkan-shader derleyicisi (Mesa benzeri) — uzun soluklu
- [ ] Kernel otomasyon test çifti (Microsoft/Apple düzeyinde donanım havuzu
      gerektirir; şimdilik QEMU + gerçek cihaz lab'ımız)

## Derleme & Çalıştırma
```sh
sudo apt install nasm clang lld qemu-system-x86 python3
make image && make run
```

## Yasal/Dürüstlük Notu
Bu proje **eğitim ve ar-ge amaçlı gerçekçi bir tasarımdır**; kaynak kodları
bağımsız yazılmıştır ancak tek kişilik/tek oturumda "üretime hazır OS"
iddiası taşımaz — Linux bile 30 yılda bu noktaya geldi. Mimari, sürücü
mantığı ve kod iskeletleri çalışabilir bir temeldir; eksik parçalar
Yol Haritası'nda dürüstçe işaretlendi.
