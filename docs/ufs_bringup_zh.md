# 6.18 UFS（HUF S）bring-up 记录

- 日期：2026-11-23
- 目标：让 6.18 能正确读写 UFS，并记录 HUF S 控制器的通用初始化路径
- UFS 设备：HISI `THR935GFCV100HDE`（4 个 LUN）
- 控制器：`fa500000.ufs`，compatible `hisilicon,hufs`

> **现状（2026-11-26）**：UFS 已在 6.18 正常工作（HS-G4 / 2 lanes）。
> 以下保留 4 核、UIC 错误、HUF S 队列到 HS-G4 的踩坑和失败记录；
> 文中“下一步/待实施”段落都是当时的计划，不代表当前状态。

---

## 1. 已确认：6.18 能访问 UFS 控制器（关键前提）

用只读探测驱动（`drivers/ufs/host/ufs-charlotte.c`，`CONFIG_SCSI_UFS_CHARLOTTE=y`）实测：

```
ufshcd-charlotte fa500000.ufs: mem[0] [mem 0xfa500000-0xfa57ffff]   # HC
ufshcd-charlotte fa500000.ufs: mem[1] [mem 0xfa3fd000-0xfa3fdfff]   # sys
ufshcd-charlotte fa500000.ufs: mem[2] [mem 0xfa400000-0xfa4fffff]   # mphy
ufshcd-charlotte fa500000.ufs: mem[3] [mem 0xfa58a000-0xfa58afff]
ufshcd-charlotte fa500000.ufs: mem[4] [mem 0xfb8c3000-0xfb8c3fff]   # crg
read-only UFSHCI probe: CAP=0x1187101f VER=0x300 HCS=0x10f HCPID=0x0 IS=0x61
sys window  first word 0xffff
mphy window first word 0xff
crg window  first word 0x1000000
```

结论：

- **UFS 控制器的电源/时钟在 6.18 下是开着的**（与 DPU 完全不同，不需要先做电源域）。
- 控制器是标准 UFSHCI 布局（CAP/VER/HCS 可读，VER=0x300 即 UFSHCI 3.0）。
- 5 个寄存器窗口都能映射（bank0 由 `ufshcd_pltfrm_init` 映射，variant init 里必须用 `hba->mmio_base`，重复映射会 `-EBUSY`）。

---

## 2. 标准模式（mainline ufshcd）尝试与失败点

做法：variant init 映射完直接返回 0，让 mainline 6.18 ufshcd 走标准流程。

结果：

```
ufshcd-charlotte fa500000.ufs: uic command ... completion timeout
ufshcd-charlotte fa500000.ufs: uic cmd 0x16 ... -110
ufshcd-charlotte fa500000.ufs: initialization failed with error -110
```

`0x16` = `UIC_CMD_DME_LINKSTARTUP`。即 **标准 UIC 路径下链路启动超时**。

原因（从厂商 5.10 代码确认）：

- HUF S 的 MPHY 需要在 link startup **之前**做一整套私有初始化：
  `hufs_mphy_v300_pre_link_attr[]`（ASIC 表）、TX VCM 设置、`cdr_reset()`、`ana_eq_slicer_en()`、
  `trailing_clocks_config()`、`saveconfig_handle()`、`wait_mphy_init_done()`、`tx_rx_r_calc()` 等
  （`platform_source/basicplatform/drivers/scsi/ufs/ufs_scorpio.c`，`hufs_link_startup_pre_change()` 约 1066 行起）。
- 这些操作通过 **厂商私有 UIC 接口** 完成：
  `hufs_uic_read_reg()/hufs_uic_write_reg()`，访问 `DME_*` 寄存器（`DME_LINKSTARTUPREQ`、
  `DME_UNIPRO_STATE`、`DME_MPHYCFGUPDT` …），地址编码与标准 SNPS 不同（`snps_to_hufs_addr()`）。
- 只有在 `is_hufs_hc && !standard_uic` 时才走 `vops->hufs_dme_link_startup`；
  标准 UIC 路径在这颗 ASIC 上不足以拉起链路。

### 为什么 5.10 能起来

5.10 的配置：

```
CONFIG_HUFS_HC=y
CONFIG_HUFS_HC_CORE_UTR=y
CONFIG_SCSI_HUFS=y
```

