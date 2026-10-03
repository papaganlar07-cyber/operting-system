# ============================================================
# AMC OS — Derleme Sistemi
# Hedefler:
#   make bootloader : boot.img (MBR + stage2, 512*3 bayt)
#   make kernel     : amc-kernel.elf (freestanding C, -nostdlib)
#   make userspace  : init, amsh, coreutils, uygulamalar (ELF)
#   make image      : amcos.iso / amcos.img (QEMU'da boot eden disk)
#   make run        : qemu-system-x86_64 ile test sürüşü
# ============================================================

CC       := x86_64-elf-gcc          # host'ta: clang --target=x86_64-none-elf
AS       := nasm
LD       := x86_64-elf-ld
CFLAGS   := -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
            -mcmodel=kernel -std=c17 -O2 -Wall -Wextra -Werror \
            -I kernel -I include
LDFLAGS  := -T kernel/link.ld -nostdlib -static

SRC_K    := $(shell find kernel fs -name '*.c')
OBJ_K    := $(SRC_K:.c=.o)

APPS     := mediaplayer desktop terminal texteditor filemanager settings
BINS     := $(addprefix build/bin/,$(APPS)) build/bin/amsh build/bin/init

.PHONY: all bootloader kernel userspace image run clean

all: image

# ---------- Önyükleyici ----------
bootloader: build/boot.bin
	nasm -f bin boot/boot.asm  -o build/stage1.bin
	nasm -f bin boot/stage2.asm -o build/stage2.bin
	cat build/stage1.bin build/stage2.bin > build/bootimg.bin

build/stage1.bin: boot/boot.asm
	$(AS) $< -o $@

build/stage2.bin: boot/stage2.asm
	$(AS) $< -o $@

# ---------- Çekirdek ----------
kernel: $(OBJ_K) build/kernel_entry.o
	$(LD) $(LDFLAGS) -o build/amc-kernel.elf $^
	objcopy -O binary build/amc-kernel.elf build/amc-kernel.bin

kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c $< -o $@

fs/%.o: fs/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- Kullanıcı alanı (libc = libamc) ----------
userspace: $(BINS)

build/bin/%: apps/%/%.c lib/libamc.a
	@mkdir -p build/bin
	$(CC) $(CFLAGS) -Ilib/include $< lib/libamc.a -o $@

build/bin/amsh: userspace/sh/amsh.c lib/libamc.a
	$(CC) $(CFLAGS) -Ilib/include $< lib/libamc.a -o $@

lib/libamc.a: $(wildcard lib/*.c)
	ar rcs $@ $^

# ---------- Disk imajı (MBR + GPT yedeği + AMCFS bölümü) ----------
image: bootloader kernel userspace
	python3 tools/mkimage.py \
	        --boot build/bootimg.bin \
	        --kernel build/amc-kernel.bin \
	        --rootfs build/bin --rootfs lib/data \
	        --size 2G --out build/amcos.img
	@echo "IMAJ HAZIR: build/amcos.img"

run: image
	qemu-system-x86_64 \
	  -drive file=build/amcos.img,format=raw,if=virtio \
	  -device ich9-intel-hda -device hda-duplex \        # Realtek-uyumlu HDA sesi
	  -device rtl8169,netdev=n0 -netdev user,id=n0 \     # gerçek RTL çipi taklidi
	  -device qemu-xhci -drive if=usb,file=flash.qcow2 \ # USB 3.0 denetleyicisi
	  -m 4G -smp 4 -enable-kvm

clean:
	rm -rf build kernel/**/*.o fs/*.o
