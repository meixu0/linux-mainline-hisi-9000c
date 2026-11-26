# 6.18 显示 bring-up：侦察结果与分阶段计划

> 状态（2026-11-24）：显示仍未点亮。作为替代路线，PCIe + AMD 亮机卡的方案已经打通
> （见 `pci_amdgpu_zh.md`）；本文保留 DPU/HDMI 的分析和后续计划。

## 1. 5.10 现状

- 图形栈是 **DRM**（`/dev/dri/card0` + `renderD128`），**没有 fbdev**（`/proc/fb` 为空）。
- 连接器：`card0-HDMI-A-1`、`card0-VGA-1`，当前都是 `disconnected`
  （没有接显示器，或者桥片没读到 HPD/EDID）。
- 显示驱动是厂商 `dkmd`（`drivers/video/fbdev/dkmd/`，尽管在 fbdev 目录下，
  实际注册的是 DRM 设备）。

## 2. 硬件拓扑（来自 DT）

| 节点 | 地址 | 说明 |
|---|---|---|
| `composer_manager` | `0xe8800000` (1 MiB) + `0xff7cc000` (8 KiB) + `0xe8d01000` (4 KiB) | DPU composer，中断 SPI 161 + 480，IOMMU sid 1 |
| `dpu_resource` | `0xe8900000` (1.5 MiB) + `0xffb85000`/`0xfb21b000`/`0xff7cc000`/`0xffb81000`/`0xe8d01000`/`0xefb02000`/`0xefa71000`/`0xff7cb000` | DPU 资源/时钟/复位窗口 |
| `dpu_cmdlist` | — | DPU 命令列表引擎（`cmdlist_7xx`，最大 48 MiB） |
| `dpu_opr_mgr` | `0xe8800000` | DPU operator 管理 |
| `mipi2hdmi` | I2C (`i2c@FF88D000`) | **MIPI-DSI→HDMI 桥片**；DT 直接给了 enable/disable/suspend/resume 寄存器序列、HPD GPIO、function GPIO |

**结论**：这块板子没有 SoC 原生 HDMI TX 节点，HDMI 走
`DPU → MIPI DSI → mipi2hdmi 桥片 → HDMI`。

电源/时钟（`ip_regulator.c` + `dpu_comp_power.c`）：

- AOD 投票寄存器 `sctrl 0x438`：bit0 AP media1、bit2 AP vivobus、bit4 AP dss
- 需要打开的 regulator：`dsssubsys`、`vivobus`、`media_subsys`、`smmu_tcu`、`disp_ch1subsys`
- 时钟：`aclk_dss`、`pclk_dss`、`aclk_qic_dss`、`pclk_qic_dss_cfg`、
  `aclk_media_common`、`pclk_media_common`、`aclk_disp_qic_subsys`、`pclk_disp_qic_subsys`
- 还有 `SCTRL_SCPERRSTEN1/2` 里的 DSS 复位位（`ip_rst_dss`）

已实测：**DPU 模块未上电时读 `0xe8800000` 会挂住**（`charlotte-diag dump`
直接卡死，只能 kill），这和早前 6.18 下读到 SError 是同一件事。


## 2.1 更正：时钟/CRG 以手机内核为准（不是 hi3660）

这块板是 **Kirin 9000C**，同树里的 `clk-hi3660.c` / `clk-hi3670.c` 是别的平台，
**不能参考**。正确的参考在手机内核：

- 时钟控制器 compatible：`hisilicon,clk-extreme-crgctrl`
- 寄存器窗口：`0xffb85000`（4 KiB）
- 驱动目录：`platform_source/basicplatform/drivers/clk/extreme/`
  - `platform/clk-char-cs.c`：时钟表（fixed / factor / gate / div / mux / scgt）
  - `plat-clk-gate.c`：`hi3xxx_clkgate_*`，`pclk->enable = base + gate->offset`,
    `ebits = gate->bit_mask`
  - `platform/clk-char-cs.c` 里 `sctrl_gate_clks[]` 对应 `0xfb21b000`

DT 里显示相关时钟（phandle 0x112 → crgctrl）：

| 时钟 | crgctrl id |
|---|---|
| `aclk_media_common` | 0x42 |
| `aclk_qic_dss` | 0x43 |
| `pclk_media_common` | 0x44 |
| `pclk_qic_dss_cfg` | 0x45 |
| `pclk_dss` | 0x49 |
| `aclk_dss` | 0x6e |
| `clk_dss_axi_mm` | 0x138 |

