# Environment Variables
CURRENT_DIR := $(shell pwd)
BUILD_DIR := $(CURRENT_DIR)/build
CONFIG_DIR := $(CURRENT_DIR)/config
NPROC := $(shell nproc)
CROSS_COMPILE ?= riscv64-linux-gnu-

# Qemu Variables
qemu_srcdir := $(CURRENT_DIR)/qemu
qemu_builddir := $(BUILD_DIR)/qemu
qemu_target := $(qemu_builddir)/qemu-system-riscv64
qemu_config_args := --target-list=riscv64-softmmu
qemu_machine := -machine xiangshan-nanhuv5
qemu_args := -cpu xiangshan-nanhuv5 -smp 1 -m 8G

# Linux Variables
linux_srcdir := $(CURRENT_DIR)/linux
linux_builddir := $(BUILD_DIR)/linux
linux_config := $(CONFIG_DIR)/xiangshan.config
linux_vmlinux := $(linux_builddir)/vmlinux
linux_image := $(linux_builddir)/arch/riscv/boot/Image
linux_start := 0x82000000
linux_offset := 0x2000000
fdt_start := 0x88000000


# Rootfs Variables
rootfs_srcdir := $(CURRENT_DIR)/rootfs
rootfs_target := $(CONFIG_DIR)/rootfs_nopasswd.cpio
rootfs_archive := $(rootfs_target).gz

# Device Tree Variables
dts_file := $(CONFIG_DIR)/nanhu-v5.dts
dtb_file := $(BUILD_DIR)/nanhu-v5.dtb

# OP-TEE Variables
optee_os_srcdir := $(CURRENT_DIR)/optee_os
optee_os_builddir := $(BUILD_DIR)/optee_os
optee_os_platdir := $(CONFIG_DIR)/plat-nanhu/
optee_os_bin := $(optee_os_builddir)/core/tee.bin
optee_os_elf := $(optee_os_builddir)/core/tee.elf
optee_os_start := 0x81000000
optee_os_offset := 0x1000000


# OpenSBI Variables
opensbi_srcdir := $(CURRENT_DIR)/opensbi
opensbi_builddir := $(BUILD_DIR)/opensbi
opensbi_bindir := $(opensbi_builddir)/platform/generic/firmware
opensbi_jump_bin := $(opensbi_bindir)/fw_jump.bin

# Foreign-interrupt preemption test Variables
preempt_test_srcdir := $(CURRENT_DIR)/tests/preempt_ta
preempt_test_builddir := $(BUILD_DIR)/preempt_test
preempt_test_uuid := 65dca01d-cd82-4b02-98bb-c980a3657545
preempt_test_ta := $(preempt_test_builddir)/ta/$(preempt_test_uuid).ta
preempt_test_host := $(preempt_test_builddir)/preempt_test
ta_dev_kit_dir := $(optee_os_builddir)/export-ta_rv64
opensbi_jump_elf := $(opensbi_bindir)/fw_jump.elf
opensbi_payload_bin := $(opensbi_bindir)/fw_payload.bin
opensbi_start := 0x80000000

# merged payload
merge_payload_bin := $(BUILD_DIR)/merge_payload.bin

###########
# help
###########
.PHONY: all help
all: help

