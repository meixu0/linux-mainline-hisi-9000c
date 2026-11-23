[English](../README.md) | 中文

# linux-mainline-hisi-9000c

面向华为麒麟 9000C（代号 `charlotte`，板卡 `QHW-100` / `SP1PANGUXM`）单板计算机的
Linux 6.18 主线移植：构建脚本、补丁、内核配置、设备树和工具集。

板子通过 UEFI -> GRUB 链式启动。厂商 5.10 内核（`5.10.97-38-9000c`）保持为默认启动项，
6.18 镜像是手动的一次性测试项（`grub-reboot`）。

## 包含内容

### 仓库结构

- `patches/`：6.18 内核补丁系列（`git am` 可直接应用，基线为 `v6.18`）
- `defconfig/`：6.18 构建使用的本地内核配置
- `drivers/`：补丁系列新增/修改的驱动源码本地镜像
- `dts/`：补丁系列使用的板级 DTS/DTSI 本地镜像
- `docs/`：移植笔记、构建指南和串口日志
- `firmware/`：固件说明（6.18 不需要额外的板级固件 blob）
- `packaging/`：移植期间使用的厂商参考包说明
- `tools/`：板端辅助工具（MMIO 诊断、initramfs 脚本、chroot 助手）
- `scripts/ci/`：非交互式构建脚本
- `scripts/local/`：在板子上直接运行的脚本

### 功能状态

| 功能 | 5.10 厂商内核 | 6.18 主线 |
|---|---|---|
| 8c12t SMP 启动（8 核 12 线程） | 是 | 是 |
| 串口控制台（`uart_v500`） | 是 | 是 |
| UFS（3 个 LUN，43 个分区） | 是 | 是，HS-G4 / 2 lanes（读约 931 MB/s） |
| PCIe0（板载 ASM1064 SATA） | 是 | 是，Gen3 x1 + `ahci` |
| PCIe1（外部 x1 插槽） | 是 | 驱动就绪，待插槽电源使能后打开 |
| amdgpu | 未编入 | 已编入，需要插卡 |
| 显示（厂商 DPU/HDMI） | 是 | 尚未（只有厂商 `dkmd`） |
| initramfs 进入 rootfs chroot | — | 是（`sdd2-chroot`，默认只读） |

### 补丁来源

全部补丁都是本板子的本地工作，参考来源：

- 板子运行时的厂商设备树（`dtc -I fs -O dts /proc/device-tree`）；
- 华为手机内核源码包（`HBN-AL00,HBN-AL10,HBN-AL80`），重点是
  `platform_source/basicplatform/drivers/{pci,clk,gpio}` 和
  `drivers/video/fbdev/dkmd`。

**不要**参考 `hi3660`/`hi3670`：Kirin 9000C 的 UFS/PCIe/时钟模块与它们不同。

- `0001`：加入 QHW-100（charlotte）板级 DTS 和 SoC 骨架
- `0002`：加入 `uart_v500` 串口控制台驱动（可交互）
- `0003`：加入只读的 DSS 电源探测驱动（显示侦察用）
- `0004`：为主线 UFS core 增加 HUF S 每核队列支持
- `0005`：加入 `ufshcd-charlotte` 变体驱动
- `0006`：通过私有 UniPro 窗口协商 HS-G4
- `0007`：加入 DesignWare PCIe host glue（`pcie-charlotte`）

### 工具

- `tools/charlotte-diag.c`：aarch64 静态 MMIO dump 工具（`dump`、`blocks`、
  `ranges`、`find`、`fill`、`fbbars`、`fbinfo`）
- `tools/sdd2-chroot`：initramfs 中挂载 rootfs（默认只读）并 chroot 的助手
- `tools/initramfs-console-init.sh`：带 shell 重生循环的 initramfs init
- `tools/initramfs-diag-init.sh`：早期 framebuffer/MMIO 诊断 init

## 启动产物布局

6.18 测试项使用独立目录，而不走发行版的 `kernel-install` 流程：

```
/boot/linux-6.18-charlotte/
├── Image                      # 内核（未压缩 arm64 Image）
├── Image.pre-pcie             # PCIe 系列之前的回滚镜像
├── Image.safe                 # 早期已知可用镜像
├── Image.ufsworking           # UFS 可用版镜像
├── charlotte-9000c.dtb        # 匹配的设备树
└── initramfs-console.cpio.gz  # 带 sdd2-chroot 的 busybox initramfs
```

GRUB 条目名为 `Linux 6.18 charlotte bring-up (TEST - uart console)`，
只通过 `grub-reboot` 选择；GRUB 默认项必须保持在 5.10。

## 快速开始

- 构建指南（板端 Ubuntu/Debian）：[English](build_guide_ubuntu_en.md) | [中文](build_guide_ubuntu_zh.md)
- UFS 移植笔记：[中文](ufs_bringup_zh.md)
- PCIe / amdgpu 笔记：[中文](pci_amdgpu_zh.md)
- 显示移植计划：[中文](display_bringup_zh.md)
- 补丁集与 rootfs chroot：[中文](rootfs_chroot_zh.md)
- 完整移植报告：[中文](bringup_report_zh.md)
- 串口日志：[docs/logs](logs/)

## 安全注意事项

- 不要修改 GRUB 默认项，也不要覆盖 5.10 内核/rootfs。
- 板上 `sdd` 只有 1 MiB 未分配区（扇区 256-511）可用于写入测试，务必先保存再恢复。
- `/dev/ttyUSB0` 是单读者：抓串口前先结束残留的 `cat`。
- 6.18 冷启动时不要读未上电的显示/PCIe MMIO 块，会触发 NoC SError 并复位。

## 参考

- 华为手机内核源码包（`HBN-AL00,HBN-AL10,HBN-AL80`）：厂商 UFS、kport PCIe、
  extreme CRG 和 dkmd 显示实现
- 板子运行时的厂商设备树（QHW-100）
