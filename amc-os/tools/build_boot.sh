#!/bin/sh
# Bootloader derleme yardimcisi: nasm ile stage1+stage2 -> bootimg.bin
set -e
nasm -f bin boot/boot.asm   -o build/stage1.bin
nasm -f bin boot/stage2.asm -o build/stage2.bin
mkdir -p build; cat build/stage1.bin build/stage2.bin > build/bootimg.bin
echo "bootimg.bin hazir: $(wc -c < build/bootimg.bin) bayt"
