[中文](build_guide_ubuntu_zh.md) | English

# Build guide (Ubuntu/Debian on the board)

All commands below run **on the board itself** (aarch64 native build, 8c12t / 12 logical CPUs).
Building on a cross-compile host works too; only the `make` invocations change.

## 0. Prerequisites

```sh
sudo apt-get update
sudo apt-get install -y build-essential bison flex bc libssl-dev libelf-dev \
    device-tree-compiler cpio kmod git
```

## 1. Fetch the kernel and apply the series

```sh
git clone --depth 1 --branch v6.18 \
    https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git linux-6.18
cd linux-6.18

git am /path/to/linux-mainline-hisi-9000c/patches/*.patch
```

The series is ordered (`0001` .. `0007`) and applies cleanly on a pristine
`v6.18`.

## 2. Configure

```sh
cp /path/to/linux-mainline-hisi-9000c/defconfig/charlotte_6.18_defconfig \
   arch/arm64/configs/charlotte_defconfig
make charlotte_defconfig
```

The fragment enables the UART console, UFS (`SCSI_UFS_CHARLOTTE`), PCIe
(`PCIE_CHARLOTTE`), the onboard SATA (`SATA_AHCI`) and amdgpu
(`DRM_AMDGPU`, built-in).

## 3. Build

```sh
make -j12 Image dtbs modules
```

Artifacts:

- `arch/arm64/boot/Image`
- `arch/arm64/boot/dts/hisilicon/charlotte-9000c.dtb`
- modules under the build tree (`modules_install` is only needed when you want
  to boot a rootfs that loads modules)

## 4. Install as a one-shot GRUB test entry

Do **not** touch the 5.10 kernel, its rootfs, or the GRUB default entry.

```sh
sudo mkdir -p /boot/linux-6.18-charlotte
sudo cp arch/arm64/boot/Image /boot/linux-6.18-charlotte/Image
sudo cp arch/arm64/boot/dts/hisilicon/charlotte-9000c.dtb \
        /boot/linux-6.18-charlotte/charlotte-9000c.dtb
sudo cp /path/to/initramfs-console.cpio.gz \
        /boot/linux-6.18-charlotte/initramfs-console.cpio.gz
```

The existing GRUB menu entry looks like this (`/etc/grub.d/40_custom`):

```
menuentry "Linux 6.18 charlotte bring-up (TEST - uart console)" {
    linux  /linux-6.18-charlotte/Image \
        earlycon=uart_v500,0xff894000 console=ttyHW0,115200n8 \
        loglevel=7 arm64.nopauth panic=20
    devicetree /linux-6.18-charlotte/charlotte-9000c.dtb
    initrd /linux-6.18-charlotte/initramfs-console.cpio.gz
}
```

Boot it once:

```sh
sudo grub-reboot "Linux 6.18 charlotte bring-up (TEST - uart console)"
sudo reboot
```

The default entry stays on 5.10, so a plain reboot returns to the vendor kernel.

## 5. Serial console

`/dev/ttyUSB0` at 115200 8N1. It is a single-reader device; stop any stale
reader first:

```sh
pkill -f 'cat /dev/ttyUSB0' || true
sg dialout -c 'stty -F /dev/ttyUSB0 115200 raw -echo -crtscts -ixon -ixoff;
               timeout 900 cat /dev/ttyUSB0'
```

`scripts/local/serial_console.sh` wraps this. The 6.18 initramfs starts a
busybox shell; `sdd2-chroot` mounts the GXDE rootfs read-only and chroots in.

## 6. Safety notes

- Keep `Image.safe` / `Image.ufsworking` / `Image.pre-pcie` in the boot
  directory as rollback points.
- The only writable scratch area on the medium is the 1 MiB unallocated region
  on `sdd` (sectors 256-511); save and restore it around any write test.
- On a cold 6.18 boot, do not read unpowered display or PCIe1 MMIO: it raises a
  NoC SError and resets the board.