即使用了 HUF S 专有 HC 模式（私有 UTRD 布局 + 私有 UIC + 多队列），并在 probe 时完成 PHY 初始化。

DT 里对应属性（live DT）：

```
ufs-use-hufs-hc = <0x01>;   hufs-use-auto-H8;   hufs-ssu-by-self;
hufs-use-HS-GEAR4;          broken-hce;         hufs-use-main-resume;
clocks = <&clk_ufsio_ref>;  clock-names = "clk_ufsio_ref";
vcc-supply = <...>;         buck-supply = <...>;
```

我们的 6.18 DT **没有** `ufs-use-hufs-hc`，本意是走标准模式；但硬件/PHY 仍需厂商初始化序列。

---

## 3. 厂商 HUF S 代码地图（移植时直接对照）

| 内容 | 文件 | 说明 |
|---|---|---|
| UTRD 私有布局 | `drivers/scsi/ufs/ufshcd.c`（`ufshcd_is_hufs_hc` 分支）+ `include/.../ufs/hufs_hci.h` | `struct hufs_utp_transfer_req_desc`、`ufshcd_hufs_hc_utrdl_alloc()` |
| UIC 私有接口 | `platform_source/.../ufs/hufs_hcd.c` | `hufs_uic_read_reg/write_reg`、`hufs_dme_link_startup()`、`snps_to_hufs_addr()` |
| PHY 初始化 | `platform_source/.../ufs/ufs_scorpio.c` | `hufs_link_startup_pre_change()`、`hufs_link_startup_post_change()`、MPHY 属性表 |
| MPHY 固件 | `platform_source/.../ufs/ufs_mphy_firmware.c`（6220 行） | 是否需要加载取决于 MPHY 版本/模式 |
| 平台 glue | `platform_source/.../ufs/hufs_plat.c` | `hufs_plat_probe`、clocks/resets/sysctrl/pinctrl/pmctrl、`hufs_hce_enable_notify`、`hufs_link_startup_notify` |
| 变体 ops（5.10） | 同上 | `hce_enable_notify` / `link_startup_notify` / `hufs_dme_link_startup` / `ufshcd_hufs_uic_change_pwr_mode` |
| 设备描述/白名单 | `ufs_scorpio.c` / DT 子节点 | Samsung/Hynix 各型号与 gear 限制 |

关键寄存器/地址：

- sys 窗口 `0xfa3fd000`：`UFSSYS_PSW_POWER_CTRL=0x04`、`UFSSYS_PHY_ISO_EN=0x08`、`UFSSYS_PHY_CLK_CTRL=0x10` 等
  （`include/.../ufs/hufs_plat.h`）。
- 私有 UIC：`UFS_VS_IE=0x11C`、`UFS_AHIT_CTRL_OFF=0x110`、`DME_*` 属性地址（`hufs_hcd.h`）。
- `clk_ufsio_ref`、`vcc`/`buck` 供电在 live DT 中存在，6.18 DT 尚未描述。

---

## 4. 结论

- “6.18 能否访问 UFS 控制器” —— **能**，电源/时钟都在，寄存器可读，CAP/VER 正常。
- “mainline ufshcd 直接驱动” —— **不行**，缺少 MPHY 私有初始化，`DME_LINKSTARTUP` 超时。
- 真正要做的是**阶段 1**：移植厂商私有 UIC + MPHY 预初始化，然后 mainline ufshcd 就能接管设备枚举。
- 在此之前不会对 UFS 做任何写操作；写验证被限制在 1 MiB 未分配区。

---

# 附：第二轮实现与对比（2026-11-23 深夜）

## 已实现并验证的部分

1. **私有 UniPro/MPHY 窗口**（resource #2 = `0xfa400000`，dword 索引）：
   `attr` 位于 `base + (attr << 2)`，状态寄存器 `DME_LOCAL_OPC_STATE=0xD066`（busy=0x9）。
   读写轮询实现在 `ufs-charlotte.c`，实测可用。
2. **ASIC MPHY pre-link 初始化**（厂商 `hufs_mphy_v300_pre_link_attr[]` + TX VCM + CDR reset +
   EQ slicer + trailing clocks + saveconfig + `DME_MPHYCFGUPDT` + 等 `MPHY_INIT(D6)` + TX/RX 校准）。
   实测输出：

   ```
   charlotte hufs: MPHY pre-link config
   charlotte hufs: MPHY init done
   charlotte hufs: MPHY pre-link config done
   ```

   即 PHY 侧已经能正确配置并完成初始化。
