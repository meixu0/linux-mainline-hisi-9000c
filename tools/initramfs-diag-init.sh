#!/bin/busybox sh
# Charlotte 6.18 bring-up diagnostic init.
#
# The display is black in 6.18 (GOP/efifb handover does not survive), so the
# only reliable channel is the polled earlycon kept alive by keep_bootcon.
# Userspace writes to /dev/kmsg are echoed to every registered console,
# including that boot console, so we log through /dev/kmsg only.
#
# Nothing writes to MMIO until PHASE 2, which runs after the screen test.

export PATH=/bin
BB=/bin/busybox
K() { echo "<6>DIAG: $*" > /dev/kmsg 2>/dev/null; }

$BB mkdir -p /proc /sys /dev /tmp
$BB mount -t proc proc /proc
$BB mount -t sysfs sysfs /sys
$BB mount -t devtmpfs devtmpfs /dev 2>/dev/null
$BB mount -t tmpfs tmpfs /tmp 2>/dev/null
$BB mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
$BB mkdir -p /dev/pts
$BB mount -t devpts devpts /dev/pts 2>/dev/null

# devtmpfs should provide all of these; create them by hand if it did not.
$BB [ -e /dev/kmsg ]    || $BB mknod /dev/kmsg c 1 11
$BB [ -e /dev/mem ]     || $BB mknod /dev/mem c 1 1
$BB [ -e /dev/null ]    || $BB mknod /dev/null c 1 3
$BB [ -e /dev/console ] || $BB mknod /dev/console c 5 1
$BB [ -e /dev/fb0 ]     || $BB mknod /dev/fb0 c 29 0

K "=================================================="
K " charlotte 6.18 DIAG init (serial via /dev/kmsg)"
K "=================================================="
K "cmdline: $($BB cat /proc/cmdline 2>/dev/null)"
K "uname:   $($BB uname -a 2>/dev/null)"
K "nproc:   $($BB nproc 2>/dev/null)"
K "/proc/fb: $($BB cat /proc/fb 2>/dev/null)"
K "cpu online: $($BB cat /sys/devices/system/cpu/online 2>/dev/null)"
K "cpu count:  $($BB grep -c ^processor /proc/cpuinfo 2>/dev/null)"
K "/dev: $($BB ls /dev 2>/dev/null | $BB tr '\n' ' ')"

for f in name modes stride virtual_size bits_per_pixel state blank; do
	[ -f /sys/class/graphics/fb0/$f ] && K "fb0/$f = $($BB cat /sys/class/graphics/fb0/$f)"
done

K "--- fbcon content analysis (before we touch it) ---"
/diag fbinfo

K "--- PHASE 1: 8 color bars on /dev/fb0 ---"
/diag fbbars 1920 1080 || K "fbbars FAILED"

K "##################################################"
K "#  LOOK AT THE HDMI SCREEN:                       #"
K "#  8 vertical bars R G B W Y C M K (60 s)        #"
K "#  bars visible -> GOP buffer is still scanned   #"
K "#  still black  -> DPU scanout is dead/elsewhere #"
K "##################################################"
$BB sleep 60

K "--- PHASE 2: read-only DPU register scan ---"
K "if the board resets from here, the last line says where"

K "== 0xff7cc000 (disp ch1 subsys), 8K =="
/diag blocks 0xff7cc000 0x2000
/diag dump   0xff7cc000 0x80

K "== 0xff7c9000, 4K =="
/diag blocks 0xff7c9000 0x1000
/diag dump   0xff7c9000 0x80

K "== 0xe8800000 (DPU), 1M: per-4K activity map =="
/diag blocks 0xe8800000 0x100000

K "== scan 0xe8800000 for [0x50000000,0x54800000) =="
/diag ranges 0xe8800000 0x100000 0x50000000 0x54800000

K "== scan 0xe8800000 for 0x54017000 (efifb base) =="
/diag find 0xe8800000 0x100000 0x54017000

K "== scan 0xe8800000 for [0x800000000,0x820000000) =="
/diag ranges 0xe8800000 0x100000 0x800000000 0x820000000

K "== scan 0xe8800000 for IOVA-like [0x100000,0x7fffffff) =="
/diag ranges 0xe8800000 0x100000 0x100000 0x7fffffff

K "--- DIAG COMPLETE ---"
K "reboot in 15 min (or power-cycle). CPU online: $($BB cat /sys/devices/system/cpu/online 2>/dev/null)"
$BB sleep 900
K "rebooting now"
$BB reboot -f