help:
	@echo "Makefile for Linux and OP-TEE on OpenXiangShan Nanhu V5"
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  help             Display help messages"
	@echo "  qemu             Build QEMU"
	@echo "  linux            Build Linux kernel"
	@echo "  linux-image      Build the Linux Image needed by QEMU"
	@echo "  optee_os         Build OP-TEE OS"
	@echo "  opensbi-jump     Build OpenSBI with jump pattern"
	@echo "  opensbi-payload  Build OpensSBI with payload pattern"
	@echo "  dtb              Generate Device Tree Blob (DTB)"
	@echo "  rootfs-extract   Extract root filesystem from cpio archive"
	@echo "  rootfs-pack      Pack root filesystem into cpio archive"
	@echo "  preempt-test     Build the foreign-interrupt preemption test TA and host app"
	@echo "  preempt-test-install  Add the preemption test to the rootfs (then rebuild linux-image)"
	@echo "  run-jump         Run QEMU with one built images, but it's just a jump, won't be packaged together"
	@echo "  run-tee          Run Nanhu V5 Linux with OP-TEE"
	@echo "  run-linux        Run Nanhu V5 Linux without OP-TEE"
	@echo "  run-payload      Run QEMU with one built images, it will be packaged together"
	@echo "  run-fpga         Run QEMU with two built images, opensbi + Linux + optee will be merged"
	@echo "  merge            Merge OpensSBI Linux Op-TEE into one bin according to the specified address range"
	@echo "  debug            Run QEMU with debugging enabled"
	@echo "  qemu-clean       Clean QEMU build directory"
	@echo "  qemu-distclean   Clean QEMU build directory and remove all generated files"
	@echo "  linux-clean      Clean Linux kernel build directory"
	@echo "  linux-distclean  Clean Linux kernel build directory and remove all generated files"
	@echo "  optee_os-clean   Clean OP-TEE OS build directory"
	@echo "  dtb-clean        Clean generated Device Tree Blob (DTB)"
	@echo "  opensbi-clean    Clean OpenSBI build directory"
	@echo "  preempt-test-clean  Clean the preemption test build directory"
	@echo "  merge-clean      Clean OpenSBI Linux Op-TEE merge file"
	@echo "  clean-all        Clean all generated file"

###########
# qemu
###########
.PHONY: qemu
qemu: $(qemu_builddir)/config-host.mak
	$(MAKE) -C $(qemu_builddir) O=$(qemu_builddir) -j $(NPROC)

$(qemu_builddir)/config-host.mak:
	mkdir -p $(dir $@)
	cd $(qemu_builddir) && \
	$(qemu_srcdir)/configure $(qemu_config_args)

###########
# rootfs
###########
.PHONY: rootfs-extract rootfs-pack
$(rootfs_target): $(rootfs_archive)
	gzip -dc $< > $@

rootfs-extract: $(rootfs_target)
	rm -rf $(rootfs_srcdir)
	mkdir -p $(rootfs_srcdir)
	fakeroot sh -c 'cd $(rootfs_srcdir) && cpio -imdv < $(rootfs_target)'

rootfs-pack: $(rootfs_srcdir)
	rm -rf $(rootfs_target)
	fakeroot sh -c 'cd $(rootfs_srcdir) && find . | cpio -o -H newc > $(rootfs_target)'

###########
# linux
###########
.PHONY: linux linux-image
linux: $(linux_builddir)/.config $(rootfs_target)
	$(linux_srcdir)/scripts/config --file $(linux_builddir)/.config \
		--set-str INITRAMFS_SOURCE $(rootfs_target)
	$(MAKE) -C $(linux_srcdir) O=$(linux_builddir) -j $(NPROC) \
		ARCH=riscv CROSS_COMPILE=$(CROSS_COMPILE) olddefconfig
	$(MAKE) -C $(linux_srcdir) O=$(linux_builddir) -j $(NPROC) \
		ARCH=riscv CROSS_COMPILE=$(CROSS_COMPILE)

linux-image: $(linux_builddir)/.config $(rootfs_target)
	$(linux_srcdir)/scripts/config --file $(linux_builddir)/.config \
		--set-str INITRAMFS_SOURCE $(rootfs_target)
	$(MAKE) -C $(linux_srcdir) O=$(linux_builddir) -j $(NPROC) \
		ARCH=riscv CROSS_COMPILE=$(CROSS_COMPILE) olddefconfig
	$(MAKE) -C $(linux_srcdir) O=$(linux_builddir) -j $(NPROC) \
		ARCH=riscv CROSS_COMPILE=$(CROSS_COMPILE) Image

