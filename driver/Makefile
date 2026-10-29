# Kbuild file for the out-of-tree module.
# Usage:  make KDIR=/path/to/kernel/build/tree     (default: running kernel)

obj-m += myuart.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
