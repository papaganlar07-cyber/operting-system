/* linker.ld - AMC-OS Adim 4: Linker Script */
ENTRY(kernel_main)

SECTIONS
{
    . = 0x1000;             /* Kodun yerlestirecegi baslangic adresi */

    .text ALIGN(4K) :
    {
        *(.text)
        *(.text.*)
        *(.data_code)       /* runtime IDT stub'lari (RX hakkiyla) */
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4K) :
    {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}
