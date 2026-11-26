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
- modules under the build tree (`modules_install` stages them for packaging)
