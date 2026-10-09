; MacawOS - exception & IRQ stubs + SYSCALL entry (NASM elf64).
extern isr_exception_handler   ; void(u64 vec, regs_t*)
extern isr_irq_handler         ; void(u64 irq, regs_t*)
extern syscall_handler_impl    ; u64(u64 *args7) -- C side

section .text
%macro SAVE_REGS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro RESTORE_REGS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

%macro EXC_STUB 1          ; no error code: CPU pushed rip,cs,rflags (24B)
global exc_%1
exc_%1:
    SAVE_REGS
    mov rdi, %1
    mov rsi, rsp
    cld
    call isr_exception_handler
    RESTORE_REGS
    add rsp, 24
    iretq
%endmacro

%macro EXC_STUB_ERRCODE 1  ; CPU pushed errcode,rip,cs,rflags (32B)
global exc_%1
exc_%1:
    SAVE_REGS
    mov rdi, %1
    mov rsi, rsp
    cld
    call isr_exception_handler
    RESTORE_REGS
    add rsp, 32
    iretq
%endmacro

EXC_STUB 0
EXC_STUB 1
EXC_STUB 2
EXC_STUB 3
EXC_STUB 4
EXC_STUB 5
EXC_STUB 6
EXC_STUB 7
EXC_STUB 9
EXC_STUB 16
EXC_STUB 17
EXC_STUB 18
EXC_STUB 19
EXC_STUB 20
EXC_STUB_ERRCODE 8
EXC_STUB_ERRCODE 10
EXC_STUB_ERRCODE 11
EXC_STUB_ERRCODE 12
EXC_STUB_ERRCODE 13
EXC_STUB_ERRCODE 14
EXC_STUB_ERRCODE 15

; ---- Hardware IRQ stubs: vectors 32..47 -> IRQ 0..15 -----------------------
%macro IRQ_STUB 1
global irq%1
irq%1:
    SAVE_REGS
    mov rdi, %1
    mov rsi, rsp
    cld
    call isr_irq_handler
    RESTORE_REGS
    add rsp, 24               ; rip, cs, rflags pushed by CPU
    iretq
%endmacro

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

; ---- SYSCALL fast path ------------------------------------------------------
; rcx = user rip, r11 = user rflags, rax = number.
; Build a 7-entry arg array {rdi,rsi,rdx,r10,r8,r9,nr} on the kernel stack,
; call the C dispatcher, return value in rax, restore via sysretq.
global syscall_entry
syscall_entry:
    push rax                  ; nr
    push rcx                  ; saved rip
    push r11                  ; saved rflags
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8*7              ; aligned scratch for args (nr goes in slot 6)
    mov [rsp+0*8], rdi
    mov [rsp+1*8], rsi
    mov [rsp+2*8], rdx
    mov [rsp+3*8], r10
    mov [rsp+4*8], r8
    mov [rsp+5*8], r9
    mov rax, [rsp+8*7+8*8]      ; saved nr (pushed first, deepest)
    mov [rsp+6*8], rax
    mov rdi, rsp
    cld
    call syscall_handler_impl
    add rsp, 8*7
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11                   ; rflags back
    pop rcx                   ; rip back
    add rsp, 8                ; drop nr copy
    sysretq
