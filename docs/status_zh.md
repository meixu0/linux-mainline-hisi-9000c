# HUAWEI Kirin 9000C（charlotte / QHW-100）Linux 6.18 bring-up 状态与诊断记录

- 日期：2026-11-23
- 目标机：QHW-100 / SP1PANGUXM（DMI: Dalian Guoxun QHW-100），BIOS 1.00.07 04/16/2025（串口 + 局域网 SSH 访问）
- 运行内核：`6.18.0 #8 SMP`（自建，`charlotte_defconfig`）
- 对照内核：厂商 `5.10.97-38-9000c`（Android BSP 派生）
- 本机串口：`/dev/ttyUSB0`（CH340，115200 8N1）

> **历史记录（2026-11-23）**：本文是 UFS/PCIe 尚未开始时的过程记录，部分结论已被后续工作取代。
> 最新状态请看 `../README.md`、`bringup_report_zh.md`、`pci_amdgpu_zh.md` 和 `build_verification_zh.md`。

---

## 1. 结论摘要

| 问题 | 状态 | 结论 |
|---|---|---|
| 6.18 只能启动 4 个 CPU | **已修复** | 启动 CPU 声明 PAC/QARMA5，其余核不声明，`ARM64_HAS_ADDRESS_AUTH`（capability 5）冲突导致 CPU4-11 被杀。加 `arm64.nopauth` 后 8c12t（12 个逻辑 CPU）全可用。 |
| 6.18 无显示（黑屏） | **已定位，未修复** | 固件在交接（ExitBootServices）时关掉了整个 DPU + HDMI 输出；6.18 没有该显示控制器驱动，efifb 只是往显存写，没人扫描输出。显示器直接报“无信号”。 |
| 没有可用的早期调试通道 | **已解决** | 新增 V500 UART 驱动，`ttyHW0` 控制台可双向交互（`console=ttyHW0,115200n8`）。 |
| 根文件系统 / 存储 | **已完成（2026-11-24）** | UFS（HUF S）已移植，`sda`..`sdd` 可用，HS-G4 / 2 lanes；详见 `ufs_bringup_zh.md`。 |

---

## 2. 平台与启动链

```
fastboot -> UEFI payload（伪装成 Android boot.img）-> GRUB -> Linux Image + 自建 DTB
```

- SoC：Kirin 9000C。DT 里的核 `compatible`（a55/a76/a53）是**误导性的**：除现有 2 类 A510 级小核外，其余是华为自研核，ID 寄存器与 ARM 标准值不一致（见日志里的 `SANITY CHECK: Unexpected variation`）。
- 内存：`0x400000` 起约 3GB + `0x100000000` 起高内存；`0x50000000-0x547fffff`（72MB）是 EFI Reserved，UEFI GOP 显存在其中。
- 显示硬件：华为 `dkmd` DPU（vendor 节点 `framebuffer` / `disp_manager` / `dpu_resource`），5.10 下由 `hisi-dpudrmfb` 驱动；6.18 无任何对应驱动。

---

## 3. 问题一：只有 4 个 CPU 可用（已修复）

### 现象（修复前）

```
CPU4: Detected conflict for capability 5 ((null)), System: 1, CPU: 0
...
smp: Brought up 1 node, 4 CPUs
```

`capability 5` 即 `ARM64_HAS_ADDRESS_AUTH`（见 `arch/arm64/include/generated/asm/cpucap-defs.h`）。
启动 CPU（0-3）报告支持 PAC，CPU4-11 报告不支持 → 系统级能力取“支持”，本地校验冲突 → 后续 CPU 被 `cpu_die()` 掉。

### 根因

- 6.18 配置里 `CONFIG_ARM64_PTR_AUTH=y`、`CONFIG_ARM64_BTI=y`；工作的 5.10 BSP 两者都是 `n`。
- 自研核的 ID 寄存器不是 ARM 标准值，导致 cap 判定不一致。

### 修复

内核命令行加：

```
arm64.nopauth
```

（`arch/arm64/kernel/pi/idreg-override.c` 中的别名，会清掉 `ID_AA64ISAR1_EL1` 的 gpi/gpa/api/apa 与 `ID_AA64ISAR2_EL1` 的 gpa3/apa3。）

### 验证（6.18 串口实测）

