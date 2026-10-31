; ============================================================
; amc-os - boot.asm (Adim 4 guncel hali)
; GDT + IDT + Protected Mode + kernel_main cagrisi
; IDT: INT 0x21 (IRQ1 klavye) -> keyboard_irq_handler (C fonksiyonu)
; Derleme : nasm -f bin boot.asm -o boot.bin
; ============================================================

[bits 16]
[org 0x7c00]

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    ; Ekran temizle (80x25 metin modu)
    mov ah, 0x00
    mov al, 0x03
    int 0x10

    ; A20 hattini ac
    mov ax, 0x2401
    int 0x15
    jc .a20_fast
    jmp .a20_done
.a20_fast:
    in al, 0x92
    or al, 2
    out 0x92, al
.a20_done:

    ; Kernel'i diskten RAM'e yukle (CHS: silindir 0, baslik 0, sektor 2+)
    mov bx, 0x1000        ; hedef bellek adresi (kernel buraya iniyor)
    mov cx, 2             ; CHS: silindir=0, sektor=2 (boot sonrasi ilk sektor)
    mov dx, 0x0000        ; surucu 0 (ilk floppy), baslik 0
.read_kernel:
    push cx
    push bx
    mov al, 1             ; 1 sektor oku
    int 0x13              ; BIOS disk servisi AH=02
    pop bx
    pop cx
    jc read_error
    add bx, 512           ; hedefe 512 byte ekle
    inc cl                ; sonraki sektor
    cmp cl, 19            ; Adim-4 kerneli 18 sektoru gecmez
    jb  .read_kernel

    jmp enter_pm

read_error:
    mov si, msg_err
    call print_string_real
err_halt:
    hlt
    jmp err_halt

print_string_real:
    lodsb
    cmp al, 0
    je .done
    mov ah, 0x0e
    int 0x10
    jmp print_string_real
.done:
    ret

msg_err db "Disk okuma hatasi!", 10, 13, 0

enter_pm:
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp code_sel:.pm_start

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
[bits 32]
.pm_start:
    mov ax, data_sel
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; ---- Ekrani temizle ----
    mov edi, 0xB8000
    mov ecx, 2000
    mov ax, 0x0F20
.clear_loop:
    stosw
    loop .clear_loop

    ; ---- IDT kurulumu kernel.c'de yapilir (init_interrupts) ----
    ; Bootloader sadece GDT'yi kurar ve PIC maskesi kapali oldugu icin
    ; bu noktada kesme gelmez. kernel_main, sti demeden once kendi
    ; IDT'sini hazirlar.

    ; ---- kernel_main'i cagir ----
    call 0x1000

.halt:
    hlt
    jmp .halt

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
gdt_start:
.null_desc:
    dd 0x0
    dd 0x0
.code_desc:                 ; 0x08
    dd 0x0000FFFF
    dd 0x00CF9A00
.data_desc:                 ; 0x10
    dd 0x0000FFFF
    dd 0x00CF9200
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

code_sel equ 0x08
data_sel equ 0x10

times 510 - ($ - $$) db 0
dw 0xaa55
