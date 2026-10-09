; ============================================================================
; MacawOS - Multiboot2 bootstrap -> Long Mode (x86_64). NASM -f elf64.
; Identity-maps low 1GiB, HHDM 0xFFFF800000000000->phys 0..1GiB,
; kernel higher-half 0xFFFFFFFF80000000 -> same PD.
; ============================================================================
MB2_MAGIC equ 0xE85250D6
MB2_ARCH  equ 0
MB2_HDR_LEN equ mb2_header_end - mb2_header_start

section .multiboot_header align=4
mb2_header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd MB2_HDR_LEN
    dd -(MB2_MAGIC + MB2_ARCH + MB2_HDR_LEN)
    dw 1                     ; information request tag
    dw 0
    dd 24                    ; 8 header + 4*4 requested tags
    dd 1                     ; basic meminfo
    dd 4                     ; memory map
    dd 17                    ; command line
    dd 2                     ; bootloader name
    dw 0                     ; end tag
    dw 0
    dd 8
mb2_header_end:

section .boot align=16
BITS 32
_start:
    cli
    mov esp, stack32_top
    mov [cs:magic_save], eax
    mov [cs:mbi_save], ebx
    cmp eax, MB2_MAGIC
    jne .no_multiboot

    ; --- zero page tables -------------------------------------------------
    mov edi, pml4_low
    xor eax, eax
    mov ecx, (stack32 - pml4_low)/4
    rep stosd

    ; --- PD: 512 huge pages covering phys 0..1GiB ------------------------
    mov edi, pd_low
    mov ecx, 0
    mov ebx, 0x83                          ; P|RW|PS
.fill_pd:
    mov eax, ecx
    shl eax, 21
    or  eax, ebx
    stosd
    xor eax, eax
    stosd
    inc ecx
    cmp ecx, 512
    jb .fill_pd

    ; --- helper regs: table addrs with P|RW flags ------------------------
    lea eax, [pd_low]
    or  eax, 3
    mov edx, eax                           ; edx = pd entry value
    lea esi, [pdpt_low]
    or  esi, 3                             ; esi = pdpt_low entry value
    lea edi, [pdpt_hhdm]
    or  edi, 3
    lea ebp, [pdpt_kernel]
    or  ebp, 3

    ; PDPT_low[0] -> PD
    mov [pdpt_low + 0*8], edx
    mov dword [pdpt_low + 0*8 + 4], 0
    ; PDPT_hhdm[0] -> PD
    mov [pdpt_hhdm + 0*8], edx
    mov dword [pdpt_hhdm + 0*8 + 4], 0
    ; PDPT_kernel[510],[511] -> PD
    mov [pdpt_kernel + 510*8], edx
    mov dword [pdpt_kernel + 510*8 + 4], 0
    mov [pdpt_kernel + 511*8], edx
    mov dword [pdpt_kernel + 511*8 + 4], 0

    ; PML4_final[0] -> PDPT_low, [261] -> PDPT_hhdm, [511] -> PDPT_kernel
    lea eax, [pml4_final]
    mov [eax + 0*8], esi
    mov dword [eax + 0*8 + 4], 0
    mov [eax + 261*8], edi
    mov dword [eax + 261*8 + 4], 0
    mov [eax + 511*8], ebp
    mov dword [eax + 511*8 + 4], 0

    ; --- GDT ------------------------------------------------------------
    lgdt [gdt64_ptr]

    ; --- PAE | CR3 | EFER.LME | PG+WP -----------------------------------
    mov eax, cr4
    or  eax, 1 << 5
    mov cr4, eax

    lea eax, [pml4_final]
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or  eax, 1 << 8
    wrmsr

    mov eax, cr0
    or  eax, (1 << 31) | (1 << 16)
    mov cr0, eax

    jmp gdt64_code:.long_mode

.no_multiboot:
    mov edi, 0xB8000
    mov ah, 0x4F
    mov ecx, err_len
    mov esi, err_msg
.print_loop:
    lodsb
    stosw
    loop .print_loop
.hang32:
    cli
    hlt
    jmp .hang32

BITS 64
.long_mode:
    mov ax, gdt64_data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; --- jump to higher-half alias of the continuation ------------------
    lea rcx, [rel in_higherhalf]          ; LMA (identity-mapped) address
    mov rax, 0xFFFFFFFF80000000           ; 64-bit mov accepts imm64
    add rcx, rax                          ; VMA alias: same physical pages
    jmp rcx

section .boot align=16
BITS 64
in_higherhalf:
    ; now executing via kernel VA; all data refs are high-half addresses
    lea rsp, [boot_stack + 32768]

    extern __bss_start
    extern __bss_end
    extern kernel_main
    lea rdi, [rel __bss_start]
    lea rdx, [rel __bss_end]
    xor eax, eax
    mov rcx, rdx
    sub rcx, rdi
    shr rcx, 3
    jz .skip_bss
    rep stosq
.skip_bss:
    and rsp, -16

    mov rdi, [rel magic_save]
    mov rsi, [rel mbi_save]               ; PHYSICAL MBI ptr (C maps via HHDM)
    cld
    call kernel_main

.final_hang:
    cli
    hlt
    jmp .final_hang

section .bss align=4096
pml4_low:      resb 4096
pdpt_low:      resb 4096
pd_low:        resb 4096
pml4_final:    resb 4096
pdpt_hhdm:     resb 4096
pdpt_kernel:   resb 4096
stack32:       resb 4096
boot_stack:    resb 32768
stack32_top equ stack32 + 4096

section .data
magic_save: dq 0
mbi_save:   dq 0

gdt64_start:
    dq 0
gdt64_code equ $ - gdt64_start
    dq (1<<40)|(1<<43)|(1<<44)|(1<<47)|(1<<53)|(1<<54) ; A,L,D,P,S,X
gdt64_data equ $ - gdt64_start
    dq (1<<40)|(1<<41)|(1<<44)|(1<<47)     ; A,W,P,S
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64_start - 1
    dd gdt64_start

section .rodata
err_msg db "MacawOS FATAL: no Multiboot2 loader!"
err_len equ $ - err_msg
