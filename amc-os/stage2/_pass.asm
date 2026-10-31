; =====================================================================
;  AMC-OS / Adim 3 : "win" komutu -> VGA Mode 13h masaustu
;  Onceki adimdaki bootloader'in TAM ve GUNCEL halidir (tek parca).
;
;  Akis:
;    1) MS-DOS tarzi siyah ekran + ASCII-art "AMC-OS" logosu
;    2) "Komut satiri yuklendi. Masaustune gecmek icin 'win' yazin."
;    3) "AMC-OS>" prompt'u, int 0x16 ile klavye okuma (yankalama+backspace)
;    4) Enter'a basilinca girilen satir komut tablosunda aranir
;    5) "win" -> gercek int 0x10 ile Mode 13h (320x200, 256 renk):
;         - camgobe mavi arka plan
;         - boydan boya Windows 95 gri gorev cubugu (3D kabartma kenarli)
;         - sol kosede kare buton icinde "BASLAT" yazisi
;
;  Mimari not: Kod tek 512 bayta sigmaz; imaj 2 sektordur:
;    SEKTOR 1 : boot kodu + handler'lar (0x7C00'de calisir)
;    SEKTOR 2 : YALNIZCA VERI (logolar, mesajlar, font, komut tablosu)
;  Bootloader kendi diskinden 2. sektoru int 0x13 CHS ile 0x8000'e yukler.
;  Etiket adresleri NASM org=0x7c00 tabanlidir; 0x7E00 altindaki her sembol
;  iki katina carpilarak RAM'deki dogru fiziksel adresini verir
;  (org X -> dosya ofseti X-0x7c00; 0x8000 + (X-0x7E00) = 2*(X-0x7E00)+0x7E00...
;  yerine en sade yol: LABEL'i D macro ile tasiyoruz; bkz. asagida).
; =====================================================================

[bits 16]
[org 0x7c00]

; ---------------- sabitler ----------------
DATA_BASE   equ 0x8000      ; 2. sektorun yuklendigi fiziksel adres
SCREEN_W    equ 320
SCREEN_H    equ 200
BAR_H       equ 24          ; gorev cubugu yuksekligi (piksel)
VGA13       equ 0xA000      ; Mode 13h video bellek segmenti

COLOR_BG    equ 15          ; palet 15 = camgobe (cyan)
COLOR_BAR   equ 8           ; palet 8  = gri (Win95 govde)
COLOR_HI    equ 7           ; palet 7  = beyaz (kabartma ust/sol kenar)
COLOR_LO    equ 4           ; palet 4  = koyu gri (golge alt/sag kenar)

STRIDE      equ 32          ; komut tablosu kayit genisligi (byte)
CMD_MAXLEN  equ 15          ; komut basina maks karakter (+NUL)

; Sektor-2 etiketlerini RAM adresine tasimanin en saglam yolu:
; NASM'de org tabani 0x7c00 oldugundan, 0x7e00 sonrasi bir etiketin
; degeri L'dir; dosya-ofseti = L - 0x7c00; RAM adresi = 0x8000 + (L - 0x7e00).
; Bunu derleme-zamaninda yapan makro:
%define DRAM(label) (DATA_BASE + ((label) - 0x7e00))

; ============================ BASLANGIC ==============================
start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti

    ; ---------- A20 hatti ac ----------
    mov ax, 0x2401
    int 0x15
    jc .a20_port
    jmp .a20_done
.a20_port:
    in al, 0x92
    or al, 2
    out 0x92, al
.a20_done:

    ; ---------- Ekran: 80x25 siyah text mod ----------
    mov ah, 0x00
    mov al, 0x03
    int 0x10

    ; ---------- Kendi diskimizden 2. sektoru 0x8000'e yukle ----------
    ; Surucu numarasi DL'de gelir (floppy 0x00, ilk HDD 0x80).
    ; Imaji floppy olarak boot ettigimizden CHS: cyl 0, head 0, sector 2.
    mov ah, 0x02            ; DISK READ
    mov al, 1               ; 1 sector
    mov ch, 0
    mov cl, 2               ; CHS 1-tabanli: ikinci sektor
    mov dh, 0
    mov bx, DATA_BASE       ; ES:BX hedefi; ES=0 -> fiziksel 0x8000
    int 0x13
    jc load_fail

    ; ---------- ASCII ART LOGO ("AMC-OS") ----------
    ; logo_table: word pointer dizisi; girdiler DOSYA-ofseti olarak degil,
    ; dogrudan RAM adresi olarak saklanir (dw DRAM(line_a1) vb.).
    mov si, DRAM(logo_table)
    mov cx, 1                       ; ekranda ilk satir
.loop_logo:
    mov di, [si]                    ; di = bu satirin RAM adresi
    test di, di
    jz .done_logo
    push cx
    push si
    push di
    mov si, di                      ; SI = string adresi
    pop di
    call print_at_str               ; CX=satir, DS:SI=string
    pop si
    pop cx
    add si, 2
    inc cx
    jmp .loop_logo
.done_logo:

    ; ---------- bilgi satiri ----------
    mov si, DRAM(msg_info)
    call print_string
    call newline

    ; ---------- komut satiri dongusu ----------
cmd_loop:
    mov si, DRAM(prompt)
    call print_string
    xor di, di                      ; input_buf indeksi

.read_key:
    xor ax, ax
    int 0x16                        ; AL = ASCII (bekleyerek okur)
    cmp al, 0x0D
    je .enter_pressed
    cmp al, 0x08
    je .backspace
    cmp al, 0x20
    jb .read_key                    ; kontrol karakterlerini yoksay
    cmp di, CMD_MAXLEN
    jae .read_key
    mov byte [input_buf + di], al
    inc di
    call putchar_real
    jmp .read_key

.backspace:
    test di, di
    jz .read_key
    dec di
    mov byte [input_buf + di], 0
    mov al, 0x08
    call putchar_real
    mov al, 0x20
    call putchar_real
    mov al, 0x08
    call putchar_real
    jmp .read_key

.enter_pressed:
    mov byte [input_buf + di], 0
    call newline

    ; ---------- komutu tablodan ara ----------
    ; DI -> BX'e tasi (16-bit modda [bx+di] gecerli, [si+di] degil)
    mov bx, DRAM(cmd_table)
.lookup_entry:
    mov byte al, [bx]
    test al, al
    jz unknown_cmd                 ; isim ilk byte'i 0 -> tablo bitti
    xor di, di
.cmp_char:
    mov al, [input_buf + di]
    mov bl, [bx + di]
    cmp al, bl
    jne .next_entry
    test al, al
    jz .found                       ; iki taraf da NUL -> eslesti
    inc di
    cmp di, CMD_MAXLEN
    jb .cmp_char
.next_entry:
    add bx, STRIDE
    jmp .lookup_entry

.found:
    mov ax, [bx + STRIDE - 2]       ; handler ofseti (sektor ici mutlak)
    test ax, ax
    jz unknown_cmd
    jmp ax                          ; handler'a git

unknown_cmd:
    mov si, DRAM(msg_unknown)
    call print_string
    call newline
    jmp cmd_loop

load_fail:
    mov si, err_txt                 ; bu metin 1. sektorde durur
    call print_string
here_halt:
    hlt
    jmp here_halt

err_txt: db "HATA: veri sectoru okunamadi!", 0

; ==================== YARDIMCI FONSIYONLAR (Real Mode) ====================

; DS:SI -> 0 ile biten stringi mevcut imlece yazdirir
print_string:
    lodsb
    cmp al, 0
    je .ps_done
    call putchar_real
    jmp print_string
.ps_done:
    ret

; AL -> BIOS teletype (parlak gri-on-siyah, DOS havasi)
putchar_real:
    pusha
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
    int 0x10
    popa
    ret

; CRLF
newline:
    pusha
    mov al, 0x0A
    call putchar_real
    mov al, 0x0D
    call putchar_real
    popa
    ret

; CX = satir, DS:SI = string -> satrin basina parlak beyaz yaz
print_at_str:
    pusha
    mov dh, cl
    xor ch, ch
    xor dl, dl              ; sutun 0
    mov ah, 0x02
    mov bh, 0
    int 0x10
    popa
.at_loop:
    mov al, [ds:si]
    inc si
    test al, al
    jz .at_done
    pusha
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x0F            ; parlak beyaz
    int 0x10
    popa
    jmp .at_loop
.at_done:
    ret

; ~300 ms gecikme (int 0x15/AH=0x86)
delay_300ms:
    pusha
    mov cx, 0
    mov dx, 0xB717
    mov ah, 0x86
    int 0x15
    popa
    ret

; ======================= EYLEM HANDLER'LARI =======================

; --- handler: "win" -> Mode 13h masaustu ---
handle_win:
    mov si, DRAM(msg_entering)
    call print_string
    call delay_300ms

    ; Mode 13h ayarla (Real Mode'dayiz; int 0x10 sorunsuz calisir)
    mov ah, 0x00
    mov al, 0x13
    int 0x10

    ; ES = video segmenti (0xA000); DI piksel sayaci
    mov ax, VGA13
    mov es, ax

    ; ---- 1) Arka plan: camgobe, gorev cubugu haric tum ekran ----
    xor di, di
    mov cx, SCREEN_H - BAR_H
.fill_rows:
    push cx
    mov cx, SCREEN_W / 2
    mov ax, COLOR_BG * 0x0101
    rep stosw
    pop cx
    loop .fill_rows

    ; ---- 2) Gorev cubugu govdesi (gri) ----
    mov di, (SCREEN_H - BAR_H) * SCREEN_W
    mov cx, SCREEN_W * (BAR_H - 2) / 2
    mov ax, COLOR_BAR * 0x0101
    rep stosw

    ; ---- 3) Kabartma kenarlar: ustte beyaz cizgi, altta koyu golge ----
    mov di, (SCREEN_H - BAR_H) * SCREEN_W
    mov cx, SCREEN_W / 2
    mov ax, COLOR_HI * 0x0101
    rep stosw
    mov di, (SCREEN_H - 1) * SCREEN_W
    mov cx, SCREEN_W / 2
    mov ax, COLOR_LO * 0x0101
    rep stosw

    ; ---- 4) BASLAT butonu: x 4..58, y (SCREEN_H-BAR_H+4)..(SCREEN_H-5) ----
    ; ici gri; ust+sol kenar beyaz; alt+sag kenar koyu gri (Win95 kabartma)
    mov bx, SCREEN_H - BAR_H + 4