3. **BROKEN_HCE 路径**：6.18 原生支持 `UFSHCI_QUIRK_BROKEN_HCE`（与厂商 `broken-hce` 分支一致），
   已在 variant init 中置位，内核改为 `DME_RESET + DME_ENABLE` 而不是写 HCE。

## 仍然卡住的地方

标准 UIC 命令（经 HCI 0x90 命令寄存器）**永远等不到完成**：

```
uic cmd 0x14 (DME_RESET) completion timeout -> -110
uic cmd 0x16 (DME_LINKSTARTUP) completion timeout -> -110
```

私有通道写 `DME_LINKSTARTUPREQ(0xD008)=1` 后，`DME_UNIPRO_STATE(0xD053)` 仍保持 `0x80`（LINK_DOWN）。

把 DT 中断顺序改成 SPI120 优先（厂商 `core0-interrupt-index=<1>`）也没有变化。

## 与工作正常的 5.10 的寄存器对比（关键）

5.10 运行中（`charlotte-diag dump`，只读）：

```
HCI 0xfa500000:
  0x00 CAP=1187101f   0x08 VER=00000300   0x24 IE=00071ef5
  0x30 HCS=0000010f   0x34 HCE=00000003        <-- HCE=1 (bit0) + bit1
vendor:
  0xC0 PROC_MODE_CFG = 001c8012
  0x110 AHIT_CTRL=1   0x118=0x114   0x11C VS_IE = 0008e001
SYS 0xfa3fd000:
  0x00=0000ffff  0x08=0000ffff  0x30=00000001  0x34=00000027
CRG 0xfb8c3000:
  0x00=01000000  0x04=5555aaaa  0x08=00121411  0x0c=03032030
```

要点：

- 5.10 里 **HCE 寄存器的值是 0x3**（HCE 已使能，且另一个 bit 也被置位）。说明厂商虽然 DT 标了
  `broken-hce`，实际仍然把 HCE 打开了。
- `PROC_MODE_CFG`、`VS_IE` 都非零，说明厂商在 probe 里配置了私有模式/中断。
- UIC 命令的完成很可能走**厂商 VS 中断**（`VS_IE=0x0008e001`），而 mainline 6.18 的 ISR 只处理
  标准 UFSHCI 中断位，因此 UIC 命令永远等不到完成 → 全部超时。

## 反汇编可行性

- 厂商 5.10 的未压缩 ARM64 Image 和 System.map 有完整符号，可
  `objdump -D -b binary -m aarch64 --adjust-vma=...` 反汇编。
- 注意：Image 带 PE 头，虚拟基址与文件偏移有偏移（`_text = 0xffffffc010080000`，但需要按 0x40
  校准），直接按符号地址反汇编会错位；可用 `--adjust-vma=0xffffffc010080000-0x40` 或先去掉 PE 头。

## 结果

在 `hce_enable_notify(PRE_CHANGE)` 里按 5.10 的实际值写 `HCE(0x34) |= 0x3`：

```
charlotte hufs: HCE before=0x1 HCS=0x10f
charlotte hufs: HCE after=0x3  HCS=0x10f
uic cmd 0x14 (DME_RESET) completion timeout
DME_RESET failed
```

结论：**HCE 不是瓶颈**。控制器已就绪（UCRDY=1），但标准 UIC 命令依旧等不到完成。

## 当前唯一阻塞点

UFSHCI 标准 UIC 命令通道（0x90 命令寄存器 + UIC_COMMAND_COMPL 中断位）在这颗控制器上不工作。
而私有 UniPro 属性窗口（0xfa400000）读写是正常的（PHY 初始化已验证）。

## 已实现（代码已在内核树中）

1. `ufs_soc_init()` 序列（SCTRL/ACTRL 时钟、复位、MPHY ISO、设备硬复位、内部复位释放、内存 TRA）
   → 实测：控制器从 `HCS=0x0` 恢复到可工作状态。
2. MPHY pre-link 配置（厂商 v300 表 + CDR/EQ/TX-RX 校准 + `MPHY_INIT` D6 等待）
   → 实测 `MPHY init done`。
