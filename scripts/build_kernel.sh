#!/usr/bin/env bash
# OPTIONAL: build a minimal, known-good x86_64 kernel into build/linux.
# Use it when your host kernel has no headers (e.g. WSL2) or you want a
# predictable setup. Afterwards `make` and `make run` pick it up automatically.
# A tinyconfig-based kernel builds in a few minutes.
#
# Needs: gcc make flex bison bc libssl-dev libelf-dev xz-utils curl
#   sudo apt install build-essential flex bison bc libssl-dev libelf-dev xz-utils curl
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build}
KVER=${KVER:-6.6}
TARBALL=$BUILD/linux-$KVER.tar.xz
SRC=$BUILD/linux

mkdir -p "$BUILD"
if [ ! -f "$SRC/Makefile" ]; then
	[ -f "$TARBALL" ] || curl -L --fail -o "$TARBALL" \
		"https://cdn.kernel.org/pub/linux/kernel/v${KVER%%.*}.x/linux-$KVER.tar.xz"
	mkdir -p "$SRC"
	tar -xf "$TARBALL" -C "$SRC" --strip-components=1
fi

cd "$SRC"
make tinyconfig
# tinyconfig is almost empty: switch on just what the demo needs
./scripts/config \
	-e 64BIT -e PRINTK -e TTY \
	-e SERIAL_8250 -e SERIAL_8250_CONSOLE \
	-e BLK_DEV_INITRD -e RD_GZIP \
	-e MODULES -e MODULE_UNLOAD \
	-e PROC_FS -e SYSFS -e DEVTMPFS \
	-e BINFMT_ELF -e BINFMT_SCRIPT \
	-e FUTEX -e POSIX_TIMERS -e EPOLL -e MULTIUSER
make olddefconfig
make -j"$(nproc)" bzImage
make -j"$(nproc)" modules	# also produces Module.symvers, needed by out-of-tree builds

echo
echo "kernel:  $SRC/arch/x86/boot/bzImage"
echo "KDIR:    $SRC   (make picks this up automatically)"