```
CPU features: SYS_ID_AA64ISAR1_EL1[31:28]: already set to 0
CPU features: SYS_ID_AA64ISAR1_EL1[27:24]: forced to 0
...
CPU4: Booted secondary processor 0x0000000400 [0x482fd424]
...
cpus: 0-11
nr_cpu_ids=12
```

现在 12 个 CPU 全部 online，只剩一行无害的 `CPU features: Unsupported CPU feature variation detected.`。

> 另一条等效修法是重新编译时设 `CONFIG_ARM64_PTR_AUTH=n`；加命令行参数更灵活，已采用。

---

## 4. 问题二：6.18 黑屏（已定位，未修复）

### 现象

- GRUB 菜单（1920×1080）在 HDMI 上**正常可见**。
- 选中 6.18 的瞬间画面消失，显示器随后进入**待机/“无信号”**状态（不是“有信号但全黑”）。
- 5.10 启动时显示正常（4K，`hisi-dpudrmfb`）。

### 证据链（6.18 串口日志）

1. 内核确实注册并使用了 efifb：

   ```
   efifb: framebuffer at 0x54017000, using 8100k, total 8100k
   efifb: mode is 1920x1080x32, linelength=7680
   Console: switching to colour frame buffer device 240x67
   fb0: EFI VGA frame buffer device
   ```

2. 内核确实把 fbcon 文字画进了这块显存（说明“写显存”这一侧完全正常）：

   ```
   fb0 mode=U:1920x1080p-77 nonzero_pixels=50715/2073600
   fb0 colors seen: ... 00aaaaaa     (灰白文字色)
   ```

3. 但显示器报“无信号”，说明 DPU/HDMI 发送端此时没在输出。

4. 显示子系统在 6.18 下处于“半掉电”状态：只读探测 `0xff7cc000` 第一页（4K）能读回 18 个非零字，
   读 `0xff7cc000+0x1000` 立刻触发异步 SError：

   ```
   SError Interrupt on CPU7, code 0x00000000be000000 ... (async)
   Kernel panic - not syncing: Asynchronous SError Interrupt
   ```

5. 6.18 内核里没有任何 DPU/HDMI 驱动，也没有时钟/电源域驱动去重新初始化它。

### 结论

**UEFI 在把控制权交给内核（ExitBootServices）时关闭了 DPU + HDMI 输出**；5.10 之所以能显示，是因为它的厂商 DPU 驱动随后重新初始化了显示管线。6.18 缺少该驱动，所以：

- efifb / simple-framebuffer / fbcon 这条路**原理上不可行**（显存写得再对，也没有扫描输出）；
- 显示器“无信号”而不是“黑图”，也印证了输出端被关掉；
- 想有显示，必须在 6.18 里重新驱动 DPU（并重新点亮 HDMI PHY/PLL）。

### 已知寄存器/地址（来自 5.10 活 DT）

| 节点 | 区域 |
|---|---|
| `framebuffer` (`dkmd,dpu_device`) | 无 reg（显存由 reserved-memory 提供） |
| `disp_manager` (`dkmd,composer_manager`) | `0xe8800000+0x100000`, `0xff7cc000+0x2000`, `0xff7c9000+0x1000` |
| `dpu_resource` (`dkmd,dpu_resource`) | `0xe8900000` 等（含 iommus，DPU 走 SMMU/IOVA） |
| EFI GOP 显存 | `0x54017000`，1920×1080×32，stride 7680 |
| Reserved 窗口 | `0x50000000-0x547fffff` |

> 注意：DPU 在 SMMU 后面，寄存器里的“扫描地址”很可能是 IOVA 而不是物理地址；直接拿 `0x54017000` 去匹配寄存器不可靠。

---

## 5. 新增：V500 UART 串口控制台（已完成）

### 驱动

新增 `drivers/tty/serial/uart-v500.c`（约 540 行，PIO 模式）：

- 不依赖 dmaengine / pinctrl / reset / 时钟框架；`uartclk` 默认 112 MHz（厂商 DT `clock-rate = <0x6acfc00>`），可由 DT `clock-frequency` 覆盖。
- `startup()` 只清状态 + 开 RX/TX 中断；`set_termios()` 对 115200 8N1 直接返回，**不碰固件已经配好的波特率/帧格式**（只请求其它参数时才重编程）。
- 注册 `ttyHW0`（major 253），并作为 console（`console=ttyHW0,115200n8`）。
- 用 6.18 新的 TX 框架 `uart_port_tx_limited()`（`tty_port.xmit_fifo`，旧 `state->xmit` 已移除）。

