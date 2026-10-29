# Top-level convenience Makefile.
#
#   make            build the kernel module and the test app
#   make run        build, pack an initramfs, boot it in QEMU (interactive)
#   make test       same, but run the selftest unattended and exit PASS/FAIL
#   make kernel     optional: build a small known-good kernel (see README)
#   make clean

BUILD := $(CURDIR)/build

# Build against the kernel QEMU will boot: the custom one from `make kernel`
# if it exists, otherwise the kernel running on this machine.
ifneq ($(wildcard $(BUILD)/linux/arch/x86/boot/bzImage),)
KDIR ?= $(BUILD)/linux
else
KDIR ?= /lib/modules/$(shell uname -r)/build
endif

.PHONY: all driver app initramfs run test kernel clean

all: driver app

driver:
	$(MAKE) -C driver KDIR=$(KDIR)

app:
	$(MAKE) -C app

initramfs: all
	bash scripts/build_initramfs.sh

run: initramfs
	bash scripts/run_qemu.sh

test: initramfs
	bash scripts/run_qemu.sh --autotest

kernel:
	bash scripts/build_kernel.sh

clean:
	-$(MAKE) -C driver KDIR=$(KDIR) clean
	-$(MAKE) -C app clean
	rm -rf $(BUILD)/initramfs $(BUILD)/initramfs.cpio.gz $(BUILD)/autotest.log
