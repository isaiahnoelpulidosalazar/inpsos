; Kernel entry point. Must be the first thing in the image (see linker.ld).
[bits 32]

section .text.entry
global _start
extern k_main
extern __bss_start
extern __bss_end

_start:
    cli
    cld
    mov esp, 0x90000            ; do not depend on what the bootloader left behind

    ; A flat binary does not contain .bss, and RAM is not guaranteed to be zero.
    ; Every zero-initialised global (vm state, dir cache, DMA tables ...) relies on this.
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    shr ecx, 2
    xor eax, eax
    rep stosd

    fninit                      ; defined x87 state: Easec uses `double`
    call k_main
.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits
