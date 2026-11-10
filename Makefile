# inpsos build system
#   make            -> inpsos.img + inpsos.iso
#   make test       -> host-side tests for the Easec interpreter and the filesystem
#   make clean
#
# Layout constants live HERE and are passed to the assembler, the C compiler and
# the packer, so they can no longer drift apart between boot.asm / kernel / pack_fs.py.

KERNEL_SECTORS ?= 240
FS_DIR_SECTOR  ?= 256

CC   ?= gcc
LD   ?= ld
ASM  ?= nasm

BUILD := build

WARN    = -Wall -Wextra -Wshadow -Wno-unused-parameter
CFLAGS  = -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O2 -std=gnu11 $(WARN) \
          -fno-stack-protector -fno-asynchronous-unwind-tables -fcf-protection=none \
          -fno-tree-loop-distribute-patterns -mno-sse -mno-mmx -mno-avx \
          -MMD -MP -Ikernel -Ieasec -I$(BUILD) \
          -DKERNEL_SECTORS=$(KERNEL_SECTORS) -DFS_DIR_SECTOR=$(FS_DIR_SECTOR)
LDFLAGS = -m elf_i386 -T linker.ld --build-id=none -z noexecstack
LIBGCC  = $(shell $(CC) -m32 -print-libgcc-file-name)

KERNEL_SRCS = $(wildcard kernel/*.c) $(wildcard easec/*.c)
KERNEL_OBJS = $(BUILD)/boot/kernel_entry.o $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_SRCS))
PROGRAMS    = $(wildcard programs/*.easec) $(wildcard programs/*.modec)

all: inpsos.iso

$(BUILD)/boot/boot.bin: boot/boot.asm
	@mkdir -p $(@D)
	$(ASM) -f bin -DKERNEL_SECTORS=$(KERNEL_SECTORS) $< -o $@

$(BUILD)/boot/kernel_entry.o: boot/kernel_entry.asm
	@mkdir -p $(@D)
	$(ASM) -f elf32 $< -o $@

# Generated headers embedding the boot sector and the packed filesystem.
$(BUILD)/fs.bin $(BUILD)/fs_bin.h $(BUILD)/boot_bin.h: $(BUILD)/boot/boot.bin tools/pack_fs.py $(PROGRAMS)
	python3 tools/pack_fs.py --programs programs --boot $(BUILD)/boot/boot.bin \
	    --dir-sector $(FS_DIR_SECTOR) --out-dir $(BUILD)

# kernel/installer.c is the only consumer of the generated headers.
$(BUILD)/kernel/installer.o: $(BUILD)/fs_bin.h $(BUILD)/boot_bin.h

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.bin: $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS) $(LIBGCC)
	@size=$$(stat -c %s $@); max=$$(( $(KERNEL_SECTORS) * 512 )); \
	 if [ $$size -gt $$max ]; then \
	   echo "ERROR: kernel.bin is $$size bytes but only $$max fit in $(KERNEL_SECTORS) sectors." >&2; \
	   echo "       Raise KERNEL_SECTORS (and keep it below FS_DIR_SECTOR=$(FS_DIR_SECTOR))."  >&2; exit 1; fi

inpsos.img: $(BUILD)/boot/boot.bin $(BUILD)/kernel.bin $(BUILD)/fs.bin
	dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	dd if=$(BUILD)/boot/boot.bin of=$@ conv=notrunc bs=512 count=1 status=none
	dd if=$(BUILD)/kernel.bin    of=$@ conv=notrunc bs=512 seek=1 status=none
	dd if=$(BUILD)/fs.bin        of=$@ conv=notrunc bs=512 seek=$(FS_DIR_SECTOR) status=none

inpsos.iso: inpsos.img
	mkdir -p $(BUILD)/iso_root
	cp inpsos.img $(BUILD)/iso_root/
	genisoimage -quiet -b inpsos.img -o $@ $(BUILD)/iso_root/

# ---- host tests (no emulator needed) ---------------------------------------
HOSTCC ?= cc
HOSTFLAGS = -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -DEASEC_HOSTED -Ieasec -fsanitize=address,undefined
test: $(BUILD)/test_easec $(BUILD)/test_fs
	$(BUILD)/test_easec tests
	$(BUILD)/test_fs

$(BUILD)/test_easec: tests/test_easec.c $(wildcard easec/*.c) $(wildcard easec/*.h)
	@mkdir -p $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) tests/test_easec.c $(wildcard easec/*.c) -o $@ -lm

$(BUILD)/test_fs: tests/test_fs.c kernel/fs.c kernel/fs.h
	@mkdir -p $(BUILD)
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -DHOSTED_TEST -Ikernel tests/test_fs.c kernel/fs.c -o $@ -fsanitize=address,undefined

clean:
	rm -rf $(BUILD) inpsos.img inpsos.iso

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
.PHONY: all test clean