3. 私有 UniPro 链路启动（`DME_LINKSTARTUPREQ=1` 轮询 `DME_UNIPRO_STATE`）
   → **实测 `link up (state 0x100)`**（此前一直 0x80）。
4. HUF S 多队列（CONFIG_HUFS_HC_CORE_UTR 等价实现）：
   - UTRD 结构扩为 48 字节（4 个 meta dword）
   - UTRL 布局：前 32 个 slot 为 spec 槽，之后 64 个为 per-core 槽
     → 软件 tag i 对应硬件 UTRD 槽 `i + 32`
   - 门铃：`UFS_CORE_UTRLDBR(tag/8) = 0x2014 + (tag/8)*0x80`，bit `tag%8`
   - 运行/停止：`UFS_CORE_UTRLRSR(i) = 0x201C + i*0x80`
   - 中断：8 条 per-core 中断（DT 中 SPI 120..127），`UFS_CORE_IS(i)=0x2000+i*0x80`，
     `UFS_CORE_IE(i)=0x2004+i*0x80`
   - 完成：读对应 core 的 UTRLDBR 清零位，调用 `__ufshcd_transfer_req_compl`

   实测：8 条 core IRQ 注册成功，core0 `UTRLRSR=1`、`IE=3`，门铃被控制器消费。

## 当前卡点

链路起来后，主中断报告 `saved_err = 0x10004`、IS 全为 error-code 位（0x71EF5），
触发 `ufshcd_err_handler`（eh_fatal），随后 err_handler 里空指针 oops（`hba->ufs_device_wlun` 尚未建立），
系统控制台卡死。

判断：HUF S 模式不使用标准 UIC 错误上报，`REG_INTERRUPT_STATUS` 里的 UIC/UTP 错误位是无效值，
mainline 的 ISR 把它当成致命错误。

## 根因

第四轮之后门铃写进去但控制器不取（`DBR` 一直挂着），且写门铃的瞬间 `UFS_CORE_UTRLRSR`
从 1 变 0、主中断报 `CONTROLLER_FATAL_ERROR`。

对照手机源码 `ufs_scorpio.c:hufs_link_startup_post_change()` 后发现：厂商在
**link startup 的 POST_CHANGE** 里会写一张 UniPro **N_/T_（network/transport）连接属性表**：

```
N_DeviceID / N_DeviceID_valid / T_ConnectionState / T_PeerCPortID /
T_PeerDeviceID / T_TrafficClass / T_ConnectionState=1
```

我们之前完全没做。结果就是：DME 层 `DME_UNIPRO_STATE=0x100`（LINK_UP）看着正常，
但网络/传输层没有建立连接，第一条 UPIU 传不出去 → 控制器 fatal error → 队列被复位
（RSR 清 0）→ 门铃永远不被消费。

## 最终改动

1. **`drivers/ufs/host/ufs-charlotte.c`**
   - 新增 `hufs_mphy_v300_post_link_attr[]`（逐条照抄 `ufs_scorpio.c`）并在
     `POST_CHANGE` 里 `charlotte_hufs_phy_post_link()` 应用：
     N_/T_ 连接属性、RX H8 timeout、`VCO_AUTO_CHG`、`PA_TActivate=5`；
     再按 `check_pa_granularity()` 保证 `PA_GRANULARITY >= 6`。
   - 保留 `ufs_soc_init`、MPHY v300 pre-link、私有 UniPro 链路启动、QoS 配置。
2. **`drivers/ufs/core/ufshcd.c`（按厂商 5.10 的 core-UTR 语义）**
   - `ufshcd_hba_capabilities()`：`hba->nutrs = 8 * 8 = 64`（厂商
     `SLOT_NUM_EACH_CORE * CORE_NUM`）→ `reserved_slot = 63`（core 7 / bit 7）。
   - per-core `IS` 改为**写回读到的值**清零（厂商写法），`IE |= 3`；
     不再写 `0xffffffff`。
   - `ufshcd_clear_cmd()`：core 模式用 `UFS_CORE_UTRLCLR(core)` 并在
     `UFS_CORE_UTRLDBR(core)` 上等门铃清零（厂商写法）。
   - `ufshcd_hba_stop()`：先关 per-core `UTRLRSR`（厂商 `ufshcd_disable_run_stop_reg`）。
   - ISR 里 core 模式屏蔽假的 `INT_FATAL_ERRORS | UIC_ERROR`。
