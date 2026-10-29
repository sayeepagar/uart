#!/bin/sh
# /init of the demo initramfs: mount the pseudo filesystems, load myuart,
# then either run the selftest unattended or drop into a shell.

export PATH=/bin:/sbin

mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev

echo
echo "=== myuart demo ($(uname -r)) ==="

if insmod /myuart.ko; then
	echo "module loaded; node: $(ls /dev/myuart0 2>/dev/null || echo MISSING)"
else
	echo "insmod FAILED, kernel log:"
	dmesg | tail -n 15
fi

if grep -q autotest /proc/cmdline; then
	if myuart_test selftest; then
		echo "MYUART_AUTOTEST: PASS"
	else
		echo "MYUART_AUTOTEST: FAIL"
		dmesg | grep -i myuart
	fi
	reboot -f	# QEMU runs with -no-reboot, so this ends the VM
fi

cat <<'EOF'

Try:
  myuart_test selftest          loopback test, no wiring needed
  cat /proc/myuart              live driver statistics
  myuart_test stats
  myuart_test recv 60           then type into `nc 127.0.0.1 5555` on the host
  myuart_test send hello        shows up in that nc window
  myuart_test echo              host-typed text is echoed back
Quit QEMU with Ctrl-A then X.

EOF

sh
reboot -f
