# HUAWEI Kirin 9000C（charlotte）Linux 6.18 bring-up 报告

- 日期：2026-11-23
- 目标机：QHW-100 / SP1PANGUXM（DMI: Dalian Guoxun QHW-100），BIOS 1.00.07 04/16/2025
- 自建内核：Linux 6.18.0（`charlotte_defconfig`）
- 对照内核：厂商 `5.10.97-38-9000c`（Android BSP 派生）

> **更新（2026-11-24）**：本报告写于 2026-11-23，当时 UFS/PCIe/amdgpu 均未开始。
> 后续进展：UFS 已完成（HS-G4）、PCIe0（板载 SATA）已打通、amdgpu 已进入
> X11 GNOME 和 Weston Wayland；GNOME Wayland 待 logind ACL 配置修复。
> 详见 `ufs_bringup_zh.md`、`amdgpu_bringup_zh.md`、`build_verification_zh.md`。
> 仍未完成：板载厂商 DPU/HDMI 路径；GNOME Wayland 待 logind ACL 修复。

---

## 一、执行摘要

| # | 问题 | 状态 | 一句话结论 |
|---|---|---|---|
| 1 | 6.18 只起来 4 个 CPU | **已解决** | 启动核声明 PAC、其余核不声明，`ARM64_HAS_ADDRESS_AUTH` 冲突。加 `arm64.nopauth` 后 8c12t（12 个逻辑 CPU）全可用。 |
| 2 | 板载 DPU/HDMI 无显示 | **已定位，未移植** | 固件交接时关掉了显示子系统；6.18 没有对应的电源/时钟/DPU 驱动。PCIe1 独立显卡的 amdgpu 显示已可用。 |
| 3 | 没有可用的调试通道 | **已解决** | 新增 V500 UART 驱动，`ttyHW0` 双向控制台可用（并修好了 RX 32 字节门限与 TX 丢字）。 |
| 4 | 根文件系统 / 存储 | **已解决** | UFS（HUF S）已移植，HS-G4 / 2 lanes，读约 931 MB/s。 |
| 5 | PCIe | **已解决** | PCIe0（板载 SATA）和 PCIe1（x1 插槽）均训练到 Gen3 x1；PCIe1 经 FF-A 放行和 56 MiB 窗口访问。 |
| 6 | amdgpu | **可用** | X11 GNOME 和 Weston Wayland 均可用；GNOME Wayland 待 logind ACL 修复。 |

**当前机器状态**：厂商 5.10 仍是默认；6.18 是独立测试镜像。

---

## 二、平台与启动链

```
fastboot -> UEFI payload（伪装成 Android boot.img）-> Linux Image + 自建 DTB
```

- SoC：Kirin 9000C。DT 里的 CPU `compatible`（a55/a76/a53）是**误导性的**：
  除 A510 级小核外，其余是华为自研核，ID 寄存器与 ARM 标准值不一致
  （内核会打印 `SANITY CHECK: Unexpected variation in SYS_ID_AA64...`，属预期）。
- 内存：`0x400000` 起约 3GB + `0x100000000` 起高内存；
  显示相关的 EFI Reserved 窗口 `0x50000000-0x547fffff`（72MB），GOP 显存在其中。
  两次实测 GOP 基址分别是 `0x54017000` 和 `0x5382e000`，即**基址不固定**。
- 显示硬件：华为 `dkmd` DPU（vendor DT 节点 `framebuffer` / `disp_manager` / `dpu_resource`），
  5.10 下由 `hisi-dpudrmfb` 驱动；6.18 无任何对应驱动。

---

## 三、问题 1：只有 4 个 CPU（已解决）

### 现象

```
CPU4: Detected conflict for capability 5 ((null)), System: 1, CPU: 0
...
smp: Brought up 1 node, 4 CPUs
```

`capability 5` = `ARM64_HAS_ADDRESS_AUTH`（`arch/arm64/include/generated/asm/cpucap-defs.h`）。
启动 CPU（0-3）报告支持 PAC/QARMA5，CPU4-11 报告不支持 → 系统能力取“支持”，本地校验冲突 → 后续 CPU 被杀。

### 根因

- 6.18 配置 `CONFIG_ARM64_PTR_AUTH=y`、`CONFIG_ARM64_BTI=y`；工作的 5.10 BSP 两者都是 `n`。
- 自研核 ID 寄存器不标准，导致 cap 判定不一致。

