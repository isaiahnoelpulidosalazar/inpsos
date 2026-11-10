# Review notes

See the chat response for the summary. This file records what was verified and what was NOT.

## Verified here
* `make test`: 16 Easec scripts and the filesystem test run under AddressSanitizer + UBSan, no failures, no leaks.
* All kernel and interpreter sources compile cleanly for i386 (`-m32 -ffreestanding`) with `-Wall -Wextra -Wshadow`.
* The kernel's object code is ~64 KiB of text, comfortably inside the 240-sector (120 KiB) kernel window.

## NOT verified (no nasm / QEMU / genisoimage in the review sandbox)
* `boot/boot.asm` and `boot/kernel_entry.asm` were not assembled.
* No end-to-end boot (BIOS -> protected mode -> kernel -> AHCI -> shell) was run.
* PIT/TSC calibration, CMOS RTC decoding and the rewritten AHCI driver are untested on real or emulated hardware.
Run `make && qemu-system-i386 -cdrom inpsos.iso -boot d -drive file=disk.img,if=none,id=d,format=raw -device ich9-ahci,id=ahci -device ide-hd,drive=d,bus=ahci.0`
(after `qemu-img create disk.img 64M`) before trusting any of it.