注意：表里 `aclk_dss` 出现在 `fixed_factor_clks[]`（`clk_spll / 3`），
说明它是固定分频而不是门控；真正的 gate 要去 `sctrl_gate_clks[]` /
其他表里按名字找。

另外 `connector_manager` 的子节点是 `mipi_dsi@2`、`dp_ctrl@0/1`
（连接器由 dkmd 的 connector manager 管理，实际 DRM 驱动名是
`hisi-dpu-drm`，`/dpu_drm`，compatible `hisi,dpu_drm`）。

`mipi2hdmi` 这个 I2C 节点在 DT 里没有 `reg`，5.10 里直接报
`of_i2c: invalid reg` 被忽略 —— 它不是这块板 HDMI 的实际通路。

## 2.2 下一步改为“寄存器快照对比”

源头上把 dkmd 的完整上电序列读通成本很高，改用和 UFS 一样的办法：

1. 5.10 显示正常时，dump `0xffb85000`、`0xfb21b000` 等**系统控制/CRG 窗口**
   （不碰 DPU `0xe8800000`，读它会挂总线）；
2. 6.18 冷启动时 dump 同样窗口；
3. diff 出显示子系统需要的 AOD/时钟/复位位，写进 6.18 的上电驱动；
4. 能稳定读 DPU 之后，再抓 DPU/DSI 的 golden 快照做 Stage 2。

## 3. 关键源码位置

```
drivers/video/fbdev/dkmd/
├── dpu/begonia/composer/power/dpu_comp_power.c   # composer_dpu_power_on / regulator/clk
├── dpu/begonia/composer/dpu_comp_mgr.c           # probe、寄存器映射
├── dpu/begonia/res_mgr/config/v700/dpu_dvfs.c    # dpu_enable_core_clock
├── dpu/begonia/device/gfxdev_mgr.c               # gfxdev_blank_power_on
├── connector/dpu_conn_mgr.c                      # connector 管理（DSI/HDMI/DP）
├── connector/mipi/mipi_dsi_dev.c                 # DSI host
├── connector/hdmitx/…                            # 原生 HDMI TX（本板不用）
└── connector/panel/…                             # 屏（本板不用）
platform_source/basicplatform/drivers/regulator/ip_regulator.c  # AOD 投票/上电
```

## 4. 分阶段计划（每步可独立验证）

### Stage 0：上电 + 能读寄存器
- 移植 `ip_regulator` 的 DSS/media1/vivobus AOD 投票 + 时钟/复位使能到一个小的 6.18 平台驱动
- 目标：6.18 下读 `0xe8800000`/`0xe8900000` 不再挂/SError，且 dump 与 5.10 一致

### Stage 1：DSI + mipi2hdmi 桥片
- 用 DT 给的 I2C 序列初始化桥片（`enable-sequence` 等）
- 配置 DSI host（固定 1920×1080 或桥片支持的模式）
- 目标：HDMI 显示器从“无信号”变成有信号（哪怕先是最小测试图）

### Stage 2：DPU 扫描输出 + framebuffer
- DPU composer 单层 + 线性 framebuffer（先固定分辨率）
- SMMU：优先用预留连续内存 + sid 旁路，避免移植 IOVA 分配器
- 目标：fbcon 出现在 HDMI 上

### Stage 3：EDID/模式/清理
- 读 EDID、支持常见模式、错误处理、电源管理

## 5. 需要的现场条件

- **接一台 HDMI 显示器**（现在两个连接器都报 disconnected，无法验证输出）
- 串口保持可用（6.18 调试仍靠 ttyHW0）

## 6. 风险

- `dkmd` 整体是数十万行、强依赖 vendor 框架的栈，不能整体移植；
  只能按上面分阶段写“最小驱动”。
- DPU composer 的 cmdlist/online 路径是最大的不确定项，
  Stage 2 可能需要按 5.10 运行时的寄存器快照逐位对齐。

---

## 7. 5.10 现场侦察（2026-11-24）

### 7.1 实际显示通路

- 桌面是 **gxde-wlcom**（wlroots Wayland 合成器），打开 `/dev/dri/card0` + `renderD128`
- `/dev/fbex_cmd` 属于**文件加密**子系统（File-Based Encryption eXtension），与显示无关
- `/sys/kernel/debug/dri/0/state` 里 3 个 `DPU-CRTC` 全是 `enable=0`、连接器未接
  → 厂商 DRM 驱动走的是**自定义（非 atomic）路径**，标准 atomic state 不可信
