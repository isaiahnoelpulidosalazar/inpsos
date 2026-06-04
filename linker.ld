ENTRY(_start)
OUTPUT_FORMAT("binary")

SECTIONS
{
    . = 0x10000;

    .text : ALIGN(4)
    {
        *(.text)
    }

    .rodata : ALIGN(4)
    {
        *(.rodata)
    }

    .data : ALIGN(4)
    {
        *(.data)
    }

    .bss : ALIGN(4)
    {
        *(.bss)
        *(COMMON)
    }
}