#!/usr/bin/env python3
# ============================================================
# AMC OS — Disk imaji uretici (mkimage)
# MBR boot sektoru + stage2 + AMCFS bolumu (kernel, /bin, /etc)
# QEMU'da dogrudan boot edilebilir 2GB raw img uretir.
# Kullanim: python3 tools/mkimage.py --boot build/bootimg.bin \
#            --kernel build/amc-kernel.bin --rootfs build/bin --out build/amcos.img
# ============================================================
import argparse, os, struct, hashlib

SECTOR = 512
AMCFS_MAGIC = b"AMCF"

def sha256(b): return hashlib.sha256(b).hexdigest()

def write_mbr(f, boot_bin, disk_sectors):
    """Boot img'i ilk sektorlere yaz; MBR partition tablosu ekle."""
    assert len(boot_bin) >= SECTOR, "boot img en az 1 sektor olmali"
    f.write(boot_bin.ljust(SECTOR * 8, b"\0"))
    # ---- MBR partition record (offset 446): tum diski AMCF tipi yap ----
    f.seek(446)
    bootable = 0x80
    chs_start = b"\x00\x02\x00"                 # head0 sect2 cyl0
    type_id   = 0xCC                            # "AMC FS"
    chs_end   = b"\xff\xff\xff"
    lba_start = struct.pack("<I", 8)            # boot alanindan sonrasi
    nsect     = struct.pack("<I", disk_sectors - 8)
    rec = bytes([bootable]) + chs_start + bytes([type_id]) + chs_end + lba_start + nsect
    f.write(rec)                                # 1. kayit
    f.write(b"\0" * 48)                         # diger 3 kayit bos

def amcfs_format(f, kernel_path, rootfs_dirs, label="amcroot"):
    """Basit AMCFS superbloku + dosya agaci serializasyonu."""
    start = f.tell()
    files = []
    for d in rootfs_dirs:
        if not os.path.isdir(d): continue
        for dirpath, _, names in os.walk(d):
            for n in names:
                p = os.path.join(dirpath, n)
                rel = "/" + os.path.relpath(p, d)
                with open(p, "rb") as fh: data = fh.read()
                files.append((rel, data))
    if os.path.exists(kernel_path):
        with open(kernel_path, "rb") as fh:
            files.append(("/boot/kernel.bin", fh.read()))

    tree = [(name, len(data), sha256(data)) for name, data in files]
    sb = struct.pack("<IHQQQI32s",
                     int.from_bytes(AMCFS_MAGIC, "little"), 1,
                     len(files), 4096, start // SECTOR, 0,
                     label.encode().ljust(32, b"\0"))
    f.write(sb.ljust(SECTOR, b"\0"))
    # inode dizisi + veri bloklari
    off = f.tell() + SECTOR * (len(files) + 1)
    meta = b""
    for (name, size, digest) in tree:
        meta += struct.pack("<QII64s128s", off, size, 0o644,
                            bytes.fromhex(digest), name.encode())
        off += ((size + SECTOR - 1) // SECTOR) * SECTOR
    f.write(meta.ljust(SECTOR * (len(files) + 1), b"\0"))
    for _, data in files:
        pad = (-len(data)) % SECTOR
        f.write(data + b"\0" * pad)
    print("[mkimage] %d dosya AMCFS'e yazildi (label=%s)" % (len(files), label))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boot", required=True)
    ap.add_argument("--kernel", default="build/amc-kernel.bin")
    ap.add_argument("--rootfs", action="append", default=["build/bin"])
    ap.add_argument("--label", default="amcroot")
    ap.add_argument("--size", default="2G")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    mult = {"K": 1024, "M": 1024**2, "G": 1024**3}
    total = int(a.size[:-1]) * mult[a.size[-1]]
    sectors = total // SECTOR

    with open(a.boot, "rb") as fh: boot = fh.read()

    with open(a.out, "wb") as f:
        write_mbr(f, boot, sectors)
        amcfs_format(f, a.kernel, a.rootfs, a.label)
        f.truncate(total)                       # imaj boyutunu sabitle
    print("[mkimage] HAZIR: %s (%d MB)" % (a.out, total // 1024 // 1024))

if __name__ == "__main__":
    main()