- 内核命令行里带厂商的图形模式变量（`GFXMODE=0,1920x1080`）
- 相关中断：`dp_gpio_hpd` (gpio 18)、`hdmi_bridge` (`ff806000.gpio` 6)

### 7.2 危险窗口（会触发 NOC 复位）

板子的重启原因一直是 `reboot_reason=AP_S_NOC`。实测：

| 窗口 | 读操作结果 |
|---|---|
| `0xffb85000`（CRG） | ✅ 安全 |
| `0xfb21b000`（SCTRL） | ✅ 安全 |
| `0xe8800000`（DPU） | ❌ 进程挂死 → NOC 复位 |
| `0xfb214000`（ACTRL）/ `0xff7cc000` | ❌ 触发 NOC 复位 |

**规则**：6.18 冷启动阶段只碰 `0xffb85000` 和 `0xfb21b000`，
DPU 必须等时钟/复位上电确认后再读。

### 7.3 已保存的快照

- `logs/5.10-crg-ffb85000.txt`：CRG 全窗口 256 个寄存器（显示工作中）
- 其中 `0xffb85e00`~`0xffb85f70` 一段明显是时钟门控/分频（非零），
  是后续和 6.18 对比的重点

### 7.4 下一步

1. 6.18 冷启动后 dump `0xffb85000` + `0xfb21b000`（安全窗口）
2. 与 `logs/5.10-crg-ffb85000.txt` diff，找出 DSS/GIC/DPU 相关门控位
3. 写 6.18 上电驱动（AOD 投票 + 时钟门控 + 复位释放）
4. 确认能安全读 DPU 后，再进入 DPU/DSI 最小扫描输出

---

## 8. 第一次 diff 结果（重要：抓到的是“显示关闭”状态）

### 8.1 6.18 侧快照

- `logs/6.18-crg-ffb85000-off.txt`：CRG 全窗口 256 个寄存器
- SCTRL 只能读到 `0x8e0`（`0xfb21b8e4` 在 6.18 下 SError → panic）
- **注意**：5.10 侧读 `0xfb21b968` 之后也会挂，SCTRL 可读范围约 `0..0x960`

### 8.2 CRG 差异（10 个）

```
ffb85058: 5.10=0x30000180  6.18=0x70000180   xor=0x40000000
ffb8505c: 5.10=0x30000180  6.18=0x70000180   xor=0x40000000
ffb850ac: 5.10=0x00006119  6.18=0x00006101   xor=0x18
ffb850b0: 5.10=0x00001232  6.18=0x00001332   xor=0x100
ffb85100: 5.10=0x00008519  6.18=0x00008509   xor=0x10
ffb85108: 5.10=0x00003425  6.18=0x00003565   xor=0x140
ffb8512c: 5.10=0x0000303f  6.18=0x00003001   xor=0x3e
ffb8521c: 5.10=0x003c0000  6.18=0x00280000   xor=0x140000
ffb85d14: 5.10=0x31840b21  6.18=0x10040021   xor=0x21800b00
ffb85e48: 5.10=0x01820000  6.18=0x01820004   xor=0x4
```

### 8.3 SCTRL 差异（8 个，0..0x8e0 范围）

```
fb21b080: 5.10=0x00000100  6.18=0x00000000
fb21b084: 5.10=0x21000000  6.18=0x00000000
fb21b09c: 5.10=0x00080000  6.18=0x00000000
fb21b120: 5.10=0x000002f1  6.18=0x00000381
fb21b2ac: 5.10=0x00000003  6.18=0x00000004
fb21b358: 5.10=0x00000082  6.18=0x00000080
fb21b41c: 5.10=0x00000400  6.18=0x00000600
fb21b450: 5.10=0x00030005  6.18=0x00010001
```

### 8.4 关键结论：这次 5.10 侧是“显示关闭”状态

抓取时的实际状态：

- `/sys/class/drm/card0-HDMI-A-1/status` = **disconnected**
- 三个 `DPU-CRTC` 全部 `enable=0`
- 显示相关 regulator 全部 **disabled**：
  `dsssubsys`、`vivobus`、`media1_subsys`、`smmu_media1_ip`、`dss_fake`

