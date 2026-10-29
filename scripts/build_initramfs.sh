#!/usr/bin/env bash
# Pack myuart.ko + myuart_test + busybox into build/initramfs.cpio.gz
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build}
STAGE=$BUILD/initramfs

BB=${BUSYBOX:-$(command -v busybox || true)}
if [ -z "$BB" ] || [ ! -x "$BB" ]; then
	echo "busybox not found. Install a static one: sudo apt install busybox-static" >&2
	exit 1
fi
LDD_OUT=$(ldd "$BB" 2>&1 || true)	# captured first: grep -q + pipefail would misfire
case "$LDD_OUT" in
	*"not a dynamic"*|*"statically linked"*) ;;
	*) echo "warning: $BB is dynamically linked; use busybox-static or set BUSYBOX=" >&2 ;;
esac
for f in driver/myuart.ko app/myuart_test; do
	[ -f "$ROOT/$f" ] || { echo "missing $f - run 'make' first" >&2; exit 1; }
done

rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,sbin,proc,sys,dev,etc,root,tmp}

cp "$BB" "$STAGE/bin/busybox"
for applet in sh mount umount insmod rmmod lsmod cat echo ls dmesg sleep \
              poweroff reboot grep head tail sed awk dd od hexdump uname \
              mkdir mknod kill ps id true false printf; do
	ln -sf busybox "$STAGE/bin/$applet"
done

cp "$ROOT/driver/myuart.ko"     "$STAGE/myuart.ko"
cp "$ROOT/app/myuart_test"      "$STAGE/bin/myuart_test"
install -m 0755 "$ROOT/scripts/init.sh" "$STAGE/init"

( cd "$STAGE" && find . -print0 | cpio --null -o -H newc -R 0:0 --quiet ) \
	| gzip -9 > "$BUILD/initramfs.cpio.gz"

echo "initramfs: $BUILD/initramfs.cpio.gz ($(du -h "$BUILD/initramfs.cpio.gz" | cut -f1))"