### 配置改动

```
# drivers/tty/serial/Kconfig
config SERIAL_UART_V500
	tristate "HiSilicon UART V500 serial port support"
	depends on ARCH_HISI || COMPILE_TEST
	select SERIAL_CORE

config SERIAL_UART_V500_CONSOLE
	bool "Console on HiSilicon UART V500 serial port"
	depends on SERIAL_UART_V500=y
	select SERIAL_CORE_CONSOLE

# drivers/tty/serial/Makefile
obj-$(CONFIG_SERIAL_UART_V500) += uart-v500.o

# arch/arm64/configs/charlotte_defconfig
CONFIG_SERIAL_UART_V500=y
CONFIG_SERIAL_UART_V500_CONSOLE=y
```

### 实测

```
ff894000.serial: ttyHW0 at MMIO 0x0 (irq = 14, base_baud = 7000000) is a UART-V500
printk: legacy console [ttyHW0] enabled
printk: legacy bootconsole [uart_v500] disabled
uart-v500 ff894000.serial: ttyHW0 irq 14 uartclk 112000000

/proc/interrupts:
 14: 334 ... GICv3 427 Level uart-v500     <- SPI395，RX 中断在计数
 15: 351 ... GICv3 172 Level xhci-hcd:usb1

~ # uname -a
Linux (none) 6.18.0 #8 SMP ... aarch64 GNU/Linux
~ # echo HELLO_..._LONG_LINE_TEST
HELLO_..._LONG_LINE_TEST
```

- 早期日志：earlycon 先输出；`ttyHW0` 注册后 earlycon 被干净地注销，**不会重复打印**。
- 小瑕疵：`MMIO 0x0`（没填 `port->mapbase`，纯显示问题）；`sh: can't access tty; job control turned off`（未用 `cttyhack`）。

---

## 6. 如何启动 / 复现

### GRUB 测试项（`/etc/grub.d/40_custom`，标题 `TEST - uart console`）

```
menuentry "Linux 6.18 charlotte bring-up (TEST - uart console)" {
    insmod part_gpt
    insmod ext2
    search --no-floppy --file --set=root /boot/linux-6.18-charlotte/Image
    linux /boot/linux-6.18-charlotte/Image earlycon=uart_v500,0xff894000 \
          console=ttyHW0,115200n8 loglevel=7 arm64.nopauth panic=20
    devicetree /boot/linux-6.18-charlotte/charlotte-9000c.dtb
    initrd /boot/linux-6.18-charlotte/initramfs-console.cpio.gz
}
```

参数说明：

- `earlycon=uart_v500,0xff894000`：最早的轮询控制台。
- `console=ttyHW0,115200n8`：唯一真正的 console（故意不加 `console=tty0`，避免日志跑到黑屏上）。
- `arm64.nopauth`：8c12t（12 逻辑 CPU）修复。
- `panic=20`：panic 后 20 秒自动重启回 5.10（不再需要手动断电）。

### 构建

```sh
cd ~/9000c/kernel-port/linux-6.18
make ARCH=arm64 charlotte_defconfig
make ARCH=arm64 -j"$(nproc)" Image hisilicon/charlotte-9000c.dtb
```

配置相对上一版只多了两个符号（`CONFIG_SERIAL_UART_V500*`），其它无变化。

### 安装 / 产物

```
~/9000c/kernel-port/out/Image-6.18-uart
~/9000c/kernel-port/out/charlotte-9000c.dtb
~/9000c/kernel-port/out/initramfs-console.cpio.gz
~/9000c/kernel-port/out/config-6.18-uart
/boot/linux-6.18-charlotte/Image              (新)
/boot/linux-6.18-charlotte/Image.prev         (上一版，回滚用)
/boot/linux-6.18-charlotte/initramfs-console.cpio.gz
```

GRUB 默认项仍是 5.10，不受影响；测试项手动选最后一项。

### 回滚

- 不选测试项即可。
- 或 `cp /boot/linux-6.18-charlotte/Image.prev /boot/linux-6.18-charlotte/Image`。
- `/etc/grub.d` 的备份已移到 `/root/grub-backups/`（**不要**在 `/etc/grub.d/` 里留 `*.bak`，`update-grub` 会把它们当脚本执行，产生重复菜单项，之前踩过）。

