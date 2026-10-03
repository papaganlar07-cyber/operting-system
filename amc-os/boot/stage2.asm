; ============================================================
; AMC OS - Bootloader Stage 2 (korumalı mod / long mode geçişi)
; Görevler:
;   1. A20 adres hattını aç (hızlı yol: port 0x92)
;   2. GDT hazırla (kod 64-bit, veri, kod 32-bit segmentleri)
;   3. PADE (PAE) sayfa tabloları -> uzun mod (long mode) kontrolü
;      CPU uzun mod desteklemiyorsa 32-bit korumalı moda düşer
;   4. Kernel imajını 0x200000 (2MB) adresine taşı ve jump
; ============================================================

[BITS 16]
[ORG 0x1000]

stage2_start:
    cli
    ; --- Mesaj ---
    mov si, msg_s2
    call print_string16

    ; --- A20 kapısını aç (Fast A20 via port 0x92) ---
    in  al, 0x92
    or  al, 2
    out 0x92, al
    ; Kısa bekleme döngüsü (A20 stabilizasyonu)
    mov cx, 0xFFFF
.a20_wait: dec cx
    jnz .a20_wait

    ; --- Uzun mod destek kontrolü (CPUID ext leaf 0x80000001, bit 29) ---
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb  enter_protected32       ; CPUID ext yok -> 32-bit moda düş
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)         ; LM bit
    jz  enter_protected32

    ; ================= LONG MODE YOLU =================
    ; PML4 -> PDPT -> PD -> PT (ilk 1GB identity mapping, 2MB büyük sayfa)
    mov dword [pml4], pdpt
    mov dword [pdpt], pd
    ; PD: 512 x 2MB'lık büyük sayfa girişleri (0 .. 1GB)
    mov edi, pd
    mov ecx, 0
.build_pd:
    mov eax, ecx
    shl eax, 21                 ; ecx * 2MB
    or  eax, 0x83               ; Present + R/W + Huge sayfa
    mov [edi], eax
    mov dword [edi+4], 0
    add edi, 8
    inc ecx
    cmp ecx, 512
    jb  .build_pd

    ; CR4.PAE = 1
    mov eax, cr4
    or  eax, 1 << 5
    mov cr4, eax

    ; CR3 = PML4
    mov eax, pml4
    mov cr3, eax

    ; EFER.LME = 1 (MSR 0xC0000001, bit 8)
    mov ecx, 0xC0000001
    rdmsr
    or  eax, 1 << 8
    wrmsr

    ; CR0.PG | CR0.WP | CR0.PE
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 16) | (1 << 0)
    mov cr0, eax

    lgdt [gdt64.pointer]
    mov eax, 0x08
jmp long_mode_entry

enter_protected32:
    ; ================= 32-BIT KORUMALI MOD YOLU =================
    lgdt [gdt32.pointer]
    mov eax, cr0
    or  eax, 1                  ; PE biti
    mov cr0, eax
    mov eax, 0x08
jmp protected_mode_entry

; ---------- 16-bit yazdırma yardımcısı ----------
print_string16:
    lodsb
    or al, al
    jz .done16
    mov ah, 0x0E
    int 0x10
    jmp print_string16
.done16: ret

[BITS 64]
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; Kernel'i 0x200000'a kopyala (stage1 diskten 0x90000'e yüklemişti)
    mov rsi, 0x90000
    mov rdi, 0x200000
    mov rcx, 512 * 1024         ; en fazla 512KB kernel
    rep movsb

    jmp 0x200000                ; KERNEL GİRİŞ NOKTASI

[BITS 32]
protected_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ax, ss
    ; Kernel'i taşı ve atla
    mov esi, 0x90000
    mov edi, 0x200000
    mov ecx, 512 * 1024
    rep movsb
    jmp 0x200000

; ================= GDT (64-bit) =================
align 8
gdt64:
    dq 0 ; null
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)   ; L=1, P=1, D=0, S=1, Type=Code
    dq (1<<41) | (1<<44) | (1<<47)             ; W=1, P=1, S=1, Data
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

; ================= GDT (32-bit yedek) =================
align 4
gdt32:
    dd 0,0                      ; null
    dd 0x00CF9800,0             ; 32-bit kod
    dd 0x00CF9200,0             ; 32-bit veri
.pointer:
    dw $ - gdt32 - 1
    dd gdt32

msg_s2: db "AMC OS stage2: A20 acildi, mod algilaniyor...", 13, 10, 0

; ---- Sayfa tabloları (BSS benzeri, stage2 sonunda) ----
align 4096
pml4: times 512 dq 0
pdpt: times 512 dq 0
pd:   times 512 dq 0

; stage2 boyutu ~16KB (sayfa tablolar dahil); MBR STAGE2_SECTORS=32 yeter
