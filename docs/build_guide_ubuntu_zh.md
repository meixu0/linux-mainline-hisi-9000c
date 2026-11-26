[English](build_guide_ubuntu_en.md) | 中文

# 构建指南（板端 Ubuntu/Debian）

下面的命令都在**板子本机**执行（aarch64 原生编译，8c12t / 12 个逻辑 CPU）。在交叉编译主机上
构建也可以，只需要改 `make` 的调用方式。

## 0. 依赖

```sh
sudo apt-get update
sudo apt-get install -y build-essential bison flex bc libssl-dev libelf-dev \
    device-tree-compiler cpio kmod git
```

## 1. 获取内核并应用补丁

```sh
git clone --depth 1 --branch v6.18 \
    https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git linux-6.18
cd linux-6.18

git am /path/to/linux-mainline-hisi-9000c/patches/*.patch
```

补丁按 `0001` .. `0007` 顺序排列，可在干净的 `v6.18` 上直接应用。

## 2. 配置

```sh
cp /path/to/linux-mainline-hisi-9000c/defconfig/charlotte_6.18_defconfig \
   arch/arm64/configs/charlotte_defconfig
make charlotte_defconfig
```

该配置片段启用了串口控制台、UFS（`SCSI_UFS_CHARLOTTE`）、PCIe
（`PCIE_CHARLOTTE`）、板载 SATA（`SATA_AHCI`）以及 amdgpu
（`DRM_AMDGPU`，内置）。

## 3. 编译

```sh
make -j12 Image dtbs modules
```

产物：

- `arch/arm64/boot/Image`
- `arch/arm64/boot/dts/hisilicon/charlotte-9000c.dtb`
- 模块位于构建目录（`modules_install` 用于打包/安装）
