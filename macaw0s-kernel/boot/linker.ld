/* MacawOS kernel — linker script: higher-half 0xFFFFFFFF80000000, LMA low */
ENTRY(_start)

PHDRS
{
    boot   PT_LOAD FLAGS(7);          /* RWX for .boot (multiboot reqs)  */
    text   PT_LOAD FLAGS(5);          /* RX                                     */
    rodata PT_LOAD FLAGS(4);          /* R                                      */
    data   PT_LOAD FLAGS(6);          /* RW                                     */
}

SECTIONS
{
    . = 1M;                           /* load address for GRUB/QEMU      */

    .boot ALIGN(4K) : {
        KEEP(*(.multiboot_header))
        *(.boot)
        *(.boot.*)
    } :boot

    . += 0xFFFFFFFF80000000;
    kernel_vaddr = .;

    __text_start = .;
    .text ALIGN(4K) : AT(ADDR(.text) - 0xFFFFFFFF80000000) {
        *(.text .text.*)
    } :text
    __text_end = .;

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - 0xFFFFFFFF80000000) {
        *(.rodata .rodata.*)
    } :rodata

    .data ALIGN(4K) : AT(ADDR(.data) - 0xFFFFFFFF80000000) {
        *(.data .data.*)
    } :data

    .bss ALIGN(4K) : AT(ADDR(.bss) - 0xFFFFFFFF80000000) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.note*)
        *(.comment)
    }
}