---

## 7. 串口调试注意事项（picocom 不能交互的原因）

- `/dev/ttyUSB0` 上的数据是“单读者”语义：**同时只能有一个进程拿到字节**。
  之前后台跑着

  ```sh
  sg dialout -c 'stty -F /dev/ttyUSB0 115200 raw ...; cat /dev/ttyUSB0' > log
  ```

  它会抢走所有输入，picocom 就会“Terminal ready”但什么都不显示。使用前先 `pgrep -af ttyUSB0` 并停掉抓取进程。
- 另外，打开 picocom 时**先按一次回车**：shell 的提示符是在 picocom 连接之前打印的，不会自动重发。
- 本机（`aosc-2575cbe`）用户已在 `dialout` 组，但**当前已登录会话的补充组可能是旧的**；遇到 `Permission denied` 时用：

  ```sh
  sg dialout -c 'picocom -b 115200 /dev/ttyUSB0'
  ```

- 不占用终端时，也可以直接“写串口”发命令：

  ```sh
  sg dialout -c 'printf "uname -a\r" > /dev/ttyUSB0'
  ```

---

## 8. 待办与建议路线

### A. 显示（下一步主攻）

现实路线按代价从低到高：

1. **最小 DPU framebuffer 驱动**（固定 1920×1080 或 4K）：用厂商源码里的寄存器序列，只做“点亮一条扫描通路 + 线性 framebuffer”，不做热插拔/多分辨率。
   - 需要先搞清楚：DPU 电源/时钟域怎么开（6.18 没有 PMU/CRG 驱动）、HDMI PHY/PLL 初始化序列、SMMU 是旁路还是需要建 IOVA 映射。
   - 现在有串口 + `panic=20`，可以低成本地做寄存器探测（读死活块会自动重启）。
2. **移植厂商 `dkmd` DRM/fbdev 显示栈**：功能全，但代码量大、私有依赖多（ion、hkip、dss、pmctrl…），周期长。
3. 保持 6.18 无头 + 5.10 有头，只用于服务器/计算场景。

### B. UFS / 根文件系统

UFS 已完成，见 `ufs_bringup_zh.md`；当前待办只剩 PCIe1 插槽电源和显示（见 `pci_amdgpu_zh.md`）。

### C. 小修（可顺手做）

- `port->mapbase = pdev->resource[0].start`（日志里 `MMIO 0x0` 变成真实地址）。
- initramfs：`setsid cttyhack sh -i`（job control / 作业控制）。
- 串口 getty（`/etc/inittab` 或 `busybox getty`）方便长期使用。
- `/dev` 下补常用节点/符号链接；把 `/init` 做成可交互 + 可切根。

---

## 9. 关键文件

（远端 `~/9000c`）

- `kernel-port/linux-6.18/`：6.18 源码树
- `kernel-port/linux-6.18/drivers/tty/serial/uart-v500.c`：新增 UART 驱动
- `kernel-port/linux-6.18/drivers/tty/serial/uart-v500-earlycon.c`：原有 earlycon
- `kernel-port/linux-6.18/arch/arm64/boot/dts/hisilicon/charlotte-9000c.dts` + `charlotte.dtsi`
- `kernel-port/out/`：Image / DTB / initramfs / config
- `kernel-port/diag/`：诊断用 C 工具（`charlotte-diag.c`）与 initramfs 构建目录
- 厂商源码：`kernel/HBN-AL00,HBN-AL10,HBN-AL80-Code_Opensource/Code_Opensource/`
  - UART：`kernel/platform_source/basicplatform/drivers/tty/serial/uart_v500*.c`
  - 显示：`kernel/drivers/video/fbdev/dkmd/dpu/begonia/`、`kernel/platform_source/display/drivers/dpu_drm`

（本机 `~/9000c`）

- `status_zh.md`（本文）
- `logs/6.18-uart-console-boot.log`：串口控制台成功启动 + 交互日志
- `logs/6.18-diag-boot.log`：显示诊断启动日志（含 SError panic）
- `earlycon.log` / `earlycon1.log`：更早的两次 6.18 启动日志

---

## 附录：关键日志摘录

### 修复前（4 核）

```
CPU4: Detected conflict for capability 5 ((null)), System: 1, CPU: 0
```

### 修复后（8c12t / 12 逻辑 CPU）

