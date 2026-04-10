# ==================== TOOLCHAIN DETECTION ====================
CROSS_GCC := $(shell command -v i686-elf-gcc 2>/dev/null)
CROSS_LD  := $(shell command -v i686-elf-ld  2>/dev/null)
CROSS_AS  := $(shell command -v i686-elf-as  2>/dev/null)

ifeq ($(and $(CROSS_GCC),$(CROSS_LD),$(CROSS_AS)),)
    # Native toolchain
    CC = gcc
    LD = ld
    AS = gcc
    CFLAGS = -ffreestanding -m32 -Wall -Wextra -I$(P)/include -I$(P)/src
    LDFLAGS = -m elf_i386
else
    # Cross toolchain (i686-elf-*)
    CC = $(CROSS_GCC)
    LD = $(CROSS_LD)
    AS = $(CROSS_GCC)
    CFLAGS = -ffreestanding -Wall -Wextra -I$(P)/include -I$(P)/src
    LDFLAGS = -m elf_i386
endif

P = FreezeProject
BUILDDIR = $(P)/build
SRCDIR = $(P)/src

C_SOURCES = $(wildcard $(SRCDIR)/*.c)
ASM_SOURCES = $(wildcard $(SRCDIR)/*.S)

C_OBJECTS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(C_SOURCES))
ASM_OBJECTS = $(patsubst $(SRCDIR)/%.S,$(BUILDDIR)/%.o,$(ASM_SOURCES))

OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS)

all: freeze.iso

# compile
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.S
	mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# kernel
$(BUILDDIR)/kernel.bin: $(OBJECTS) $(SRCDIR)/linker.ld
	$(LD) $(LDFLAGS) -T $(SRCDIR)/linker.ld -o $@ $(OBJECTS)

# grub
iso/boot/grub/grub.cfg: $(P)/grub/grub.cfg
	mkdir -p iso/boot/grub
	cp $< $@

# build iso
freeze.iso: $(BUILDDIR)/kernel.bin iso/boot/grub/grub.cfg
	rm -rf iso/boot/kernel.bin
	mkdir -p iso/boot/grub
	cp $(BUILDDIR)/kernel.bin iso/boot/
	grub-mkrescue -o $@ iso

# staging directory for filesystem content
FSDIR = FreezeProject/fs-staging
ROOTFSDIR = $(P)/rootfs
IMPORTDIR = $(P)/import
ROOTFS_SOURCES = $(shell if [ -d $(ROOTFSDIR) ]; then find $(ROOTFSDIR) -mindepth 1 -print; fi)
IMPORT_SOURCES = $(shell if [ -d $(IMPORTDIR) ]; then find $(IMPORTDIR) -mindepth 1 -maxdepth 1 -type f -print; fi)
URL ?=
NAME ?=

$(FSDIR)/.dirs:
	mkdir -p $(FSDIR)
	mkdir -p $(FSDIR)/bin
	mkdir -p $(FSDIR)/boot
	mkdir -p $(FSDIR)/etc
	mkdir -p $(FSDIR)/home
	mkdir -p $(FSDIR)/lib
	mkdir -p $(FSDIR)/media
	mkdir -p $(FSDIR)/mnt
	mkdir -p $(FSDIR)/opt
	mkdir -p $(FSDIR)/root
	mkdir -p $(FSDIR)/tmp
	mkdir -p $(FSDIR)/usr/bin
	mkdir -p $(FSDIR)/usr/lib
	mkdir -p $(FSDIR)/usr/books
	mkdir -p $(FSDIR)/var/log
	touch $@


$(FSDIR)/.files: $(FSDIR)/.dirs $(ROOTFS_SOURCES) $(IMPORT_SOURCES)
	@if [ -d $(ROOTFSDIR) ]; then cp -a $(ROOTFSDIR)/. $(FSDIR)/; fi
	cp $(P)/books/* $(FSDIR)/usr/books/ 2>/dev/null || true
	@if [ -d $(IMPORTDIR) ]; then \
		count=$$(ls -1 $(IMPORTDIR) 2>/dev/null | wc -l); \
		if [ "$$count" -gt 0 ]; then \
			echo "Importing $$count file(s) into /home..."; \
			cp $(IMPORTDIR)/* $(FSDIR)/home/; \
		fi \
	fi
	touch $@

# disk image for persistent file system (ext2 format)
freeze.img: $(FSDIR)/.files
	@if command -v genext2fs >/dev/null 2>&1; then \
		echo "Creating ext2 filesystem with genext2fs..."; \
		genext2fs -b 5120 -d $(FSDIR) -L FREEZE $@; \
	else \
		echo "genext2fs not found. Install it for a proper filesystem:"; \
		echo "  sudo apt install genext2fs"; \
		echo "Creating blank disk image as fallback..."; \
		dd if=/dev/zero of=$@ bs=1M count=10 2>/dev/null || true; \
	fi

run: freeze.iso freeze.img
	@if [ -n "$$DISPLAY" ] || [ -n "$$WAYLAND_DISPLAY" ]; then \
		qemu-system-x86_64 -cdrom freeze.iso -drive file=freeze.img,format=raw,media=disk; \
	else \
		echo "No GUI display detected; running QEMU in headless mode."; \
		qemu-system-x86_64 -cdrom freeze.iso -drive file=freeze.img,format=raw,media=disk -display none -serial stdio; \
	fi

import:
	@if [ -z "$(URL)" ]; then \
		echo "Usage: make import URL=https://example.com/file.txt [NAME=file.txt]"; \
		exit 1; \
	fi
	@mkdir -p $(IMPORTDIR)
	@fname="$(NAME)"; \
	if [ -z "$$fname" ]; then fname="$${URL##*/}"; fi; \
	if [ -z "$$fname" ] || [ "$$fname" = "$(URL)" ]; then fname="downloaded_file"; fi; \
	fname="$${fname%%\?*}"; \
	echo "Downloading $(URL) -> $(IMPORTDIR)/$$fname"; \
	if command -v curl >/dev/null 2>&1; then \
		curl -fL "$(URL)" -o "$(IMPORTDIR)/$$fname"; \
	elif command -v wget >/dev/null 2>&1; then \
		wget -O "$(IMPORTDIR)/$$fname" "$(URL)"; \
	else \
		echo "Error: neither curl nor wget is installed."; \
		exit 1; \
	fi; \
	echo "Imported $$fname. Run 'make run' to include it in /home."

clean:
	rm -rf $(BUILDDIR) freeze.iso iso $(FSDIR) freeze.img

.PHONY: all clean run import
