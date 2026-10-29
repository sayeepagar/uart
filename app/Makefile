# Static build so the binary runs inside the tiny busybox initramfs.
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
LDFLAGS ?= -static

myuart_test: myuart_test.c ../include/myuart_ioctl.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f myuart_test

.PHONY: clean
