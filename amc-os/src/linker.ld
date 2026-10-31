/* linker.ld - AMC-OS: metin 1 MiB'a hizalanir */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R-X */
    data PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
    . = 1M;                  /* 0x100000 */

    .text ALIGN(4K) : {
        KEEP(*(.multiboot_header))
        *(.text .text.*)
    } :text

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    } :data

    .data ALIGN(4K) : {
        *(.data .data.*)
    } :data

    .bss ALIGN(4K) : {
        PROVIDE(__bss_start = .);
        *(COMMON)
        *(.bss .bss.*)
        PROVIDE(__bss_end = .);
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