.btn_row:
    push bx
    mov cx, 4
.btn_col:
    mov al, COLOR_BAR
    cmp bx, SCREEN_H - BAR_H + 4
    je .btn_light
    cmp cx, 4
    je .btn_light
    cmp bx, SCREEN_H - 5
    je .btn_dark
    cmp cx, 58
    je .btn_dark
    jmp .btn_write
.btn_light:
    mov al, COLOR_HI
    jmp .btn_write
.btn_dark:
    mov al, COLOR_LO
.btn_write:
    mov di, bx
    imul di, di, SCREEN_W
    add di, cx
    mov byte [es:di], al
    inc cx
    cmp cx, 59
    jbe .btn_col
    pop bx
    inc bx
    cmp bx, SCREEN_H - 5
    jbe .btn_row

    ; ---- 5) Buton icine "BASLAT" (5x7 bitmap font, siyah) ----
    ; blit_text girdileri: DS:SI=string, BX=x, CX=y, BH=reng, ES=video seg
    mov si, DRAM(txt_baslat)
    mov bx, 12                          ; x
    mov cx, SCREEN_H - BAR_H + 9        ; y
    xor bh, bh                          ; renk = siyah
    call blit_text

.gui_halt:
    hlt
    jmp .gui_halt

; --- handler: "help" -> komutlari listele ---
handle_help:
    mov si, DRAM(msg_help1)
    call print_string
    call newline
    mov bx, DRAM(cmd_table)
