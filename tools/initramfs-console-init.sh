#!/bin/busybox sh
# Charlotte 6.18 UART console bring-up init.
#
# console=ttyHW0 is the only console on this entry, so /dev/console is the
# V500 UART: printk and this shell both go to the serial port.

BB=/bin/busybox

$BB mkdir -p /proc /sys /dev /tmp /etc
$BB mount -t proc proc /proc
$BB mount -t sysfs sysfs /sys
$BB mount -t devtmpfs devtmpfs /dev 2>/dev/null
$BB mount -t tmpfs tmpfs /tmp 2>/dev/null
$BB [ -e /dev/kmsg ] || $BB mknod /dev/kmsg c 1 11
$BB [ -e /dev/ttyHW0 ] || $BB mknod /dev/ttyHW0 c 4 64
# helpers (udhcpc, net-dhcp) need a shell at the usual path
$BB [ -e /bin/sh ] || $BB ln -sf /bin/busybox /bin/sh

echo ""
echo "=============================================="
echo " 6.18 CHARLOTTE: UART CONSOLE IS ALIVE (ttyHW0)"
echo "=============================================="
$BB uname -a
echo "cmdline: $($BB cat /proc/cmdline)"
echo "cpus:    $($BB cat /sys/devices/system/cpu/online 2>/dev/null)"
echo "proc/fb: $($BB cat /proc/fb 2>/dev/null)"
echo "tty:     $($BB ls /dev/ttyHW* 2>/dev/null)"
echo "net:     $($BB ls /sys/class/net 2>/dev/null | $BB tr '\n' ' ')"
echo ""

echo "Installed rootfs on /dev/sdd2:"
echo "  sdd2-chroot         mount read-only and chroot in (safe)"
echo "  sdd2-chroot rw      mount read-write and chroot in"
echo "  net-dhcp            bring up eth0 via DHCP (USB RTL8153)"
echo ""

# Keep PID 1 alive and the console readable: respawn the shell when it
# exits.  PID 1 must never exit (it panics the kernel with
# "Attempted to kill init").  TTIN/TTOU are ignored in the parent so the
# shell cannot be stopped by job-control terminal bookkeeping.
#
# With job control enabled the shell needs a controlling terminal for
# Ctrl-C to reach the foreground job (otherwise a stuck helper cannot be
# interrupted over the serial line).  setsid -c is used when available and
# the plain shell is the fallback.
trap '' TTIN TTOU
while :; do
	if $BB setsid -c true >/dev/null 2>&1; then
		$BB setsid -c $BB sh -i </dev/ttyHW0 >/dev/ttyHW0 2>&1
	else
		$BB sh -i </dev/ttyHW0 >/dev/ttyHW0 2>&1
	fi
	echo "" >/dev/ttyHW0
	echo "[init] shell exited, respawning in 1s (PID 1 stays alive)" >/dev/ttyHW0
	$BB sleep 1
done
