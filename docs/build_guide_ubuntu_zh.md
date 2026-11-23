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
- 模块位于构建目录（只有需要从 rootfs 加载模块时才需要 `modules_install`）

## 4. 安装为一次性 GRUB 测试项

**不要**改动 5.10 内核、它的 rootfs，以及 GRUB 默认项。

```sh
sudo mkdir -p /boot/linux-6.18-charlotte
sudo cp arch/arm64/boot/Image /boot/linux-6.18-charlotte/Image
sudo cp arch/arm64/boot/dts/hisilicon/charlotte-9000c.dtb \
        /boot/linux-6.18-charlotte/charlotte-9000c.dtb
sudo cp /path/to/initramfs-console.cpio.gz \
        /boot/linux-6.18-charlotte/initramfs-console.cpio.gz
```

已有的 GRUB 菜单项如下（`/etc/grub.d/40_custom`）：

```
menuentry "Linux 6.18 charlotte bring-up (TEST - uart console)" {
    linux  /linux-6.18-charlotte/Image \
        earlycon=uart_v500,0xff894000 console=ttyHW0,115200n8 \
        loglevel=7 arm64.nopauth panic=20
    devicetree /linux-6.18-charlotte/charlotte-9000c.dtb
    initrd /linux-6.18-charlotte/initramfs-console.cpio.gz
}
```

单次启动：

```sh
sudo grub-reboot "Linux 6.18 charlotte bring-up (TEST - uart console)"
sudo reboot
```

默认项仍然是 5.10，所以普通重启会回到厂商内核。

## 5. 串口控制台

`/dev/ttyUSB0`，115200 8N1。它是单读者设备，先结束残留的读取进程：

```sh
pkill -f 'cat /dev/ttyUSB0' || true
sg dialout -c 'stty -F /dev/ttyUSB0 115200 raw -echo -crtscts -ixon -ixoff;
               timeout 900 cat /dev/ttyUSB0'
```

`scripts/local/serial_console.sh` 封装了上述操作。6.18 initramfs 会启动
busybox shell；`sdd2-chroot` 会以只读方式挂载 GXDE rootfs 并 chroot 进去。

## 6. 安全注意事项

- 保留启动目录里的 `Image.safe` / `Image.ufsworking` / `Image.pre-pcie`
  作为回滚点。
- 介质上唯一可写的临时区域是 `sdd` 的 1 MiB 未分配区（扇区 256-511），
  做写入测试前后务必保存/恢复。
- 6.18 冷启动时不要读未上电的显示或 PCIe1 MMIO：会触发 NoC SError 并复位板子。