也就是说：monitor 的 HPD 没有回到板子（多数 KVM/显示器只有在切到该输入时才拉 HPD），
DPU 电源域是关的。所以上面的 diff **不能**用来推断显示门控位。

### 8.5 下一步（需要现场配合）

1. 把显示器输入源切到板子这一路，确认屏幕真的有画面
2. 确认 `card0-HDMI-A-1/status` 变成 `connected`、显示 regulator 变成 `enabled`
3. 在这个状态下重新抓 `0xffb85000` / `0xfb21b000`，再和这次“off”快照对比
   —— 那时的差异才是真正需要复现的显示门控/时钟/复位位

---

## 9. efifb 路线验证：失败（固件确实关了输出）

6.18 启动日志里 efifb 是正常的：

```
efifb: framebuffer at 0x54017000, using 8100k
efifb: mode is 1920x1080x32, linelength=7680
fb0: EFI VGA frame buffer device
```

往 `/dev/fb0` 写黑屏 + 8 MB 随机数据后，**显示器仍然“无信号”**。
结论：UEFI 在 `ExitBootServices` 交接时已经停掉了 DPU/HDMI 的扫描输出，
efifb 只是往显存里写数据，没有任何硬件在扫描它。

→ 显示只能走厂商 DPU 驱动，没有“白捡 framebuffer”的捷径。

## 10. DPU 寄存器无法从外部访问（关键结论）

两种方式都试过，都会触发 `AP_S_NOC` 复位：

| 方式 | 结果 |
|---|---|
| `/dev/mem` mmap 读 `0xe8800000`（用户态） | 进程挂死 → NOC 复位 |
| 内核模块 `ioremap + readl`（`/tmp/dpumod/dpu_dump.ko`） | 同样 NOC 复位 |

（板子每次重启的 `reboot_reason=AP_S_NOC` 就是这么来的。）

原因判断：DPU 窗口在 AP 侧没有做好 NoC/QIC 通路配置时，任何 AP 访问都会被
NoC 判错。厂商驱动能访问，是因为它先把 `dpu_resource` 里的那些 sysctrl 窗口
（`0xffb85000`/`0xffb81000`/`0xefb02000`/`0xefa71000`/`0xff7cb000`/`0xff7cc000`/
`0xe8d01000` …）和 power/clock 配好了。

**结论：不能靠“外部抓 golden 寄存器 → 对照”这条路做显示移植。**

## 11. 显示移植的真实工作量与选项

现状（已确认）：

- 5.10 显示 = 厂商 `dkmd`（`drivers/video/fbdev/dkmd/`，约 100+ 个 .c）
  + 闭源 GPU 用户态（Maleoon 910，`libGFX_hisi` / `libhvgr`）
- 桌面（gxde-wlcom）使用厂商的非 mainline DRM UAPI（`SET_MASTER=DRM_IO(0x1e)`，
  MODE ioctl 偏移 0xA0+），并且 **必须** 跑在 `5.10.97-38-9000c` 内核上
- 6.18 要出画面，只能自己实现：DSS 上电（AOD/时钟/复位/NoC）→ DPU composer
  单层扫描输出 → 连接器（DP/DSI/HDMI）→ fbcon

选项：

- **A. 移植 dkmd 的显示子集到 6.18**（推荐但要按周算）
  - 先做 `dpu_comp_power.c` + `ip_regulator.c` 的 AOD/时钟/复位/NoC
  - 再做 DPU composer 的 online 单层路径（固定 1920×1080 + 线性 FB）
  - 最后接连接器（需要先确认 HDMI 是 DP 还是 DSI 出的）
  - 验收：fbcon 出现在 HDMI
- **B. 只做 5.10 可用性维护**，6.18 继续串口/UFS 用途
- **C. 混合**：6.18 只做无显示服务端（UFS/网络），显示留在 5.10

## 12. 下一步具体动作（如果选 A）

1. 静态确认 HDMI-A-1 走的 encoder：`dkmd/product/dkmd_display_engine.c`
   里 DRM connector/encoder 的创建代码，以及 `connector_manager` 子节点
   （`mipi_dsi@2` / `dp_ctrl@0` / `dp_ctrl@1`）哪个对应 HDMI
2. 把 `dpu_comp_power.c` 的上电序列（regulator/clk/reset/AOD）整理成表
3. 在 6.18 写最小上电驱动，验收 = 读 `0xe8800000` 不再 NOC
4. 之后再做 DPU 单层扫描输出