### 修复与验证

命令行加 `arm64.nopauth`（`arch/arm64/kernel/pi/idreg-override.c` 中的别名，清 `ID_AA64ISAR1/2` 的 PAC 位）。

实测：

```
CPU features: SYS_ID_AA64ISAR1_EL1[27:24]: forced to 0
CPU4: Booted secondary processor 0x0000000400 [0x482fd424]
cpus: 0-11
nr_cpu_ids=12
```

---

## 四、问题 2：板载 DPU/HDMI 无显示（已定位，未移植）

### 现象

- UEFI/bootloader 菜单在 HDMI 上正常可见（1920×1080）。
- 切换到 6.18 的**瞬间**画面消失，显示器随后进入**待机/“无信号”**。
- 5.10 启动正常（4K，`hisi-dpudrmfb`）。

### 证据链

1. efifb 正常注册并映射显存：

   ```
   efifb: framebuffer at 0x54017000, using 8100k, total 8100k
   efifb: mode is 1920x1080x32, linelength=7680
   Console: switching to colour frame buffer device 240x67
   ```

2. 内核确实把 fbcon 文字画进了这块显存（写显存这一侧完全正常）：

   ```
   fb0 nonzero_pixels=50715/2073600
   fb0 colors seen: ... 00aaaaaa
   ```

3. 显示器报“无信号” → DPU/HDMI 发送端没有输出。

4. 6.18 下 DPU 完全不可访问，读即 panic：

   ```
   devmem 0xe8800000 32
   -> SError Interrupt (async) -> Kernel panic
   ```

5. 细粒度探测（干净日志）：

   ```
   SMC PSCI_VERSION           fid=0x84000000 -> 0x10001        # SMC 通道正常
   SMC 0xc3000700 (id=0xff)   -> 0xc3000700 0xff 0x0 0x0       # 厂商 ip-regulator 服务不存在
   aod vote before = 0x0
   aod vote after  = 0x15                                      # AP media1/vivobus/dss 投票位
   CRG e8d01000: 00000000 00000000 00000000 00020000           # 外设 CRG 可读
   读 0xe8800000 -> SError -> panic                            # DPU 仍无响应
   ```

6. `disp_manager` 子块 `0xff7cc000` 第一页可读（18 个非零字），`0xff7cc000+0x1000` 读即 SError。

### 结论

固件在把控制权交给内核（ExitBootServices/交接）时**关掉了显示子系统的电源/时钟/复位**；
5.10 靠厂商完整显示栈重新拉起，6.18 缺这套栈，所以：

- **efifb / simple-framebuffer / fbcon 这条路原理上不可行**（显存写得再对也没人扫描）；
- 显示器“无信号”而不是“黑图”，说明输出端被关掉；
- 想有显示必须补齐电源 + 时钟 + 复位 + DPU + HDMI PHY。

### 显示子系统上电路径（厂商 5.10 的实现，已查清）

| 环节 | 厂商实现 | 6.18 现状 |
|---|---|---|
| 子系统上电 | `ip_regulator.c`：AOD 投票 + `arm_smccc_smc(0xC3000700, regulator_id, 1)` | SMC 服务在本固件不存在（原样返回参数）；AOD 投票可写但不足以拉起 DPU |
| 层级 | media1_subsys(0) → vivobus(2) → dsssubsys(4) / disp_ch1subsys(5) | — |
| AOD 投票寄存器 | `sctrl_address.base_addr(0xFB21B000) + 0x438`：bit0 AP media1、bit2 AP vivobus、bit4 AP dss | 实测可写：`0x0 -> 0x15` |
| 与协处理器交互 | `REGULATOR_MBX_ID = IPC_ACPU_LPM3_MBX_2` 邮箱（LPM3） | 未移植（很可能是真正上电动作所在） |
| 时钟门控 | `clk/extreme/platform/clk-char-cs.c` 的 `crgctrl_gate_clks[]` / `media1crg_gate_clks[]` | 未移植 |
| 相关门控名 | `aclk_dss`、`pclk_dss`、`aclk_qic_dss`、`pclk_qic_dss_cfg`、`aclk_media_common`、`pclk_media_common`、`aclk_disp_qic_subsys`、`pclk_disp_qic_subsys` | — |
| CRG 基址 | 主 CRG `0xFFB85000`（`clk_crgctrl@FFB85000`）；外设 CRG `0xE8D01000`（DT 节点名叫 `regulator_ip@FFB85000`，但 `reg` 是 `0xE8D01000`，注意别被名字骗） | 外设 CRG 在 6.18 可读 |
| 复位状态 | `SOC_CRGPERIPH_PERRSTSTAT3_ADDR(0xE8D01000 + off)` | 未解析 |
| DPU 初始化 | `drivers/video/fbdev/dkmd/dpu/begonia/`（composer/online/dfr/ppc 等） | 未移植 |
| HDMI PHY/PLL | 厂商 `mipi2hdmi` / `dp_ctrl` 相关（含 PMIC ldo22 等） | 未移植 |
| SMMU | DPU 节点带 `iommus`，DMA 走 IOVA | 未处理 |