```
CPU features: SYS_ID_AA64ISAR1_EL1[27:24]: forced to 0
CPU4: Booted secondary processor 0x0000000400 [0x482fd424]
cpus: 0-11
```

### 显示诊断

```
efifb: framebuffer at 0x54017000, using 8100k, total 8100k
efifb: mode is 1920x1080x32, linelength=7680
Console: switching to colour frame buffer device 240x67

fb0 nonzero_pixels=50715/2073600   (文字确实画进去了)
显示器：无信号 / 待机

0xff7cc000 第 1 页: 可读 (18 个非零字)
0xff7cc000 + 0x1000: SError -> Kernel panic
```

### 串口控制台

```
uart-v500 ff894000.serial: ttyHW0 irq 14 uartclk 112000000
 14: 334 ... GICv3 427 Level uart-v500
~ # uname -a
Linux (none) 6.18.0 #8 SMP ... aarch64 GNU/Linux
```

---

# 附：显示子系统电源/时钟的进一步探测（2026-11-23 晚）

## 结论先行

6.18 里 DPU 完全不可访问（读 `0xe8800000` 立即异步 SError → panic）。原因**不是**缺一个寄存器初始化，
而是显示子系统在固件交接后被关掉了电源/时钟；要恢复显示，需要按厂商 BSP 的路径补齐**一整套电源+时钟栈**，
再加 DPU/HDMI 驱动。这是数周量级的工程，不是一次探测能解决的。

## 已确认的事实

### 1. SMC/ATF 通道正常，但厂商的 "ip regulator" 服务在本固件上不存在

```
dss-probe: SMC PSCI_VERSION           fid=0x84000000 -> 0x10001          # PSCI v1.1，SMC 可用
dss-probe: SMC regu bogus id          fid=0xc3000700 a1=0xff -> 0xc3000700 0xff 0x0 0x0
dss-probe: SMC media1_subsys          fid=0xc3000700 a1=0x0  -> 0xc3000700 0x0 0x1 0x0
dss-probe: SMC dsssubsys              fid=0xc3000700 a1=0x4  -> 0xc3000700 0x4 0x1 0x0
```

- `IP_REGULATOR_FID_VALUE = make_fid(FID_REGU_GROUP=7, 0) = 0xC3000700`（`bl31_smc.h` / `ip_regulator.c`）。
- 调用后 **x0..x3 原样返回**，说明 ATF 没有这个 OEM 服务（或未注册），调用是空操作。
- 因此厂商 5.10 的显示上电**并不依赖**这个 SMC 成功。

### 2. AOD 投票寄存器可写

```
dss-probe: aod vote before = 0x0
dss-probe: aod vote after  = 0x15        # bit0 AP media1, bit2 AP vivobus, bit4 AP dss
```

- 地址 `0xFB21B000 + 0x438`（`sctrl_address.base_addr` + `aod_sc_offset`）。
- 厂商 `aod_set_and_get_poweron_state()` 还会通过 `IPC_ACPU_LPM3_MBX_2` 邮箱与 LPM3 协处理器交互，
  真正的电源序列很可能在 LPM3 侧完成。仅写投票位不足以让 DPU 响应（实测仍然 SError）。

### 3. CRG 寄存器块

| 块 | 地址 | 6.18 可读? | 备注 |
|---|---|---|---|
| 主 CRG (`clk-extreme-crgctrl`) | `0xFFB85000` | 未测（应可读） | 5.10 参考值见下 |
| 外设 CRG (`regulator_ip` 节点 reg) | `0xE8D01000` | **可读** | 6.18: `00000000 00000000 00000000 00020000` |
| DPU (`disp_manager`) | `0xE8800000` | **不可读 → SError** | 6.18 掉电/无时钟 |
| `disp_manager` 子块 | `0xFF7CC000+0x1000` | 不可读 → SError | 第一页可读（18 个非零字） |

5.10 上主 CRG 前 0x20 字节：

```
0xffb85000: 00000000 00000000 00fd796c 00fd796c
0xffb85010: 00000000 00000000 ebffffff cbefffff
```

（`0x00fd796c`、`0xebffffff`、`0xcbefffff` 是大量门控使能位；6.18 里对应位置大概是 0。）

### 4. 厂商的时钟/复位代码位置

