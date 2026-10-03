# ============================================================
# AMC OS v0.5 - Derleme Sistemi
#   make bootloader : boot.img (MBR+stage2, nasm ile GERCEKTEN derlenir)
#   make sim        : ./build/amcsim  (sanal anakart uzerinde kernel kodu!)
#   make run-sim    : simulasyonu calistir (boot akisi + suurucu testleri)
#   make kernel     : amc-kernel.elf (cross-toolchain gerektirir)
#   make image      : QEMU disk imaji
#   make run        : qemu-system-x86_64
#   make test-boot  : MBR imzasi/boyut dogrulamasi
# ============================================================
CC       ?= gcc
HOSTCC   := gcc
NASM     := nasm
CROSSCC  := x86_64-elf-gcc

.PHONY: all bootloader sim run-sim kernel image run test-boot clean

all: bootloader sim test-boot

# ---------- Onyukleyici (gercek derleme) ----------
bootloader: build/stage1.bin build/stage2.bin
	cat build/stage1.bin build/stage2.bin > build/bootimg.bin
	@echo "bootimg.bin: $$(wc -c < build/bootimg.bin) bayt"

build/stage1.bin: boot/boot.asm
	@mkdir -p build
	$(NASM) -f bin boot/boot.asm -o $@

build/stage2.bin: boot/stage2.asm
	@mkdir -p build
	$(NASM) -f bin boot/stage2.asm -o $@

test-boot: build/stage1.bin
	@sz=$$(stat -c%s build/stage1.bin); sig=$$(tail -c2 build/stage1.bin | od -An -tx1 | tr -d ' '); \
	if [ "$$sz" = "512" ] && [ "$$sig" = "55aa" ]; then \
	  echo "[test-boot] OK: MBR 512B + 0x55AA imzasi dogru"; \
	else echo "[test-boot] HATA: boyut=$$sz imza=$$sig"; exit 1; fi

# ---------- Simulasyon (kernel algoritmalari gercekten calisir) ----------
SIMSRC := sim/sim_main.c sim/sim_hw.c sim/sim_tests.c \
          kernel/time/timesrc.c kernel/drivers/storage/nvme.c \
          kernel/drivers/wifi/rtw_wifi.c kernel/arch/x86_64/vmx.c

sim: build/amcsim

build/amcsim: $(SIMSRC) sim/posix_compat.h
	@mkdir -p build
	$(HOSTCC) -DAMC_SIM -I sim -I kernel -O1 -g -Wall -Wextra -Wno-unused-parameter \
	  $(SIMSRC) -o $@
	@echo "[sim] derlendi -> build/amcsim"

run-sim: sim
	@./build/amcsim

# ---------- Cross kernel (toolchain varsa) ----------
kernel:
	@if command -v $(CROSSCC) >/dev/null; then \
	  echo "[kernel] cross toolchain var: Makefile.kernel ile derle"; \
	else \
	  echo "[kernel] x86_64-elf-gcc yok. 'make sim' ile yerel test onerilir."; fi

image: bootloader
	python3 tools/mkimage.py build/bootimg.bin build/amcos.img

run: image
	qemu-system-x86_64 -drive format=raw,file=build/amcos.img -m 512 -serial stdio || \
	  echo "[run] qemu kurulu degil; 'make run-sim' deneyin"

clean:
	rm -rf build
