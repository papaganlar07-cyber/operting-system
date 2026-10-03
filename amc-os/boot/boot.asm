; ============================================================
; AMC OS - Bootloader (Boot Sector, 512 bayt, MBR uyumlu)
; Gerçekçi çok aşamalı önyükleme:
;   Aşama 1 : BIOS 0x7C00 boot sektörü -> diskten stage2'yi yükler
;   Aşama 2 : A20 kapısını açar, GDT kurar, uzun mod (long mode) dener,
;             korumalı moda geçer ve kernel'i belleğe yükler
; Not: Bu proje eğitim/araştırma amaçlı bir OS çekirdeğidir.
;      QEMU üzerinde çalışacak şekilde tasarlanmıştır.
; ============================================================

[BITS 16]
[ORG 0x7C00]

start16:
    cli                     ; kesmeleri kapat
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00          ; yığın boot sektörünün altına doğru büyüsün

    ; Ekranı temizle (BIOS INT 0x10, AH=00, AL=03 -> 80x25 metin)
    mov ah, 0x00
    mov al, 0x03
    int 0x10

    ; Hoş geldiniz mesajı
    mov si, msg_boot
    call print_string

    ; ---- Stage2'yi diskten oku (LBA ilk deneme) ----
    ; AMC FS süperbloku stage2'nin hemen ardından gelir;
    ; stage2 = 2 sektör (1024 bayt), disk offset 2. sektörden itibaren.
    mov bx, 0x1000          ; stage2 -> 0x1000:0 (segment 0x1000)
    mov es, bx
    xor bx, bx
    mov ah, 0x02            ; BIOS disk read
    mov al, 8               ; 8 sektör oku (kernel imaj alanı)
    mov ch, 0               ; silindir 0
    mov cl, 2               ; sektör 2'den başla (stage1 sektör 1)
    mov dh, 0               ; head 0
    mov dl, 0x80            ; ilk sabit disk / USB sürücüsü
    int 0x13
    jc  .disk_error

    jmp 0x1000:0x0000       ; stage2'ye atla

.disk_error:
    mov si, msg_diskerr
    call print_string
    halt_loop:
        hlt
        jmp halt_loop

; ---- Yardımcı: BIOS ile karakter dizisi yazdır ----
print_string:
    lodsb
    or  al, al
    jz  .done
    mov ah, 0x0E
    mov bl, 0x0F
    int 0x10
    jmp print_string
.done:
    ret

msg_boot:   db "AMC OS Loader v0.4 (stage1)", 13, 10, 0
msg_diskerr:db "HATA: Disk/USB okunamadi! BIOS 0x13 hatasi.", 13, 10, 0

; ---- Boot imzası (tam 512 bayt) ----
times 510-($-$$) db 0
dw 0xAA55