3. **DMA**：`set_dma_mask` 限 32 位（UTRDLBA 落在 4 GiB 以下，和 5.10 一致）。
4. **内核配置**：`CONFIG_BLK_DEV_SD=y`（之前没开，SCSI 能枚举但不出 `/dev/sd*`）。

## 现象

6.18 下 UFS 能正常枚举、读写数据也正确，但**极慢**：

| 测试 | 5.10 | 6.18（修复前） |
|---|---|---|
| 单次 4 KiB 读 | 0.056 ms | 9.67 ms |
| 1 MiB 读 | ~1 ms | 1719 ms |
| 1 GiB 顺序读 | 987 MB/s | ~0.6 MB/s（跑不完） |

## 根因

链路一直停在 **PWM-G1**（M-PHY PWM gear 1，约 3 Mbit/s，正好 0.6 MB/s）。

mainline 的齿轮切换流程是：

```
ufshcd_device_params_init()
  -> ufshcd_get_max_pwr_mode()        // 读 PA_CONNECTED*LANES / PA_MAX*GEAR
  -> ufshcd_post_device_init()
       -> ufshcd_config_pwr_mode()    // 写 PA_TXGEAR/PA_RXGEAR/... + PA_PWRMODE
```

其中所有 `ufshcd_dme_get/set()` 都走标准 UIC 命令口（`REG_UIC_COMMAND`），
而这个控制器上标准 UIC 命令**永远不完成**（之前 UIC 命令超时就是这个问题）。
于是 `ufshcd_get_max_pwr_mode()` 失败 → `max_pwr_info.is_valid = false` →
`ufshcd_post_device_init()` 直接返回，**从不切 HS**，一直跑 PWM-G1。

## 修复（补丁 0006）

1. `ufshcd_dme_get()` / `ufshcd_dme_set()` 增加变体钩子，把本地属性读写
   改走私有 UniPro 窗口（`0xfa400000`）。
   注意编码转换：核心用 `(mib << 16) | selector`，而窗口是
   `(selector << 16) | mib`（1 MiB = 4 个 selector bank，每个 256 KiB）。
   直接把核心的 attr 左移 2 位当偏移会越界（第一次就 oops 在这里）。
2. `ufshcd_get_max_pwr_mode()` 增加变体钩子。连接 lane 数和最大齿轮是
   **peer** 属性，peer DME 路径同样不可用，所以由驱动直接给出与厂商 DT
   （`hufs-use-HS-GEAR4` / `hufs-use-two-line`）一致的参数：
   **HS-G4 / 2 lanes / rate B**。
3. `PA_PWRMODE` 写入改为轮询 `DME_POWERMODEIND` 的 busy 位并重试；
   成功后在 `pwr_change_notify(POST_CHANGE)` 里重新应用
   `PA_TActivate` / `PA_GRANULARITY` 等 post-link 参数。

## 修复后（同一份静态 O_DIRECT 微基准）

| 测试 | 5.10 | 6.18（HS-G4） |
|---|---|---|
| 读 1 GiB, bs=1M, 偏移 0 | 987 MB/s | 931 MB/s |
| 读 1 GiB, bs=1M, 偏移 64 GiB | 1223 MB/s | 880 MB/s |
| 读 64 MiB, bs=4K | 74 MB/s | 85 MB/s |
| 读 64 MiB, bs=128K | 705 MB/s | 437 MB/s |
| 单次 4 KiB 读延迟 | 0.056 ms | 0.116 ms |
| 单次 1 MiB 读延迟 | ~1 ms | 1.11 ms |

结论：顺序读与 5.10 同一水平（931 vs 987 / 880 vs 1223 MB/s）；
小请求延迟约为 5.10 的 2 倍，但已完全可用。

剩余差距主要来自 6.18 的队列上限（`max_sectors_kb=256`、`max_segments=128`，
5.10 是 1280 / 256），后续可以把 PRDT 数量和 `max_segments` 提上去再优化。

`dmesg` 关键行：

```
charlotte hufs: max pwr mode HS-G4, 2 lanes, rate B
charlotte hufs: PA_PWRMODE 0x11 -> DME_POWERMODEIND 0x0 (tries 1)
charlotte hufs: pwr change done: ind=0x2 gear=4 lanes=2 pwr=1 hs_rate=2
```
