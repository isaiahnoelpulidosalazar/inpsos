# inpsos

A small 32-bit x86 operating system: custom MBR bootloader, freestanding libc, AHCI/SATA driver, the INPS
filesystem and **Easec**, a bytecode-compiled scripting language that runs the shell's programs.

## Source layout
```
boot/        boot.asm (MBR stage), kernel_entry.asm (entry, BSS clear)
kernel/      libc (string, printf, heap) - console, keyboard, timer/RTC - pci, ahci, blockdev
             fs (INPS) - shell, installer, kernel.c (k_main)
easec/       lexer, compiler (single pass), vm, object (tables, GC), natives; easec.h = embedding API
programs/    *.easec programs and *.modec modules packed into the filesystem
tools/       pack_fs.py
tests/       host-side tests (Easec scripts + filesystem), run by `make test`
docs/        EASEC_LANGUAGE.md (reference + v1->v2 migration), REVIEW.md
```
Dependency direction: `kernel.c -> shell/installer -> fs -> blockdev <- ahci`, and `shell -> easec` only through
`EasecHost`. The interpreter knows nothing about the kernel, which is why it can be tested on a normal PC.

## Build
```
sudo apt install nasm gcc-multilib genisoimage make python3
make            # inpsos.img + inpsos.iso
make test       # host tests (ASan/UBSan)
```
Layout constants (`KERNEL_SECTORS`, `FS_DIR_SECTOR`) are set once in the Makefile and passed to nasm, gcc and the packer.
The build fails if the kernel outgrows its reserved sectors.

## Boot and install
Press **i** during the 2-second start-up delay to force the installer; otherwise any SATA disk containing the
`INPS` signature at sector 256 boots straight into the shell. In the installer, `install` asks which disk (if
several) and requires you to type `YES` before it erases sectors 0..320 and writes bootloader, kernel and programs.

## Shell
`name` runs `name.easec`. Built-ins: `clear`, `reboot`. Shipped programs: `help`, `list`, `create_file`,
`delete_file`, `test`, `test_datetime`. Each run gets a fresh interpreter; its memory is released on exit.

## INPS filesystem
Sector 256 holds up to 10 entries (name[32], start, sectors, size, used) and the signature at byte 508; file data
follows from sector 257 in contiguous extents. Writing an existing name replaces it; deleted space is reused.
(The on-disk format is unchanged from v1.)

## Easec
See [docs/EASEC_LANGUAGE.md](docs/EASEC_LANGUAGE.md). Quick taste:
```
job add(a, b) { out a + b }
var total = add(15, 25)
say "total: " + total
```
