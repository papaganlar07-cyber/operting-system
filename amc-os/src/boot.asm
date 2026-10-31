; boot.asm - AMC-OS: Multiboot 1 (GRUB) uyumlu, 32-bit Protected Mode giris
bits 32

section .multiboot_header
align 4
MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ 0
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)
dd MB_MAGIC
dd MB_FLAGS
dd MB_CHECKSUM

section .text
global _start
extern kernel_main

_start:
    ; GRUB bizi zaten 32-bit korumali modda, paging kapali,
    ; CS/DS/ES/SS = duz 4GB segmentlerle baslatir.
    cli                     ; kesmeleri kapat (kernel IDT kurana kadar)
    xor ebp, ebp            ; stack trace zincirini temizle

    ; --- Ekrani temizle: VGA text buffer 0xB8000, 80*25*2 byte ---
    mov edi, 0xB8000
    mov ecx, 80 * 25 / 2    ; 1000 kez 'stosd' -> 4 byte yazar = 8000 byte
    xor eax, eax            ; bosluk (0x20) + siyah-on-beyaz renk (0x07)
    mov ax, 0x0720
    cld
    rep stosd

    call kernel_main        ; C cekirdegin tetikle

.hang:
    hlt
    jmp .hang               ; kernel geri donerse CPU'yu duraklat
