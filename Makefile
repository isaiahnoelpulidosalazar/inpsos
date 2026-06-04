CC = gcc
LD = ld
ASM = nasm

CFLAGS = -m32 -ffreestanding -fno-pie -fno-builtin -nostdlib -O2 -Ikernel
LDFLAGS = -m elf_i386 -T linker.ld

LIBGCC = $(shell $(CC) -m32 -print-libgcc-file-name)

KERNEL_OBJS = \
	boot/kernel_entry.o \
	kernel/kernel.o \
	kernel/libc.o \
	kernel/ahci.o \
	kernel/easec.o

all: inpsos.iso

boot/boot.bin: boot/boot.asm
	$(ASM) -f bin $< -o $@

boot/kernel_entry.o: boot/kernel_entry.asm
	$(ASM) -f elf32 $< -o $@

kernel/fs_bin.h: boot/boot.bin pack_fs.py $(wildcard programs/*.easec)
	python3 pack_fs.py

kernel/kernel.o: kernel/kernel.c kernel/fs_bin.h
	$(CC) $(CFLAGS) -c $< -o $@

kernel/libc.o: kernel/libc.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel/ahci.o: kernel/ahci.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel/easec.o: kernel/easec.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel.bin: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS) $(LIBGCC)

fs.bin: kernel/fs_bin.h

inpsos.img: boot/boot.bin kernel.bin fs.bin
	dd if=/dev/zero of=$@ bs=512 count=2880
	dd if=boot/boot.bin of=$@ conv=notrunc bs=512 count=1
	dd if=kernel.bin of=$@ conv=notrunc seek=1 bs=512
	dd if=fs.bin of=$@ conv=notrunc seek=256 bs=512

inpsos.iso: inpsos.img
	mkdir -p iso_root
	cp inpsos.img iso_root/
	genisoimage -b inpsos.img -o $@ iso_root/

clean:
	rm -f boot/*.bin boot/*.o kernel/*.o *.bin *.img *.iso fs.bin kernel/fs_bin.h kernel/boot_bin.h
	rm -rf iso_root