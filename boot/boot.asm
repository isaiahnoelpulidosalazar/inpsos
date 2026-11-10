; ---------------------------------------------------------------------------
; inpsos stage-1 bootloader (MBR)
;
;   * loads KERNEL_SECTORS sectors starting at LBA 1 to physical 0x10000
;   * enables A20, loads a flat GDT, enters 32-bit protected mode
;   * jumps to the kernel entry point at 0x10000
;
; Changes vs. the original:
;   - BUG FIX: the second read used segment 0x1F00 (= 0x1F000) although the first
;     read ended at 0x1E000, leaving a 4 KiB hole in the kernel image.  The kernel is
;     now loaded by ONE loop in 32 KiB chunks, so the segments are always contiguous.
;   - Kernel size is a build-time constant (nasm -DKERNEL_SECTORS=..) shared with the
;     Makefile, kernel and installer instead of two hard-coded 120-sector reads.
;   - Fast-A20 no longer sets bit 0 of port 0x92 (that bit is "system reset").
;   - Error path halts in a loop; success path no longer re-reads ds/es twice.
; ---------------------------------------------------------------------------
[bits 16]
[org 0x7c00]

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 240
%endif

KERNEL_SEG     equ 0x1000                    ; segment 0x1000 -> physical 0x10000
KERNEL_PHYS    equ 0x10000
CHUNK_SECTORS  equ 64                        ; 32 KiB per BIOS call, never crosses a 64 KiB boundary

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    cld
    mov [boot_drive], dl                     ; BIOS passes the boot drive in DL

    mov si, MSG_BOOTING
    call print_string

    call load_kernel

    mov ax, 0x2401                           ; A20 via BIOS ...
    int 0x15
    in al, 0x92                              ; ... and "fast A20" as a fallback
    and al, 0xFE                             ; never touch bit 0 (system reset)
    or  al, 0x02
    out 0x92, al

    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp CODE_SEG:init_pm

; --- print_string: DS:SI -> zero terminated string --------------------------
print_string:
    pusha
.loop:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0e
    int 0x10
    jmp .loop
.done:
    popa
    ret

; --- load_kernel: read KERNEL_SECTORS sectors (LBA 1..) to 0x10000 ----------
load_kernel:
    mov word  [dap_offset],  0
    mov word  [dap_segment], KERNEL_SEG
    mov dword [dap_lba_lo],  1
    mov dword [dap_lba_hi],  0
    mov cx, KERNEL_SECTORS                   ; sectors still to read
.next_chunk:
    mov ax, CHUNK_SECTORS
    cmp cx, ax
    jae .have_count
    mov ax, cx                               ; last, shorter chunk
.have_count:
    mov [dap_count], ax
    push ax
    push cx
    mov ah, 0x42                             ; INT 13h extended read
    mov dl, [boot_drive]
    mov si, dap
    int 0x13
    pop cx                                   ; (POP does not alter CF)
    pop ax
    jc .error
    sub cx, ax
    jz .done
    movzx edx, ax
    add [dap_lba_lo], edx                    ; next LBA
    shl ax, 5                                ; sectors * 512 / 16 = paragraphs
    add [dap_segment], ax                    ; next segment, contiguous with the last
    jmp .next_chunk
.done:
    ret
.error:
    mov si, MSG_ERR_DISK
    call print_string
.halt:
    hlt
    jmp .halt

boot_drive: db 0x80

align 4
dap:
    db 0x10
    db 0
dap_count:    dw 0
dap_offset:   dw 0
dap_segment:  dw 0
dap_lba_lo:   dd 0
dap_lba_hi:   dd 0

; --- flat 4 GiB code/data segments ------------------------------------------
gdt_start:
    dd 0x0
    dd 0x0
gdt_code:
    dw 0xffff
    dw 0x0
    db 0x0
    db 10011010b
    db 11001111b
    db 0x0
gdt_data:
    dw 0xffff
    dw 0x0
    db 0x0
    db 10010010b
    db 11001111b
    db 0x0
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

[bits 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov eax, KERNEL_PHYS
    jmp eax

MSG_BOOTING:  db "inpsos: loading kernel...", 13, 10, 0
MSG_ERR_DISK: db "Disk read error!", 13, 10, 0

times 510-($-$$) db 0
dw 0xaa55
