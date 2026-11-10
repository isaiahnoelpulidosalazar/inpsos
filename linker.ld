/* Flat binary loaded by the bootloader at 0x10000. */
ENTRY(_start)
OUTPUT_FORMAT("binary")

SECTIONS
{
    . = 0x10000;

    .text : ALIGN(4)
    {
        *(.text.entry)          /* _start MUST be first: the bootloader jumps to 0x10000 */
        *(.text .text.*)
    }

    .rodata : ALIGN(4) { *(.rodata .rodata.*) }
    .data   : ALIGN(4) { *(.data .data.*) }

    /* BSS is not part of the flat file; kernel_entry.asm zeroes it at boot. */
    .bss : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }

    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) }
}
