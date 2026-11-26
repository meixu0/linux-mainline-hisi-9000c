[English](../README.md) | 中文

# linux-mainline-hisi-9000c

面向华为麒麟 9000C（代号 `charlotte`，板卡 `QHW-100` / `SP1PANGUXM`）单板计算机的
Linux 6.18 主线移植：构建脚本、补丁、内核配置、设备树和工具集。

厂商 5.10 内核（`5.10.97-38-9000c`）保持为默认；6.18 是独立的测试镜像。

## 包含内容

### 仓库结构

- `patches/`：6.18 内核补丁系列（`git am` 可直接应用，基线为 `v6.18`）
- `defconfig/`：6.18 构建使用的本地内核配置
- `drivers/`：补丁系列新增/修改的驱动源码本地镜像
- `dts/`：补丁系列使用的板级 DTS/DTSI 本地镜像
- `docs/`：移植笔记和构建指南
- `firmware/`：固件说明（6.18 不需要额外的板级固件 blob）
- `tools/`：板端辅助工具（MMIO 诊断）
- `scripts/ci/`：非交互式构建脚本

### 功能状态

| 功能                         | 5.10 厂商内核 | 6.18 主线                                                                                                                     |
| -------------------------- | --------- | --------------------------------------------------------------------------------------------------------------------------- |
| 8c12t SMP 启动（8 核 12 线程）    | 是         | 是                                                                                                                           |
| 串口控制台（`uart_v500`）         | 是         | 是                                                                                                                           |
| UFS（HUF S）                       | 是         | 是，HS-G4 / 2 lanes（读约 931 MB/s）                                                                                              |
| PCIe0（板载 ASM1064 SATA）     | 是         | 是，Gen3 x1 + `ahci`                                                                                                          |
| PCIe1（外部 x1 插槽）            | 是         | 是，Gen3 x1；需 FF-A `SET_MEM_UNSEC`，BAR0 在总线侧 `0x90000000`、经低 4 GiB 的 56 MiB 窗口映射                                              |
| Radeon 显卡（Oland）           | —         | **是**：5 个 ring + 5 个 IB 测试全部通过，fbdev 输出正常、屏幕文字可读                                                                            |
| amdgpu                     | 未编入       | **X11 / Weston / GNOME Wayland 可用（有已知问题）**：GMC/GART、UVD、fb0、DVI-I-1 正常；radeonsi 下 GL 4.6 / GLES 3.2。GNOME Wayland 4K@30 vsync 约 29.7 FPS、1080p@60 约 52–59 FPS，关 vsync 3139 FPS；4K↔1080p 切换已由补丁 0015 修复，X11/Wayland 均已复测 |
| 显示（厂商 DPU/HDMI）            | 是         | 尚未（只有厂商 `dkmd`）                                                                                                             |

### 补丁来源

全部补丁都是本板子的本地工作，参考来源：

- 板子运行时的厂商设备树（`dtc -I fs -O dts /proc/device-tree`）；
- 华为手机内核源码包（`HBN-AL00,HBN-AL10,HBN-AL80`），重点是
  `platform_source/basicplatform/drivers/{pci,clk,gpio}` 和
  `drivers/video/fbdev/dkmd`；
- [`mariobalanica/arm-pcie-gpu-patches`](https://github.com/mariobalanica/arm-pcie-gpu-patches)：
  arm64 外接 GPU 的内核补丁集。本板 PCIe1 的三个根因（MMIO 必须 Device
  映射、需要对齐修复、GTT 系统内存需强制 WC）与该项目的
  0001/0002/0003 一一对应，详见
  [pcie1_ffa_bar_zh.md](pcie1_ffa_bar_zh.md) 第 5 节。

**不要**参考 `hi3660`/`hi3670`：Kirin 9000C 的 UFS/PCIe/时钟模块与它们不同。

- `0001`：加入 QHW-100（charlotte）板级 DTS 和 SoC 骨架
- `0002`：加入 `uart_v500` 串口控制台驱动（可交互）
- `0003`：加入只读的 DSS 电源探测驱动（显示侦察用）
- `0004`：为主线 UFS core 增加 HUF S 每核队列支持
- `0005`：加入 `ufshcd-charlotte` 变体驱动
- `0006`：通过私有 UniPro 窗口协商 HS-G4
- `0007`：加入 DesignWare PCIe host glue（`pcie-charlotte`）
- `0008`：给 PCIe1 加 64 位 BAR 窗口与入向 `dma-ranges`
- `0009`：通过 platdrv FF-A 安全分区放行 PCIe1
- `0010`：amdgpu Bus Master + SI VRAM 窗口重编
- `0011`：报告 56 MiB 重定位 aperture，并强制 GTT 使用 WC
- `0012`：用户态 VRAM BO 在 mmap/fault 时迁移到 GTT
- `0013`：补上 SI 的 PASID TLB flush 回调
- `0014`：reset magic 和 UVD 消息缓冲区改用 I/O 访问器
- `0015`：modeset 后恢复 DCE vblank/pageflip 中断掩码，修复 4K↔1080p 切换
- 实测与证据见 [amdgpu_bringup_zh.md](amdgpu_bringup_zh.md)

### 工具

- `tools/charlotte-diag.c`：aarch64 静态 MMIO dump 工具（`dump`、`blocks`、
  `ranges`、`find`、`fill`、`fbbars`、`fbinfo`）

## 快速开始

- 构建指南（板端 Ubuntu/Debian）：[English](build_guide_ubuntu_en.md) | [中文](build_guide_ubuntu_zh.md)
- UFS 移植笔记：[中文](ufs_bringup_zh.md)
- PCIe / amdgpu 笔记：[中文](pci_amdgpu_zh.md)
- PCIe1 / FF-A / 64 位窗口：[中文](pcie1_ffa_bar_zh.md)
- amdgpu 迁移记录：[中文](amdgpu_bringup_zh.md)
- GDM greeter scanout BO pin 失败分析（已修复：PAM 环境注入）：[中文](scanout_bo_pin_zh.md)
- 当前可用桌面：GNOME Wayland（amdgpu + radeonsi，DVI-I-1
  @3840x2160@30）；X11 GNOME 和 Weston 也可用。4K↔1080p 切换已由
  补丁 0015 修复并在 Wayland/X11 下复测。用户态 VRAM→GTT 策略让
  SError 消失；第 2 轮出现的约 555 秒 Xorg 停顿在第 3 轮未复现。
  诊断服务已修正（不再读取 1 GiB 的 `amdgpu_vram` debugfs）并暂时
  禁用。详见 [amdgpu_bringup_zh.md](amdgpu_bringup_zh.md) 第 8 节
- USB 网卡 / 网络笔记：[中文](net_bringup_zh.md)
- 显示移植计划：[中文](display_bringup_zh.md)
- 完整移植报告：[中文](bringup_report_zh.md)

## 构建验证

当前补丁系列和 `defconfig/charlotte_6.18_defconfig` 可以在干净的上游
`v6.18` 上构建通过；配置一致性和 `git am` 冲突的踩坑记录见
[构建验证记录](build_verification_zh.md)。

## 安全注意事项

- 不要读未上电的显示/PCIe MMIO 块；6.18 冷启动时可能触发 NoC SError 并复位。
- 板级部署、磁盘和 rootfs 操作不在本文范围内。

## 参考

- 华为手机内核源码包（`HBN-AL00,HBN-AL10,HBN-AL80`）：厂商 UFS、kport PCIe、
  extreme CRG 和 dkmd 显示实现
- 板子运行时的厂商设备树（QHW-100）
