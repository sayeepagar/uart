#!/usr/bin/env bash
# Boot the demo initramfs in QEMU.
#   COM1 (0x3f8, IRQ 4) -> your terminal, Linux console   (kernel 8250 driver)
#   COM2 (0x2f8, IRQ 3) -> TCP port 5555 on the host      (myuart driver)
#
#   scripts/run_qemu.sh              interactive
#   scripts/run_qemu.sh --autotest   run the selftest unattended, exit 0/1
#
# Environment:
#   KERNEL   kernel image (default: build/linux bzImage, else /boot/vmlinuz-$(uname -r))
#   SERIAL2  QEMU -serial argument for COM2 (default: TCP server on HOST_PORT).
#            e.g. SERIAL2=/dev/ttyACM0 to talk to a real board (see docs/STM32_PORT.md)
#   HOST_PORT  default 5555
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build}
INITRD=$BUILD/initramfs.cpio.gz
HOST_PORT=${HOST_PORT:-5555}
SERIAL2=${SERIAL2:-tcp:127.0.0.1:$HOST_PORT,server,nowait}
AUTOTEST=0
[ "${1:-}" = "--autotest" ] && AUTOTEST=1

if [ -z "${KERNEL:-}" ]; then
	if [ -f "$BUILD/linux/arch/x86/boot/bzImage" ]; then
		KERNEL=$BUILD/linux/arch/x86/boot/bzImage
	else
		KERNEL=/boot/vmlinuz-$(uname -r)
	fi
fi
if [ ! -r "$KERNEL" ]; then
	echo "cannot read kernel image: $KERNEL" >&2
	echo "  Ubuntu keeps it root-only: sudo chmod a+r $KERNEL" >&2
	echo "  or build one:  make kernel   (or set KERNEL=/path/to/bzImage)" >&2
	exit 1
fi
[ -f "$INITRD" ] || { echo "missing $INITRD - run 'make initramfs'" >&2; exit 1; }
command -v qemu-system-x86_64 >/dev/null || {
	echo "qemu-system-x86_64 not found: sudo apt install qemu-system-x86" >&2; exit 1; }

if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
	ACCEL=(-accel kvm -cpu host)
else
	ACCEL=(-accel tcg -cpu max)
	echo "note: no KVM, using software emulation (slower, still fine)" >&2
fi

# 8250.nr_uarts=1 keeps the kernel's own serial driver off COM2 so myuart can have it
CMDLINE="console=ttyS0 8250.nr_uarts=1 loglevel=6 panic=1 rdinit=/init"

COMMON=(-machine pc -m 512 "${ACCEL[@]}" -no-reboot
        -kernel "$KERNEL" -initrd "$INITRD")

if [ "$AUTOTEST" = 1 ]; then
	LOG=$BUILD/autotest.log
	: > "$LOG"
	timeout 240 qemu-system-x86_64 "${COMMON[@]}" \
		-append "$CMDLINE autotest" \
		-display none -monitor none \
		-serial "file:$LOG" -serial null || true
	cat "$LOG"
	if grep -q 'MYUART_AUTOTEST: PASS' "$LOG"; then
		echo "==> autotest PASSED"; exit 0
	fi
	echo "==> autotest FAILED (log: $LOG)"; exit 1
fi

cat >&2 <<EOF
COM1 = this terminal (Linux console). COM2 = $SERIAL2
Host side:  nc 127.0.0.1 $HOST_PORT      Quit QEMU: Ctrl-A then X
EOF
exec qemu-system-x86_64 "${COMMON[@]}" \
	-append "$CMDLINE" \
	-display none \
	-serial mon:stdio -serial "$SERIAL2"