### 5.10 参考值（可作为后续 diff 基线）

主 CRG 前 0x20 字节：

```
0xffb85000: 00000000 00000000 00fd796c 00fd796c
0xffb85010: 00000000 00000000 ebffffff cbefffff
```

5.10 关键地址：

| 节点 | 区域 |
|---|---|
| `disp_manager`（`dkmd,composer_manager`） | `0xE8800000+0x100000`、`0xFF7CC000+0x2000`、`0xFF7C9000+0x1000` |
| `dpu_resource`（`dkmd,dpu_resource`） | `0xE8900000` 等 |
| EFI GOP 显存 | 实测 `0x54017000` / `0x5382e000`（不固定） |

---

## 五、问题 3：UART 控制台（已完成）

### 驱动

新增 `drivers/tty/serial/uart-v500.c`（PIO，约 560 行）：

- 不依赖 dmaengine / pinctrl / reset / 时钟框架；`uartclk` 默认 112MHz（厂商 DT `clock-rate = <0x6acfc00>`），可由 DT `clock-frequency` 覆盖。
- `startup()` 只清状态、编程 FIFO 门限、开 RX/TX/RX超时中断；**不碰固件已配好的 115200 8N1**。
- `set_termios()` 对 115200 8N1 直接返回，只有请求其它参数时才重编程。
- 注册 `ttyHW0`（major 253）并作为 console；使用 6.18 新 TX 框架 `uart_port_tx_limited()`（`tty_port.xmit_fifo`）。
- 修了两个真实缺陷：
  1. 固件把 `FIFO_CFG` 的 RX 触发门限设为 **1/2 = 32 字节**且屏蔽 RX 超时中断 → 敲键要满 32 字符才响应。
     改为 `TX 1/2 | RX 1/8 | RTS 7/8` 并打开 RX 超时中断后，`ls\r` 三个字符立即执行。
  2. `v500_putchar()` 的等待超时太短，输出突发时丢字符。改为阻塞等待 `TXFF` 清零。

### 配置

```
# drivers/tty/serial/Kconfig
config SERIAL_UART_V500            tristate, depends on ARCH_HISI || COMPILE_TEST, select SERIAL_CORE
config SERIAL_UART_V500_CONSOLE    bool, depends on SERIAL_UART_V500=y, select SERIAL_CORE_CONSOLE

# drivers/tty/serial/Makefile
obj-$(CONFIG_SERIAL_UART_V500) += uart-v500.o

# arch/arm64/configs/charlotte_defconfig
CONFIG_SERIAL_UART_V500=y
CONFIG_SERIAL_UART_V500_CONSOLE=y
```

### 实测

```
ff894000.serial: ttyHW0 at MMIO 0xff894000 (irq = 14, base_baud = 7000000) is a UART-V500
printk: legacy console [ttyHW0] enabled
printk: legacy bootconsole [uart_v500] disabled      # 干净交接，不重复打印
uart-v500 ff894000.serial: ttyHW0 irq 14 uartclk 112000000

/proc/interrupts:
 14: ... GICv3 427 Level uart-v500     # SPI395
~ # uname -a
Linux (none) 6.18.0 ... aarch64 GNU/Linux
```

---

## 六、已知限制

- 未上电的显示/PCIe MMIO 块不能读，否则触发 NoC SError；当前 6.18 Image
  不含相关探测代码。
- PMIC、板载 DPU/HDMI 显示和厂商网络路径仍未移植；PCIe1 独立显卡的
  amdgpu 路径已可用。
