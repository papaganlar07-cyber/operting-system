# ============================================================
#  AMC-OS Makefile - boot.asm + kernel.c -> amc-os.iso (GRUB)
#  Kullanim:
#     make            -> ISO uretir (build/amc-os.iso)
#     make run        -> QEMU'da baslatir
#     make clean      -> her seyi temizler
#   Cross-compiler varsa:  make CROSS=i686-elf-
#   Duz yerel gcc ile    :  make CROSS=
# ============================================================

CROSS ?= $(shell command -v i686-elf-gcc >/dev/null 2>&1 && echo "i686-elf-" || echo "")

NASM          := nasm
CC            := $(CROSS)gcc
LD            := $(CROSS)gcc
OBJCOPY       := $(CROSS)objcopy
GRUB_MKRESCUE := grub-mkrescue

# --- Bayraklar ---------------------------------------------------------
NASMFLAGS := -f elf32

# Derleyici/linker mimari bayragi: 32-bit hedef.
# Debian/Ubuntu gcc hem -m32 destekler; i686-elf-gcc icin bos birakilir.
ARCHFLAG := $(if $(filter i686-elf-,$(CROSS)),,-m32)

# freestanding, red-zone yok (interruptler icin kritik), PIC yok
CFLAGS  := -std=gnu17 -O2 -g -Wall -Wextra -Werror \
           -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -mno-red-zone $(ARCHFLAG) -Isrc

LDFLAGS := -T src/linker.ld -nostdlib -static $(ARCHFLAG) \
           -z max-page-size=0x1000 -z noexecstack

# --- Dosyalar ----------------------------------------------------------
SRC_DIR   := src
BUILD_DIR := build
ISO_DIR   := $(BUILD_DIR)/isodir

BOOT_OBJ   := $(BUILD_DIR)/boot.o
KERNEL_OBJ := $(BUILD_DIR)/kernel.o
OBJS       := $(BOOT_OBJ) $(KERNEL_OBJ)

KERNEL_ELF   := $(BUILD_DIR)/kernel.elf
MODULE_BIN   := $(ISO_DIR)/boot/amc-os.bin
GRUB_CFG_DIR := $(ISO_DIR)/boot/grub
GRUB_CFG     := $(GRUB_CFG_DIR)/grub.cfg

ISO := $(BUILD_DIR)/amc-os.iso

# --- Hedefler ----------------------------------------------------------
.PHONY: all run qemu vbox clean check-tools

all: iso

# 1) Assembly -> ELF32 nesne
$(BOOT_OBJ): $(SRC_DIR)/boot.asm | $(BUILD_DIR)
	@echo "[NASM] $< -> $@"
	$(NASM) $(NASMFLAGS) $< -o $@

# 2) C -> freestanding nesne
$(KERNEL_OBJ): $(SRC_DIR)/kernel.c | $(BUILD_DIR)
	@echo "[CC ] $< -> $@"
	$(CC) $(CFLAGS) -c $< -o $@

# 3) Link -> Multiboot ELF (GRUB dogrudan bu ELF'i boot edebilir)
$(KERNEL_ELF): $(OBJS) $(SRC_DIR)/linker.ld | $(BUILD_DIR)
	@echo "[LD ] $(OBJS) -> $@"
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# 4) Yedek moduler format (bazi GRUB kurulumlari 'module' olarak raw bin ister)
$(MODULE_BIN): $(KERNEL_ELF) | $(BUILD_DIR)
	@mkdir -p $(ISO_DIR)/boot
	@echo "[OBJCOPY] $< -> $@"
	$(OBJCOPY) -O binary $< $@

# 5) grub.cfg otomatik yazilir
$(GRUB_CFG): | $(BUILD_DIR)
	@mkdir -p $(GRUB_CFG_DIR)
	@echo "[GRUB] $(GRUB_CFG) olusturuluyor"
	@printf '%s\n' \
'set timeout=5' \
'set default=0' \
'menuentry "AMC-OS v0.1" {' \
'    multiboot /boot/kernel.elf' \
'    boot' \
'}' > $@

# 6) ISO uretimi
iso: $(KERNEL_ELF) $(GRUB_CFG) | check-tools
	@mkdir -p $(ISO_DIR)/boot
	@cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	@echo "[ISO] $(ISO) uretiliyor..."
	$(GRUB_MKRESCUE) -o $(ISO) $(ISO_DIR)
	@ls -lh $(ISO)

check-tools:
	@command -v $(GRUB_MKRESCUE) >/dev/null 2>&1 || { \
	  echo "EKSIK PAKET: grub-mkrescue yok."; \
	  echo "Debian/Ubuntu: sudo apt install -y grub-pc-bin grub-common xorriso mtools"; \
	  echo "Arch:          sudo pacman -S --needed grub mtools xorriso"; \
	  echo "Fedora:        sudo dnf install -y grub2-pc-modules grub2-tools xorriso mtools"; \
	  exit 1; }
	@command -v nasm >/dev/null 2>&1 || { echo "EKSIK: nasm"; exit 1; }

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# 7) QEMU'da calistir
run: iso
	qemu-system-i386 -cdrom $(ISO) -boot d -m 256M -serial stdio

qemu: run

# 8) VirtualBox notu (VMDK uretimi opsiyonel)
vbox: iso
	@echo "VirtualBox: yeni makine (Type: Other/Version: DOS veya Linux 32-bit)"
	@echo "            Optical Drive -> $(abspath $(ISO))"
	@command -v VBoxManage >/dev/null 2>&1 && \
	  VBoxManage convertfromraw --format VMDK $(ISO) $(BUILD_DIR)/amc-os.vmdk || true

clean:
	rm -rf $(BUILD_DIR)