.help_walk:
    mov byte al, [bx]
    test al, al
    jz .help_end
    xor di, di
.help_name:
    mov al, [bx + di]
    test al, al
    jz .help_nl
    call putchar_real
    inc di
    jmp .help_name
.help_nl:
    call newline
    add bx, STRIDE
    jmp .help_walk
.help_end:
    jmp cmd_loop                ; handler'dan ana CLI dongusune don (global etiket)

; --- handler: "reboot" ---
handle_reboot:
    mov si, DRAM(msg_reboot)
    call print_string
    call delay_300ms
    xor ax, ax
    mov es, ax
    jmp 0xffff:0x0000                   ; BIOS reset vectoru

; --- DS:SI stringini (BX=x, CX=y) noktasina 5x7 fontla cizer, renk BH ---
; Font: DRAM(font5x7), index = harf-'A'. Harf genisligi 5px + 1px bosluk.
; Girdilerin global kopyalari kullanilir ki pusha/popa sirasinda
; registerler ezilmesin.
blit_text:
.bt_char:
    mov al, [si]
    test al, al
    jz .bt_done
    inc si
    cmp al, ' '
    je .bt_advance
    ; glif adresini hesapla
    sub al, 'A'
    movzx eax, al
    imul eax, eax, 5
    mov bp, DRAM(font5x7)
    add bp, ax
    ; girdileri global kopyalara al
    mov [cur_x], bx
    mov [cur_y], cx
    mov [cur_c], bh
    xor di, di                  ; col = 0
.bt_col:
    mov al, [bp]                ; column bitmap (bit0 = en ust piksel)
    inc bp
    mov [col_bm], al
    xor ch, ch                  ; row = 0
.bt_row:
    mov al, [col_bm]            ; bt'in bellek+register girdi kombinasyonunu
    mov cl, ch                  ; shr sabit sayilari destekler; CL ile kaydir
    shr al, cl
    and al, 1
    jz .bt_row_next
    pusha
    ; hedef piksel = (cur_y + row)*320 + (cur_x + col)
    mov ax, [cur_y]
    add ax, cx                  ; y + row
    mov bx, ax
    imul bx, bx, SCREEN_W
    add bx, [cur_x]
    add bx, di                  ; x + col
    mov al, [cur_c]
    mov byte [es:bx], al
    popa
