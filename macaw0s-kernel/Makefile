# ============================================================================
# MacawOS Kernel — x86_64 build system (NASM + GCC freestanding)
# Targets: all | run | iso | clean
# Toolchain: nasm, x86_64-elf-gcc (or host gcc with CROSS= for testing), ld
# ============================================================================

CROSS     ?= x86_64-elf-
CC        := $(CROSS)gcc
LD        := $(CROSS)ld
NASM      := nasm

WARNINGS  := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
             -Wold-style-definition -Wno-unused-parameter -Werror
CFLAGS    := -std=gnu11 -O3 -g $(WARNINGS) \
             -ffreestanding -mno-red-zone -m64 -mcmodel=kernel \
             -nostdlib -fno-pic -fno-pie -fno-stack-protector \
             -fno-builtin -fno-asynchronous-unwind-tables \
             -I$(KERNEL_DIR)/include
LDFLAGS   := -n -T $(BOOT_DIR)/linker.ld --build-id=none \
             -z max-page-size=0x1000 --no-warn-rwx-segments

BOOT_DIR   := boot
KERNEL_DIR := kernel
BUILD_DIR  := build

ASM_SOURCES := $(wildcard $(BOOT_DIR)/*.asm) $(shell find $(KERNEL_DIR) -name '*.asm')
C_SOURCES   := $(shell find $(KERNEL_DIR) -name '*.c')

ASM_OBJECTS := $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
C_OBJECTS   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
OBJECTS     := $(ASM_OBJECTS) $(C_OBJECTS)

KERNEL_ELF  := $(BUILD_DIR)/macaw-os.bin

.PHONY: all run iso clean

all: $(KERNEL_ELF)

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJECTS) $(BOOT_DIR)/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	@echo ">> Linked $@ ($(shell stat -c%s $@ 2>/dev/null || echo ?) bytes)"

QEMU_FLAGS := -machine q35 -m 1G -serial stdio -no-reboot -no-shutdown \
              -device isa-debug-exit,iobase=0xf4,iosize=0x04

run: $(KERNEL_ELF)
	qemu-system-x86_64 $(QEMU_FLAGS) \
	  -kernel $(KERNEL_ELF) \
	  -drive if=none,id=nvm,format=raw,file=nvme.img,size=64 \
	  -device nvme,serial=deadbeef,drive=nvm \
	  -device rtl8139,netdev=net0 -netdev user,id=net0

iso: $(KERNEL_ELF)
	@mkdir -p isodir/boot/grub
	cp $(KERNEL_ELF) isodir/boot/macaw-os.bin
	printf 'set timeout=0\nset default=0\nmenuentry "MacawOS" {\nmultiboot2 /boot/macaw-os.bin\nboot\n}\n' > isodir/boot/grub/grub.cfg
	grub-mkrescue -o macaw-os.iso isodir 2>/dev/null || \
	  echo "grub-mkrescue unavailable; ELF still boots via qemu -kernel"

clean:
	rm -rf $(BUILD_DIR) isodir macaw-os.iso
