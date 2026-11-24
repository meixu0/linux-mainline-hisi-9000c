# tools/

On-device helpers used during bring-up. Build them on the board or with an
aarch64 cross toolchain.

## charlotte-diag

Freestanding aarch64 MMIO tool used instead of `/dev/mem` when servicing
registers that must not be touched by a normal process.

```sh
make -C tools charlotte-diag
# or: aarch64-linux-gnu-gcc -static -O2 -o charlotte-diag charlotte-diag.c
sudo ./charlotte-diag dump 0xffb85000 0x100
sudo ./charlotte-diag dump 0xfb21b000 0x400
```

Subcommands: `dump`, `blocks`, `ranges`, `find`, `fill`, `fbbars`, `fbinfo`.

> Always stop before reading an unpowered block. On a cold 6.18 boot the DPU
> window (`0xe8800000`) and the PCIe1 DBI (`0xedc00000`) raise a NoC SError.

## sdd2-chroot

initramfs helper that mounts the GXDE OS 25 rootfs on `sdd2` read-only
(`ro,noload`) and chroots into it. Read-write is opt-in:

```sh
sdd2-chroot        # read-only, safe default
sdd2-chroot rw     # read-write, only when you really need it
```

## net-dhcp

initramfs helper for the USB Ethernet adapter (Realtek RTL8153, mainline
`r8152`).  It brings the link up first (otherwise `udhcpc` can spin on
"Network is down"), gets a lease with a 30 s timeout, and writes
`/etc/resolv.conf` from the DHCP `dns` option:

```sh
net-dhcp            # eth0
net-dhcp eth1       # other interface name
```

The initramfs must contain `/bin/sh`, the `rtl_nic/*.fw` firmware and the
script itself; `initramfs-console-init.sh` creates `/bin/sh` at boot.

## Initramfs scripts

- `initramfs-console-init.sh`: mounts `proc`/`sys`/`dev`, prints the chroot
  hint, then runs `while :; do sh -i; done` so a dropped serial reader cannot
  kill the shell.
- `initramfs-diag-init.sh`: early framebuffer/MMIO diagnostic init used during
  the display recon (writes a test pattern and dumps safe registers).

Build a console initramfs:

```sh
mkdir -p initramfs/{bin,dev,proc,sys,etc,lib/firmware/rtl_nic}
cp "$(command -v busybox)" initramfs/bin/
ln -sf busybox initramfs/bin/sh
install -m755 initramfs-console-init.sh initramfs/init
install -m755 net-dhcp initramfs/net-dhcp
cp /lib/firmware/rtl_nic/rtl8153*.fw initramfs/lib/firmware/rtl_nic/
(cd initramfs && find . | cpio -o -H newc | gzip -9) > initramfs-console.cpio.gz
```