$(linux_builddir)/.config: $(linux_config) Makefile
	mkdir -p $(dir $@)
	$(MAKE) -C $(linux_srcdir) O=$(linux_builddir) -j $(NPROC) \
		ARCH=riscv CROSS_COMPILE=$(CROSS_COMPILE) defconfig
	$(linux_srcdir)/scripts/kconfig/merge_config.sh -m \
		-O $(linux_builddir) $@ $(linux_config)
	$(MAKE) -C $(linux_srcdir) O=$(linux_builddir) -j $(NPROC) \
		ARCH=riscv CROSS_COMPILE=$(CROSS_COMPILE) olddefconfig

###########
# DTB
###########
.PHONY: dtb
dtb: $(dtb_file)

$(dtb_file): $(dts_file)
	mkdir -p $(dir $@)
	dtc -I dts -O dtb -o $(dtb_file) $(dts_file)

###########
# OT-TEE
###########
.PHONY: optee_os
optee_os:
	mkdir -p $(optee_os_builddir)
	cp -rf $(optee_os_platdir) $(optee_os_srcdir)/core/arch/riscv/
	$(MAKE) -C $(optee_os_srcdir) O=$(optee_os_builddir) -j $(NPROC) \
	ARCH=riscv PLATFORM=nanhu
	rm -rf $(optee_os_srcdir)/core/arch/riscv/plat-nanhu

###########
# foreign-interrupt preemption test
###########
.PHONY: preempt-test preempt-test-install
preempt-test:
	test -f $(ta_dev_kit_dir)/mk/ta_dev_kit.mk || $(MAKE) optee_os
	mkdir -p $(preempt_test_builddir)/ta
	$(MAKE) -C $(preempt_test_srcdir)/ta \
	CROSS_COMPILE=$(CROSS_COMPILE) \
	TA_DEV_KIT_DIR=$(ta_dev_kit_dir) \
	O=$(preempt_test_builddir)/ta
	$(CROSS_COMPILE)gcc -static -O2 -Wall -Wextra \
	-I$(preempt_test_srcdir)/include \
	-o $(preempt_test_host) $(preempt_test_srcdir)/host/main.c

# Regenerate the base cpio from the tracked archive and append an overlay
# archive holding the test; the kernel unpacks concatenated cpio archives.
preempt-test-install: preempt-test
	rm -rf $(preempt_test_builddir)/overlay
	mkdir -p $(preempt_test_builddir)/overlay/lib/optee_armtz
	mkdir -p $(preempt_test_builddir)/overlay/usr/bin
	install -m 0444 $(preempt_test_ta) \
	$(preempt_test_builddir)/overlay/lib/optee_armtz/
	install -m 0755 $(preempt_test_host) \
	$(preempt_test_builddir)/overlay/usr/bin/
	gunzip -c $(rootfs_archive) > $(rootfs_target)
	cd $(preempt_test_builddir)/overlay && \
	find . -mindepth 1 | cpio -o -H newc -R 0:0 >> $(rootfs_target)

###########
# merge
###########
.PHONY: merge
merge: $(merge_payload_bin)

$(merge_payload_bin): $(opensbi_jump_bin) $(optee_os_bin) $(linux_image) 
	rm -rf $@
	dd if=/dev/zero of=$@ bs=1M count=128 status=none
	dd if=$(opensbi_jump_bin) of=$@ conv=notrunc
	dd if=$(optee_os_bin) of=$@ bs=1M seek=$$(($(optee_os_offset) / 0x100000)) conv=notrunc
	dd if=$(linux_image) of=$@ bs=1M seek=$$(($(linux_offset) / 0x100000)) conv=notrunc


