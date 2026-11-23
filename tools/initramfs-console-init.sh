#!/bin/busybox sh
# Charlotte 6.18 UART console bring-up init.
#
# console=ttyHW0 is the only console on this entry, so /dev/console is the
# V500 UART: printk and this shell both go to the serial port.

BB=/bin/busybox

$BB mkdir -p /proc /sys /dev /tmp
$BB mount -t proc proc /proc
$BB mount -t sysfs sysfs /sys
$BB mount -t devtmpfs devtmpfs /dev 2>/dev/null
$BB mount -t tmpfs tmpfs /tmp 2>/dev/null
$BB [ -e /dev/kmsg ] || $BB mknod /dev/kmsg c 1 11
$BB [ -e /dev/ttyHW0 ] || $BB mknod /dev/ttyHW0 c 4 64

echo ""
echo "=============================================="
echo " 6.18 CHARLOTTE: UART CONSOLE IS ALIVE (ttyHW0)"
echo "=============================================="
$BB uname -a
echo "cmdline: $($BB cat /proc/cmdline)"
echo "cpus:    $($BB cat /sys/devices/system/cpu/online 2>/dev/null)"
echo "proc/fb: $($BB cat /proc/fb 2>/dev/null)"
echo "tty:     $($BB ls /dev/ttyHW* 2>/dev/null)"
echo ""

exec $BB sh -i </dev/ttyHW0 >/dev/ttyHW0 2>&1