.bt_row_next:
    inc ch
    cmp ch, 7
    jb .bt_row
    inc di
    cmp di, 5
    jb .bt_col
    ; sutunlar bitti -> sonraki harfe gec: cur_x += 6
    mov bx, [cur_x]
    add bx, 6
    jmp .bt_char
.bt_advance:
    add bx, 6
    jmp .bt_char
.bt_done:
    ret

; ---------------- sektor ici global yardimcilar ----------------
col_bm:     db 0
cur_x:      dw 0
cur_y:      dw 0
cur_c:      db 0
input_buf:  times 16 db 0

TIMES 4096 db 0
dw 0xaa55

; =====================================================================
;  SEKTOR 2 : YALNIZCA VERI (dosya ofseti 512 -> RAM 0x8000)
;  Bu bolumdeki hicbir sembol KOD icermeyen ham veridir; erisimler
;  DRAM(makrosu) ile yapilir.
; =====================================================================

msg_info:     db "Komut satiri yuklendi. Masaustune gecmek icin 'win' yazin.", 0
prompt:       db "AMC-OS> ", 0
msg_unknown:  db "Bilinmeyen komut. 'help' yazabilirsiniz.", 0
msg_entering: db "Masaustune geciliyor...", 0
msg_help1:    db "Komutlar:", 0
msg_reboot:   db "Yeniden baslatiliyor...", 0

txt_baslat: db "BASLAT", 0

; ---- 5x7 buyuk-harf fontu (A-Z sirali; index = harf-'A') ----
font5x7:
    db 0x7C,0x12,0x11,0x12,0x7C   ; A
    db 0x7F,0x49,0x49,0x49,0x36   ; B
    db 0x3E,0x41,0x41,0x42,0x3C   ; C
    db 0x7F,0x41,0x41,0x22,0x1C   ; D
    db 0x7F,0x49,0x49,0x49,0x41   ; E
    db 0x7F,0x09,0x09,0x09,0x01   ; F
    db 0x3E,0x41,0x49,0x4A,0x3C   ; G
    db 0x7F,0x08,0x08,0x08,0x7F   ; H
    db 0x41,0x7F,0x41,0x00,0x00   ; I
    db 0x20,0x40,0x41,0x3F,0x01   ; J
    db 0x7F,0x08,0x14,0x22,0x41   ; K
    db 0x7F,0x40,0x40,0x40,0x40   ; L
    db 0x7F,0x02,0x0C,0x02,0x7F   ; M
    db 0x7F,0x04,0x08,0x10,0x7F   ; N
    db 0x3E,0x41,0x41,0x41,0x3E   ; O
    db 0x7F,0x09,0x09,0x09,0x06   ; P
    db 0x3E,0x41,0x51,0x21,0x5E   ; Q
    db 0x7F,0x09,0x19,0x29,0x46   ; R
    db 0x46,0x49,0x49,0x49,0x31   ; S
    db 0x41,0x41,0x7F,0x41,0x41   ; T
    db 0x3F,0x40,0x40,0x40,0x3F   ; U
    db 0x1F,0x20,0x40,0x20,0x1F   ; V
    db 0x3F,0x40,0x38,0x40,0x3F   ; W
    db 0x63,0x14,0x08,0x14,0x63   ; X
    db 0x07,0x08,0x70,0x08,0x07   ; Y
    db 0x61,0x51,0x49,0x45,0x43   ; Z

; ---- ASCII-art logo satirlari ----
line_a1: db "    _     __  __ ____   ____ ", 0
line_a2: db "   / \   |  \/  |  _ \ / ___| ", 0
line_a3: db "  / _ \  | |\/| | |_) |\___ \ ", 0
line_a4: db " / ___ \ | |  | |  _ <  ___) |", 0
line_a5: db "/_/   \_\|_|  |_|_| \_\|____/  - OS", 0

; Logo pointer tablosu: her girdi DOGRUDAN RAM adresi (derleme-zamani DRAM)
logo_table:
    dw DRAM(line_a1)
    dw DRAM(line_a2)
    dw DRAM(line_a3)
    dw DRAM(line_a4)
    dw DRAM(line_a5)
    dw 0

; ---- komut kontrol tablosu (STRIDE=32) ----
; Format: [30B isim+NUL+rezerve][2B handler ofseti (sektor-1 mutlak)]
cmd_table:
    db "win"
    times 26 db 0
    dw handle_win

    db "help"
    times 26 db 0
    dw handle_help

    db "reboot"
    times 24 db 0
    dw handle_reboot

    dw 0                      ; tablo sonu isareti

; Sektor-2 dolgu: toplam 1024 bayt
TIMES 4096 db 0