###########
# opensbi
###########
.PHONY: opensbi-jump opensbi-payload
opensbi-jump: $(dtb_file) 
	mkdir -p $(opensbi_builddir)
	$(MAKE) -C $(opensbi_srcdir) O=$(opensbi_builddir) -j $(NPROC) \
	CROSS_COMPILE=$(CROSS_COMPILE) \
	PLATFORM=generic \
	FW_TEXT_START=$(opensbi_start) \
	FW_FDT_PATH=$(dtb_file) \
	FW_JUMP_FDT_ADDR=$(fdt_start) \
	FW_JUMP_ADDR=$(optee_os_start)

opensbi-payload: linux-image $(dtb_file)
	mkdir -p $(opensbi_builddir)
	$(MAKE) -C $(opensbi_srcdir) O=$(opensbi_builddir) -j $(NPROC) \
	CROSS_COMPILE=$(CROSS_COMPILE) \
	PLATFORM=generic \
	FW_TEXT_START=$(opensbi_start) \
	FW_FDT_PATH=$(dtb_file) \
	FW_PAYLOAD=y \
	FW_PAYLOAD_PATH=$(linux_image) \
	FW_PAYLOAD_ALIGN=0x100000 \
	FW_PAYLOAD_FDT_ADDR=$(fdt_start) \
	FW_PAYLOAD_OFFSET=0x2000000


##########
# run
##########
.PHONY: run-jump run-tee run-linux run-payload run-fpga
run-jump run-tee: qemu opensbi-jump optee_os linux-image
	$(qemu_target) $(qemu_machine) $(qemu_args) \
	-d guest_errors -D guest_log.txt \
	-bios $(opensbi_jump_bin) \
	-device loader,file=$(optee_os_bin),addr=$(optee_os_start) \
	-device loader,file=$(linux_image),addr=$(linux_start) \
	-nographic

run-linux run-payload: qemu opensbi-payload
	$(qemu_target) $(qemu_machine) $(qemu_args) \
	-d guest_errors -D guest_log.txt \
	-bios $(opensbi_payload_bin) \
	-nographic

run-fpga: $(merge_payload_bin)
	$(qemu_target) $(qemu_machine) $(qemu_args) \
	-d guest_errors -D guest_log.txt \
	-bios $(merge_payload_bin) \
	-nographic



##########
# debug
##########
.PHONY: debug
debug: $(merge_payload_bin)
	$(qemu_target) $(qemu_machine) $(qemu_args) \
	-d guest_errors -D guest_log.txt \
	-bios $(merge_payload_bin) \
	-nographic \
	-s -S \
	# -bios $(opensbi_jump_elf) \
	# -device loader,file=$(optee_os_bin),addr=$(optee_os_start) \
	# -device loader,file=$(linux_image),addr=$(linux_start) \

###########
# clean
###########
.PHONY: qemu-clean qemu-distclean linux-clean linux-distclean optee_os-clean dtb-clean opensbi-clean preempt-test-clean merge-clean clean-all
qemu-clean:
	$(MAKE) -C $(qemu_builddir) clean

qemu-distclean:
	rm -rf $(qemu_builddir)

linux-clean:
	$(MAKE) -C $(linux_builddir) clean

linux-distclean:
	rm -rf $(linux_builddir)

optee_os-clean:
	rm -rf $(optee_os_builddir)
	rm -rf $(optee_os_srcdir)/core/arch/riscv/plat-nanhu

dtb-clean:
	rm -f $(dtb_file)

opensbi-clean:
	rm -rf $(opensbi_builddir)

preempt-test-clean:
	rm -rf $(preempt_test_builddir)

merge-clean:
	rm -rf $(merge_payload_bin)

clean-all:
	rm -rf $(qemu_builddir)
	rm -rf $(linux_builddir)
	rm -rf $(optee_os_builddir)
	rm -rf $(optee_os_srcdir)/core/arch/riscv/plat-nanhu
	rm -f $(dtb_file)
	rm -rf $(opensbi_builddir)
	rm -rf $(preempt_test_builddir)
	rm -rf $(merge_payload_bin)