- 门控表：`platform_source/basicplatform/drivers/clk/extreme/platform/clk-char-cs.c`
  - `crgctrl_gate_clks[]`（主 CRG，`0xFFB85000`）
  - `media1crg_gate_clks[]`（media1 CRG）
  - 相关门控名：`aclk_dss`、`pclk_dss`、`aclk_qic_dss`、`pclk_qic_dss_cfg`、
    `aclk_media_common`、`pclk_media_common`、`aclk_disp_qic_subsys`、`pclk_disp_qic_subsys`
- 门控实现：`plat-clk-gate.c`：`pclk->enable = data->base + gate->offset; pclk->ebits = gate->bit_mask;`
- 复位状态：`ip_regulator.c` 里 `SOC_CRGPERIPH_PERRSTSTAT3_ADDR(pmic->regs)`（即 `0xE8D01000` 块内）
- 子系统上电：`ip_regulator.c` 的 `ip_to_atf_enabled()` → `ip_clock_work()` + `ip_power_on()`

## 引脚：要真正点屏还缺什么

1. **电源**：media1 → vivobus → dsssubsys 的层级上电（AOD 投票 + LPM3 邮箱或等价寄存器序列）。
2. **时钟**：至少 `aclk_dss`、`pclk_dss`、`aclk_qic_dss`、`pclk_qic_dss_cfg`、`aclk/pclk_media_common`、
   `aclk/pclk_disp_qic_subsys`（CRG 门控位）。
3. **复位**：CRG 里 DSS/DSI/HDMI 相关复位位需要解复位。
4. **DPU 初始化**：LDI/SDF/DFR 等合成器 + 扫描输出（厂商 `dkmd/dpu/begonia` 里有完整序列）。
5. **HDMI PHY/PLL**：厂商有 `mipi2hdmi`/`dp_ctrl` 相关节点与 ATF/PMIC 交互。
6. **SMMU**：DPU 的 `iommus` 属性表明其 DMA 走 SMMU/IOVA；要么旁路，要么建映射。

## 期间顺带修好的两件事

1. **串口 RX 交互不实时**：原因是固件把 UART `FIFO_CFG` 的 RX 触发门限设成 **1/2 = 32 字节**，
   而且 RX 超时中断被屏蔽——必须一次敲满 32 个字符才会被读到（这就是 picocom 里"敲键没反应"的真相）。
   修复：`startup()` 写 `FIFO_CFG = TX 1/2 | RX 1/8 | RTS 7/8`，并打开 RX 超时中断（`INT_MSK` bit2）。
   现在 `ls\r` 三个字符即可立即执行。
2. **控制台日志丢字符**：`v500_putchar()` 原来的 `timeout` 太短，打印突发时会丢字符。
   改成阻塞等待 `TXFF` 清零后，日志完整无缺。

## 串口日志抓取的坑（重要）

`/dev/ttyUSB0` 是**单读者**语义：同一条串口流如果同时有 `cat`、`picocom` 两个读者，
每个字节只会被其中一个取走，两边日志都会"随机缺字"，看起来像内核丢字符。
排查/取证前先 `pgrep -af ttyUSB` 确认只有一个读者。

## 当前产物状态（2026-11-24 更新）

- `/boot/linux-6.18-charlotte/Image`：PCIe0/SATA 可用版（`8247f40db86c917609d81daac7efa5ef`）
- `/boot/linux-6.18-charlotte/Image.repotest`：本次仓库构建验证镜像
  （`4690ec04b1a04b2183411104d17014ee`）
- `/boot/linux-6.18-charlotte/Image.ufsworking`：早期 UFS 可用版（回滚用）
- `docs/logs/6.18-dss-probe.log`：当时的 DPU 探测日志

## 建议的后续路线（2026-11-24 更新）

1. **PCIe1（x1 插槽）/ AMD 亮机卡**：先补 PMIC LDO16（`pcieSwitch`）电源，
   再确认卡的 BAR 能否放进 59 MB 窗口；详见 `pci_amdgpu_zh.md`。
2. 显示若要继续：先做 **5.10 vs 6.18 的 CRG 全量 diff**（5.10 有参考值，6.18 可读 `0xE8D01000`），
   确定哪些门控被关；再研究 LPM3 邮箱上电协议（或找到等效的 AOD 触发方式）。
3. 最后才是 DPU + HDMI PHY 驱动（可直接复用厂商寄存器序列，但工作量最大）。
