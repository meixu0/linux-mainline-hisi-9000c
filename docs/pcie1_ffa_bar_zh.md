# PCIe1 / FF-A / BAR 窗口与 AMD Oland 显卡打通（2026-11-25）

本文记录 6.18 上把 **PCIe1（外部 x1 插槽）** 上的 AMD Oland 显卡完整
打通的过程与结论。PCIe0（板载 ASM1064）此前已可用，不在本文范围内。

状态：**已完成**。radeon 下 PCIe 枚举、VRAM 访问、GART、5 个 ring、
IB 测试、显示输出全部通过，屏幕上可读到正常的内核日志文字。

## 0. 结论摘要

| 项目 | 结果 |
|---|---|
| PCIe1 DBI（`0xedc00000`）可读 | 需要安全世界放行，走 FF-A 直连安全分区 |
| 链路 | Gen3 x1，`edc00000.pcie` 正常枚举 Oland `[1002:6611]` |
| BAR0（256 MiB）的 CPU 可见方式 | 总线侧放 `0x90000000`，经低 4 GiB 的 56 MiB 窗口映射 |
| VRAM 的 CPU 映射属性 | 必须 Device（`ttm_uncached`），不能 Normal-NC |
| GTT（系统内存）的一致性 | 必须非缓存（`ttm_write_combined`），不能 cached |
| CPU→VRAM 是否真实落地 | 是，BAR 与 GPU 引擎看到同一份 VRAM |
| radeon 命令提交 | 5 个 ring + 5 个 IB 测试全部通过 |
| 显示 | 1080p 帧缓冲输出正常，屏幕文字可读 |
| amdgpu | 尚未迁移，见第 6 节 |

## 1. PCIe1 的放行：FF-A 直连安全分区

固件默认把 PCIe1 的地址块留在安全世界，未放行时任何访问（包括用户态
`devmem`）都得到异步 SError：

```
SError Interrupt on CPU10, code 0x00000000be000011
pc : dw_pcie_read+0x88/0xa0
lr : dw_pcie_read_dbi+0x60/0xb0
```

### 1.1 实现方不是 ATF 的 SiP 服务

实测把该 FID 当普通 SMC 发送，返回的是**原样回显**：

```
FFA/SMC 0xc3002702(rc1) -> 0xc3002702 0x1     /* a0 == FID，未被处理 */
```

说明 ATF 的 SiP 处理函数没有这个 FID，实现方是安全分区。因此 mainline
的 FF-A 传输层是唯一可行路径。

### 1.2 mainline FF-A 传输层为什么起不来

`drivers/firmware/arm_ffa/smccc.c` 要求 SMCCC ≥ 1.2：

```c
	if (arm_smccc_get_version() < ARM_SMCCC_VERSION_1_2)
		return -EOPNOTSUPP;
```

本固件经 PSCI 报 `smccc=0x10000`（1.0），`ffa_init()` 直接返回且不打
日志，`/sys/bus/ffa/devices` 为空。故在板级驱动里自带直连实现。

### 1.3 直连调用序列

```text
FFA_VERSION (0x84000063) args=0x10001        -> 0x10001
FFA_ID_GET  (0x84000069)                     -> a2 = NS endpoint id (0)
FFA_MSG_SEND_DIRECT_REQ (0x8400006f)
    a1 = (src << 16) | dst , dst = 0x8004
    a3 = FID (0xc3002702 / 0xc3002700)
    a4 = rc_id (1)
    -> FFA_MSG_SEND_DIRECT_RESP (0x84000070), a3..a5 = 返回数据
```

- 目标分区 id 是 `0x8004`，该 SPMC 不回填 UUID，不能按 UUID 匹配；
- 直连请求不需要 RX/TX 缓冲区，可完全跳过共享内存握手；
- 该 SPMC 上 `FFA_RX_RELEASE` / `FFA_RXTX_UNMAP` 可能把 CPU 卡死，
  走直连请求就不需要碰它们；
- 若要用 `FFA_PARTITION_INFO_GET`，缓冲区地址必须取自平台 SMC
  `0xc3002d01`，自己分配的页会被 `FFA_RET_DENIED (-6)` 拒绝。

放行后：

```
charlotte-pcie edc00000.pcie: FFA 0xc3002702(rc1) ok: 0x0 0x1 0x0
charlotte-pcie edc00000.pcie: DBI id=0x36b019e5 link=0
charlotte-pcie edc00000.pcie: PCIe Gen.3 x1 link up
charlotte-pcie edc00000.pcie: host ready, link=1
```

`FID_PCIE_SET_TBU_BYPASS` 同时下发（DT 没有 `iommus`）。TBU1/HSDT
寄存器读回与厂商 5.10 逐项相同，说明 bypass 生效。

## 2. 窗口布局：为什么必须搬 BAR0

### 2.1 平台只给了 59 MiB

固件 DT 里 PCIe1 只有一段 32 位非 prefetchable 窗口：

```
0xea100000 + 0x03b00000   /* 59 MiB */
```

配合 config 窗口（`0xea000000`）和 DBI（`0xedc00000`），这一块总共
64 MiB，正好被 BAR2（256 KiB）、ROM（128 KiB）、DBI、PHY、PCIe0 寄存器
等填满。插槽里这张卡的 BAR0 是 **256 MiB** 的 64 位可预取 BAR，塞不进去。

厂商 5.10 的运行时枚举也印证了这一点：它枚举到了这张卡，但 BAR0
**未分配**（resource 全 0），只用了 BAR2 `0xea100000` 和 ROM
`0xea140000`。

在 5.10 上把未分配的 BAR0 临时写全 1 再读回，掩码为
`0xF000000C / 0xFFFFFFFF`，确认大小就是 256 MiB。

### 2.2 `0x1000000000` 不能用

早期我们自造了一段 64 位可预取窗口 `0x1000000000 + 0x10000000`，
PCI 核心分配 BAR0 到这里，读写看起来也自洽。但这是假象：

- 经 BAR 写入 `0xdeadbeef` 后，把 GPU 的 `PCI_COMMAND.MEMORY` 关掉
  （`0x0006 → 0x0004`），**同一地址仍读回 `0xdeadbeef`**；
- 由该 BAR 解码的地址在解码关闭后应当得不到响应，仍能读回说明这段
  根本没有到达 GPU，而是被 SoC 侧别的存储接住了。

同时驱动里 dump 的出向 iATU 证明 RC 侧配置是对的（见 2.4），所以问题
不在翻译，而在这一段地址本身没有被路由到 PCIe1。这条路作废。

### 2.3 采用方案：把 BAR0 搬到低窗口内的子窗口

约束是 BAR0 的基址必须按真实大小 **256 MiB 对齐**，所以不能直接放在
`0xea200000`。做法是：

- 总线侧选一个 256 MiB 对齐的空地址 `0x90000000`；
- CPU 侧用低 4 GiB 里已确认可达的 `0xea200000` 起 56 MiB 映射过去。

当前 DTS：

```dts
ranges = <0x02000000 0x0 0xea100000 0x0 0xea100000 0x0 0x00100000>,
         <0x42000000 0x0 0x90000000 0x0 0xea200000 0x0 0x03800000>,
         <0x43000000 0x10 0x00000000 0x10 0x00000000 0x0 0x10000000>;
```

- 第一段 1 MiB：保留 BAR2、ROM、audio BAR；
- 第二段 56 MiB 可预取：CPU `0xea200000–0xed9fffff` → PCI
  `0x90000000–0x937fffff`；
- 第三段仍保留原来的 64 位窗口，只为让 PCI 核心正常完成 BAR 分配，
  之后再在驱动里把设备 BAR0 改写走（见 2.4）。这段地址不可用于访问。

BAR0 在设备绑定驱动之前改写（`BUS_NOTIFY_BIND_DRIVER`）：

```
DIAG618 BAR0 relocated=0x9000000c/0x0
```

即地址 `0x90000000`，仍为 64 位可预取。

### 2.4 出向 iATU 实测

用 DWC 访问器 dump 出向窗口（窗口寄存器在 `dbi_base + 0x300000`，
`DBI + 0x900` 只是 viewport，不是窗口寄存器）：

| 窗口 | 使能 | 类型 | CPU 侧 base–limit | PCI 侧 target |
|---|---|---|---|---|
| ob0 | 是 | CFG0 | `0xea000000–0xea00ffff` | `0x01010000` |
| ob1 | 是 | MEM | `0xea100000–0xea1fffff` | `0xea100000` |
| ob2 | 是 | MEM | `0xea200000–0xed9fffff` | `0x90000000` |
| ob3 | 是 | MEM | `0x1000000000–0x100fffffff` | `0x1000000000` |

`/proc/iomem` 也一致：

```
ea100000-ea1fffff : PCI Bus 0000:01
  ea100000-ea13ffff : 0000:01:00.0   (BAR2)
  ea140000-ea15ffff : 0000:01:00.0   (ROM)
  ea160000-ea163fff : 0000:01:00.1   (audio)
ea200000-ed9fffff : PCI Bus 0000:01
```

其中 `ea200000-ed9fffff` 这一段就是 ob2 的 CPU 侧范围，对应 GPU 的
BAR0（PCI 地址 `0x90000000` 起）。

## 3. 根因与修复

打通需要三件事同时成立，缺一不可，而且前两次失败恰好各自被另一个掩盖。

### 3.1 出向访问必须用 Device 映射

这条访问链是：

- `arch/arm64/include/asm/io.h`：`ioremap()` 用 `PROT_DEVICE_nGnRE`，
  `ioremap_wc()` 用 `PROT_NORMAL_NC`；
- `ttm_bo_ioremap()`：`ttm_write_combined` → `ioremap_wc()`，其余
  （含 `ttm_uncached`）→ `ioremap()`；
- `radeon_ttm_io_mem_reserve()` 原本给 VRAM 一律设 `ttm_write_combined`。

所以 radeon 对 VRAM 的所有 CPU 访问（GART 表、RLC clear-state、IH
ring……）都是 **Normal-NC**。而用户态能正常读写同一窗口，是因为
`/dev/mem` 走 `phys_mem_access_prot()` → `pgprot_noncached()`，即
**Device-nGnRnE**。

同一物理地址、两种映射属性，行为完全不同：

| 路径 | 映射属性 | 结果 |
|---|---|---|
| `devmem` → `/dev/mem` | Device-nGnRnE | 读写正常 |
| `radeon_bo_kmap` → `ioremap_wc` | Normal-NC | 首次写即 SError |

把 VRAM 的缓存属性改成 `ttm_uncached`（→ `ioremap()` → Device-nGnRE）
后，SError 完全消失。这与上游社区结论一致：arm64 上多数 SoC 的 PCIe
接口不支持 Normal-NC 语义的 outbound 事务（可合并、可乱序、可非对齐），
必须用 Device 属性。

改法（`radeon_ttm_io_mem_reserve()`）：

```c
		mem->bus.offset += rdev->mc.aper_base;
		mem->bus.is_iomem = true;
		if (rdev->mc.aper_base == 0xea200000)
			mem->bus.caching = ttm_uncached;
		else
			mem->bus.caching = ttm_write_combined;
```

### 3.2 GTT（系统内存）对象必须非缓存

ring 不在 VRAM：`CP_RB0_BASE = 0x00400010` 对应 GPU 地址
`0x40001000`，落在 GART 区间，即 **ring 在系统内存里，由 GPU 经 GART
读取**。

`radeon_ttm_tt_create()` 默认给 GTT BO 用 `ttm_cached`，于是 CPU 写 ring
只落在 cache，GPU 非嗅探读 DRAM 只能看到旧内容，CP 永远取不到指令。

改法：本文板把 `ttm_cached` 的 GTT 对象改成 `ttm_write_combined`
（Normal non-cacheable 系统内存）。

对照组很清楚：仅修 3.1 时 ring 测试仍失败
（`scratch(0x850C)=0xCAFEDEAD`，`CP_ME_CNTL=0x4100` 表示 ME 未 halt、
`GRBM_STATUS=0xA0003028` 表示 CP 一直 busy，但 scratch 从未被写回）；
加上 3.2 后立刻全过。

### 3.3 地址必须真实路由

即第 2 节。BAR0 必须放在总线侧 256 MiB 对齐、且由低 4 GiB 窗口可达的
位置，而不是 `0x1000000000`。

## 4. 验证结果

Image SHA256：
`0e1f97e971d1fccd4fc4010558e70fef8b485433075175a91a4da09013d8c772`，
构建号 `#110`。

命令提交：

```
[drm] ring test on 0 succeeded in 1 usecs
[drm] ring test on 1 succeeded in 1 usecs
[drm] ring test on 2 succeeded in 1 usecs
[drm] ring test on 3 succeeded in 9 usecs
[drm] ring test on 4 succeeded in 6 usecs
[drm] ib test on ring 0 succeeded in 0 usecs
...
[drm] ib test on ring 4 succeeded in 0 usecs
```

显示：

```
[drm] Radeon Display Connectors: DP-1, DVI-I-1
[drm] fb mappable at 0xEA661000
[drm] vram apper at 0xEA200000
[drm] size 33177600, depth 24, pitch 15360
Console: switching to colour frame buffer device 480x135
[drm] fb0: radeondrmfb frame buffer device
```

屏幕上可见正常的内核日志文字，扫描输出读到的就是 CPU 经 BAR 写进
VRAM 的帧缓冲内容。

已知限制：BAR0 只有 56 MiB 通过窗口暴露给 CPU，TTM 的 VRAM 管理器据此
收缩到 56 MiB 可见；GPU 自身仍可用完整 1 GiB VRAM（GART 表、ring、
显示都在其中）。

补充验证（Image SHA256
`d88eaf587853321fb23603ff890bfd46e4ddf30a711d2eaa7f1a5fc38a457495`，
构建号 `#111`）：把 `rs600_gart_set_page()` 恢复成普通 `writeq()`（不再走
`MM_INDEX/MM_DATA` 间接通路）后，GART 表写入正常，5 个 ring + 5 个 IB 测试
依旧全部通过。说明之前的 MM 通路 hack 是不必要的。

## 5. 与上游 `arm-pcie-gpu-patches` 的对应关系

本项目在排查和修复过程中**参考了**
[`mariobalanica/arm-pcie-gpu-patches`](https://github.com/mariobalanica/arm-pcie-gpu-patches)
（Linux 内核补丁集，用于在 arm64 平台上驱动外接 GPU，基于 Ampere Altra
的 PCIe erratum 补丁扩展而来）。该仓库恰好覆盖本板遇到的同类问题：

| 补丁 | 内容 | 与本板的关系 |
|---|---|---|
| `0002-arm64-mm-Force-Device-mappings-for-PCIe-MMIO` | 钩住 `pte_mkspecial()`，把 PCIe MMIO 的 Normal-NC 强制改成 `pgprot_device` | 正是 3.1 的通用版本；本板先在 radeon 侧做等价改动 |
| `0001-arm64-mm-Handle-alignment-faults` | 捕获对齐异常，把非对齐多字访问拆成对齐访问 | 强制 Device 后必需；用户态对 VRAM 的 `memcpy` 也会触发 |
| `0003-drm-Force-writecombined-mappings-for-DMA` | TTM 的 sysmem 分配强制 WC | 正是 3.2 的通用版本 |
| `0004-PCI-dwrockchip-Disable-root-port-BARs` | 关掉 root port 自己的 BAR | 本板 `0000:00:00.0 BAR 0 [mem size 0x01000000]` 也分配失败、白占 16 MiB |

仓库 README 中列出的三类 arm64 常见障碍也与本板一一对应：

- **MMIO 空间不足**：本板只有 59 MiB 低窗口，放不下 256 MiB 的 BAR0；
- **MMIO 严格访问要求**：Normal-NC 出向事务直接 SError，必须 Device；
- **非一致 PCIe**：GTT 系统内存必须非缓存，否则 GPU 读到旧数据。

后续若要在本板上做通用化，建议直接采用 0001+0002+0003，并把 radeon 侧的
局部改动撤掉；0004 可在需要更大低端窗口时再评估。

## 6. 尚未完成

1. **迁移到 amdgpu（当前主线）**：radeon 在本板受“CPU 不能宽写 VRAM”和
   “legacy KMS”两条限制，已决定放弃，迁移方案见第 11 节。
2. **清理诊断打印**：`DIAG618` 系列（`vram uncached`、`gtt wc`、
   `gart gpu=`、iATU dump）只用于定位，正式版本应移除。
3. **桌面栈验证**：等 amdgpu 拿到 atomic KMS 后再验证现代合成器；
   目前只验证到 GPU 渲染与 fbdev 输出。
4. **视频硬解**：`oland_uvd.bin` 未内嵌，UVD 不可用（非阻塞项）。

## 7. 已被推翻、不要重复的结论

以下都是排查过程中出现过的判断，后来被实验否定，正文已删除：

| 曾经的结论 | 为什么错 |
|---|---|
| `0x1000000000` 是可用高窗口 | 关掉 GPU 的 MEM 解码后仍能读回写入值，说明根本没到 GPU |
| BAR 与 MM 两条路径指向不同存储 | 前提是 BAR 走的是 `0x1000000000` 这段无效地址；换到有效窗口后两条路径一致 |
| HDP 缓存未失效导致 BAR 写不落地 | 同上，窗口无效才是主因 |
| 地址混洗/偏移映射 | 在无效窗口上做的落点搜索，结论无意义 |
| BAR0 在 `si_startup` 早期不可用 | 实际是映射属性问题；Device 映射下早期访问正常 |
| 64 位写不被支持 | 拆成两次 32 位写仍然 SError，与宽度无关 |
| GART 表必须在 `si_startup` 早期走 MM 间接通路 | 恢复普通 `writeq` 后一切正常，Device 映射下窗口从第一次写就可用 |
| 显示通路已工作 / 显示永远花屏 | 两次都是在错误窗口下的观察 |

另外两个方法性教训：

- 用 `devmem` 从 `0xedc00000 + 0x900` 读到全零，是因为那里只是 iATU
  viewport 而非窗口寄存器；窗口寄存器在 `dbi_base + 0x300000`。
- 回滚单个源文件时用保留 mtime 的 `cp -a` 会让 `make` 认为无需重编译，
  镜像里仍是旧目标文件。回滚后必须确认构建日志里出现对应的 `CC`。

## 8. 复现

### 8.1 需要的源码改动

1. `dts/charlotte.dtsi`：pcie1 `ranges` 拆成 1 MiB + 56 MiB + 原 64 位窗口
   （见 2.3）。
2. `drivers/pci/controller/dwc/pcie-designware-host.c`：在
   `BUS_NOTIFY_BIND_DRIVER` 且 `device == 0x6611` 时把
   `PCI_BASE_ADDRESS_0/1` 写成 `0x90000000 / 0`。
3. `drivers/gpu/drm/radeon/si.c`：`si_mc_init()` 里对 `1002:6611` 覆盖
   `aper_base = 0xea200000`、`aper_size = visible_vram_size = 0x3800000`。
4. `drivers/gpu/drm/radeon/radeon_ttm.c`：
   `radeon_ttm_io_mem_reserve()` 的 VRAM 分支用 `ttm_uncached`；
   `radeon_ttm_tt_create()` 把 `ttm_cached` 改成 `ttm_write_combined`。
5. initramfs 里放入 `radeon/oland_*.bin`（`oland_ce.bin`/`oland_me.bin`
   是 `hainan_*` 的软链，复制时要去引用）。

### 8.2 构建与启动

```sh
make ARCH=arm64 -j12 Image
make ARCH=arm64 hisilicon/charlotte-9000c.dtb
```

GRUB 一次性入口（**不要改默认项**）：

```
linux /boot/linux-6.18-charlotte/Image earlycon=uart_v500,0xff894000 \
      console=tty0 console=ttyHW0,115200n8 loglevel=8 \
      arm64.nopauth panic=20 pcie_aspm=off pcie_port_pm=off
devicetree /boot/linux-6.18-charlotte/charlotte-9000c.dtb
initrd /boot/linux-6.18-charlotte/initramfs-console.cpio.gz
```

串口在 `ttyHW0`（115200 8N1）。PCIe1 上电后：
`BAR2 = 0xea100000`，BAR0 总线地址 `0x90000000`、CPU 侧 `0xea200000`，
`DBI = 0xedc00000`。

## 9. 相关文件

| 路径 | 内容 |
|---|---|
| `drivers/pci/pcie-charlotte.c` | RC 驱动镜像（FF-A 直连、iATU 配置） |
| `drivers/pci/controller/dwc/pcie-designware-host.c` | 板端 iATU dump + BAR0 重定位 |
| `drivers/gpu/drm/radeon/{si.c,radeon_ttm.c,radeon_gart.c,rs600.c}` | radeon 侧改动与诊断 |
| `dts/charlotte.dtsi` | pcie1 窗口定义 |
| `patches/` | 已归集的补丁（0008 DTS、0009 FF-A） |

## 10. 真实 rootfs 验证（2026-11-25）

在 initramfs 里验证通过后，用 6.18 直接挂载真实 rootfs 启动。为此需要补
四项与 GPU 无关、但缺一就起不来的东西：

| 问题 | 现象 | 处理 |
|---|---|---|
| 根设备解析 | `VFS: Cannot open root device "UUID=..."`（`unknown-block(0,0)`） | 改用 `root=PARTUUID=... rootwait rootfstype=ext4`；UFS 是异步扫描，`UUID=` 不可靠 |
| systemd | `Failed to mount tmpfs ... /sys/fs/cgroup` → `Freezing execution` | `CONFIG_CGROUPS=y`（含 SCHED/FREEZER/DEVICE/CPUACCT/BPF） |
| 必需挂载 | `/boot/efi`（fstab 里 vfat）挂不上 → 进 emergency mode | `CONFIG_VFAT_FS`、`MSDOS_FS`、`NLS_CODEPAGE_437`、`NLS_ISO8859_1` |
| 固件加载 | `radeon/OLAND_pfp.bin` 加载失败，GPU init 直接放弃 | radeon 是内建驱动，`module_init` 早于 rootfs 挂载，`/lib/firmware` 还不存在；把 `oland_*.bin` 用 `CONFIG_EXTRA_FIRMWARE` 编进内核 |

补齐后 rootfs 正常启动，radeon 完整初始化（5 ring + 5 IB 全过）。

### 10.1 已验证可用的 GPU 路径

| 测试 | 方式 | 结果 |
|---|---|---|
| ring / IB | radeon 初始化自测 | 5 + 5 全过 |
| CPU 写 VRAM | 自写 `vramtest`：dumb buffer 1920×1080、mmap、8 MB 写入读回 | 正常，无 SError |
| GPU 渲染 | 自写 `vram3d`：surfaceless EGL + GLES2，`radeonsi`(oland) 渲染 10 帧并 `glFinish`、读回像素 | 正常，像素值正确 |

### 10.2 剩余问题：登录管理器触发 SError

图形目标启动时，`gxdm`（Treeland 系合成器 `gxde-wlcom`）会在映射一个
1920×1080 的 VRAM 缓冲（`0xea681000`，`0x7e9000` 字节）之后立刻触发
异步 SError 并 panic，可稳定复现：

- 强制 `WLR_RENDERER=pixman` 后仍然复现，说明与渲染后端无关；
- 纯 CPU 写同一规格的 VRAM 缓冲不复现；
- EGL/GLES 真渲染不复现；
- `multi-user.target` 下完全稳定。

因此这是合成器特有的路径（KMS 模式设置/扫描输出相关），尚未定位到
具体 ioctl。注意每次 panic 后 rootfs 需要做 fsck（231 GB），重启代价
较高，后续排查建议先在 `multi-user` 下用最小程序复现。

### 10.3 连接器 `disconnected` 的原因：显示器输入源

`card0-DP-1` 与 `card0-DVI-I-1` 一度都报 `disconnected`、EDID 为 0，
radeon 因此报 `Cannot find any crtc or sizes`、不注册 `fb0`。后续确认
这只是**显示器没有切到该卡的输入源**，不是 GPU 侧 HPD/DDC 或 DCE 的问题，
不需要在驱动里排查。

### 10.4 SError 的根因：单次写不能超过 8 字节

用自写的最小程序（`vtest`，把每一步写进 `/dev/kmsg`，panic 后仍能在串口
日志里看到最后一步）在同一个 1920×1080 VRAM dumb buffer 上逐项对照：

| 填充方式 | 结果 |
|---|---|
| 显式 4 字节 / 8 字节存储 | ✅ 正常 |
| `dc zva, xN`（按缓存行清零） | ✅ 正常 |
| `str q0, [x], #16`（16 字节 SIMD） | ❌ 立刻 SError |
| `stp q0, q0, [x], #32`（32 字节） | ❌ 立刻 SError |
| glibc `memset`（内部使用 SIMD） | ❌ 立刻 SError |
| `AddFB` / `SetCrtc` / `PageFlip` | ✅ 不触发（`SetCrtc` 因无连接器返回 `-EINVAL`） |

结论：**这条 NoC 路径到 PCIe1 不支持大于 8 字节的单次写事务**。超出宽度
的写会被下游拒绝，以异步 external abort（SError）返回给 AP。

这条结论同时解释了前面所有相关现象：

- 内核用 Normal-NC（`ioremap_wc`）时会 panic：write-combining 会把相邻的
  4 字节写合并成更宽的突发，等于发出了 >8 字节的事务；改成 Device 映射
  后不再合并，所以 4/8 字节全部正常；
- 桌面合成器（Treeland/`gxde-wlcom`）一启动就 panic：它对 VRAM 帧缓冲做
  `memset`/`memcpy`，glibc 走 SIMD 路径，必然发出 16/32 字节写；
- 纯 GPU 渲染不受影响：Mesa 的上传走 GTT（系统内存），渲染由 GPU 执行。

`pages` 属性为 Device 时，ARM 架构规定 16 字节及以上的访问是
CONSTRAINED UNPREDICTABLE；多数实现会报对齐异常，可以像
`arm-pcie-gpu-patches` 的 0001 那样在异常处理里拆成对齐访问，但本板
**不报同步对齐异常，而是让事务下到 NoC 后被拒绝**，因此内核没有机会拆分。

**已用厂商 5.10 内核做对照，确认这是硬件固有约束**：在同一张卡的
BAR2（`0xea100000` + `0x8500` 起始的 scratch 寄存器区）上，5.10 内核：

- 8 字节写读回 `0x1122334455667788`，正确；
- 4 字节写读回 `0xDEADBEEF`，正确；
- 16 字节 `str q` 直接 SIGSEGV。

所以宽度限制与 6.18 移植无关，也不是 DT 里没用上的 `noc_target_id`
造成的（该属性确实被厂商 `pcie_kport` 读取，但设备在厂商系统下 BAR0
根本没分配，说明厂商从未打算让 CPU 直接访问这块 VRAM）。

这意味着：在本板上，用户态（glibc 的 `memset`/`memcpy`、任何 SIMD 拷贝）
不能直接写 VRAM 映射。

## 11. 决策：放弃 radeon，迁移到 amdgpu

> amdgpu 侧的具体改动、根因与当前状态见
> [`amdgpu_bringup_zh.md`](amdgpu_bringup_zh.md)。

### 11.1 为什么 radeon 在本板走不通

不是 radeon 有 bug，而是它的两个前提在这块板子上都不成立：

1. **它假设 VRAM 可以由 CPU 直接映射并随意写。**
   radeon 的 TTM `io_mem_reserve` 把所有 VRAM BO 的 CPU 映射指向 BAR
   窗口，用户态（合成器、fbdev、X 的未加速拷贝）会直接用 SIMD 写进去。
   而本板该窗口只接受 ≤8 字节的单次写（10.4、10.5 已验证，厂商 5.10
   内核同样如此）。
2. **它是 legacy KMS 驱动。**
   radeon 使用 `drm_crtc_helper_funcs`，没有 atomic commit、没有
   modifiers。现代合成器（Treeland/wlroots、mutter、kwin）都以 atomic
   为前提。

厂商 5.10 的做法从侧面印证了第 1 点：它把 BAR0 整个留空不分配，从不给
CPU 直接访问 VRAM 的机会。

### 11.2 amdgpu 相对具备的能力

| 能力 | radeon | amdgpu |
|---|---|---|
| Atomic KMS / modifiers | 无 | 有 |
| visible VRAM < real VRAM 的原生支持 | 需要手工改 `visible_vram_size` | 原生（`adev->gmc.visible_vram_size`） |
| 把 CPU 可见对象放到非可见 VRAM 之外 | 无 | 由 VRAM 管理器分区管理 |
| 现代合成器兼容性 | 差 | 正常 |

### 11.3 迁移清单

硬件与窗口部分保持不变（第 2 节的 BAR0 重定位、56 MiB 窗口、iATU 配置
全部沿用）。需要做的只有：

1. **配置**：`CONFIG_DRM_AMDGPU=y`、`CONFIG_DRM_RADEON=n`；
   内嵌固件改为 `amdgpu/oland_*.bin`（原有 `radeon/oland_*` 可保留）。
2. **可见窗口**：在 `gmc_v6_0_mc_init()` 里对 `1002:6611` 覆盖
   `adev->gmc.aper_base = 0xea200000`、`aper_size = visible_vram_size
   = 0x3800000`（与 radeon 侧同样的原因：内核 resource 仍记录着未路由
   的 `0x1000000000`）。
3. **VRAM 的 CPU 映射属性**：**必须改**。`amdgpu_vram_mgr.c` 把 VRAM
   资源的 `bus.caching` 设成 `ttm_write_combined`，于是
   `ttm_bo_kmap()` 走 `ioremap_wc()`（Normal-NC），在本文板上必然
   SError。需要在 `amdgpu_ttm_io_mem_reserve()` 的 VRAM 分支里对
   `1002:6611` 强制 `ttm_uncached`（Device 映射）。

   这一条是本板迁移时最难发现的一处：`caching` 不是驱动显式设置的，
   而是 VRAM 管理器在资源初始化时写入的；实测打印为
   `res->bus.caching == ttm_write_combined`。
4. **系统内存一致性**：`amdgpu_ttm_tt_create()` 里把 `ttm_cached`
   改为 `ttm_write_combined`（GPU 非嗅探读，缓存映射会读到旧数据）。
5. **构建与验证**：先只替换驱动内核进 initramfs，确认
   `ring gfx/compute/dma1/dma2 test succeeded`，再进 rootfs 验证渲染。

### 11.4 amdgpu 迁移中的实测排除（2026-11-26）

迁移过程中在 `amdgpu` 上遇到 GART 表写入即 SError。逐项排除如下：

| 检查 | 结果 |
|---|---|
| 设备 BAR0 配置 | `bar0=0x9000000c/0x0`，重定位未被覆盖 |
| `PCI_COMMAND` | `0x6`，MEM 解码开 |
| `res->bus.offset` / `is_iomem` / `start` | `0xea200000` / `1` / `0`，正确 |
| `res->bus.caching` | 原为 `ttm_write_combined`（`amdgpu_vram_mgr.c` 设的），已强制 `ttm_uncached` |
| `aper_base_kaddr` 预映射 | 原为 `ioremap_wc()`，连续 BO 会优先用它；已在本板跳过 |
| `BIF_FB_EN` / `MC_SHARED_BLACKOUT_CNTL` | `0x3` / `0x0`，帧缓冲读写通路已开 |
| `HDP_NONSURFACE_BASE` 改为 0 | 无变化 |
| 4 KiB / 1 MiB / 2 MiB 对齐映射的 8 字节写+读 | 全部成功 |
| 连续 16 笔 8 字节写 + 读回 | 全部正确（`rd0`/`rd15` 均匹配） |
| 通过 GART 自己的 `ptr` 写 `+0x40000`，再用独立 ioremap 交叉读 | 一致 |

结论：**CPU 经 BAR 访问 VRAM 已完全验证可用**，包括连续写。剩余 SError
不是 CPU 写路径造成的。由于出问题的代码
（`amdgpu_gtt_mgr_recover` → `amdgpu_ttm_gart_bind`）会构造 IB 并提交
GPU 作业、由 SDMA 把 PTE 写入 VRAM，下一步应转向 GPU 侧 DMA（入向窗口 /
GART 地址）而不是 BAR。

### 11.4 amdgpu SError 的逐层二分（2026-11-26）

按“跳过 + 打点”的方式逐层缩小，结论如下（每一步都记录在串口日志里）：

| 二分点 | 结果 |
|---|---|
| 跳过 `gmc_v6_0_hw_init` 全部 | 仍 SError → 不在 GMC/GART 初始化 |
| 在 `amdgpu_device_init` 入口 / early-init 后打点 | 走到 `after-early-init r=0` 之后 |
| 在 `amdgpu_device_ip_init` 每个 sw_init/hw_init 前打点 | 停在 `IP sw i=3`（common=0、gmc=1、ih=2、**gfx=3**） |
| 在 `gfx_v6_0_sw_init` 内分段 | `ucode r=0` → `rlc begin` |
| 在 `amdgpu_gfx_rlc_init_sr` 内分段 | `create done`、`loop n=218 done`（全部成功） |
| 在 `gfx_v6_0_rlc_init` 的 clear-state 段打点 | `cs create begin dws=972` → `cs build begin` → **SError** |

最终定位：**`gfx_v6_0_get_csb_buffer()` 写入 RLC clear-state 缓冲时触发**。

这与 radeon 最初 `si_get_csb_buffer()` 崩溃是同一类代码；radeon 当时通过把
VRAM 映射改成 Device（`ttm_uncached`）解决。amdgpu 这边的映射已确认是
Device（`res->bus.caching=0`、每-BO `ioremap()`、无 `aper_base_kaddr`），
所以还需要检查这个 clear-state BO 的实际落点与大小是否都在 56 MiB 窗口内。

### 11.5 amdgpu SError 的最终根因与修复（2026-11-26）

逐层二分后定位到 `gfx_v6_0_get_csb_buffer()` → `amdgpu_gfx_csb_data_parser()`。
两者都是**逐 u32 写**的循环：

```c
buffer[count++] = cpu_to_le32(ext->extent[i]);
```

这段代码本身合法（每次 4 字节），但 **GCC 会把相邻的 32 位存储合并成
`stp` 成对存储（16 字节）**，即使内核启用了 `-mgeneral-regs-only`
（该选项只禁用 SIMD 寄存器，不阻止通用寄存器的 `stp` 合并）。本板到
PCIe1 的通路只接受 **≤8 字节的单次写**，于是这条 16 字节存储触发
异步 SError。

修复：把这些写改成 `volatile` 存储，禁止编译器合并。

```c
((volatile u32 *)buffer)[count++] = cpu_to_le32(ext->extent[i]);
```

改动位置：`amdgpu_gfx.c`（preamble start/end、data parser 共 12 处）、
`gfx_v6_0.c`（`gfx_v6_0_get_csb_buffer` 3 处）。

结果：**SError 完全消失**，RLC 初始化（`gfx rlc r=0`）与 ring 分配全部
通过，驱动继续走到 `ring gfx test failed (-110)`——即此前已知的 CP 不执行
问题，但不再导致整机 panic。

这条结论对后续所有“内核往 VRAM BO 写数据”的路径都适用：只要编译器可能
把多次小写合并成 >8 字节的存储，就需要同样的处理。

### 11.6 SError 修复后的状态（2026-11-26）

修复 CSB 写入后，amdgpu 可以完整走完驱动初始化，不再 panic，停在
`ring gfx test failed (-110)`。超时处的实测数据：

```
ring gpu=0xff00619000 size=0x4000 wptr_sw=0x200
CP_RB0_RPTR=0x1d0  CP_RB0_WPTR=0x200
ring[0x1d0..0x1f4] 全为 0x80000000（NOP 填充）
ring[0]=0xc0054400（PACKET3 SET_CONFIG_REG，测试包格式正确）
wb gpu=0xff00401000 rptr_a=0xff00401040 fence_a=0xff00401080（GTT）
VM fault0 status=0
```

即：CP 已经消费了测试包（rptr 推进过它），但 `SCRATCH_REG0` 没有被写成
期望值；无 VM fault、无 SError。ring 在 GTT 且已是非缓存映射，CPU 写入
对 CP 可见（rptr 能推进就是证据）。

下一步应对比 radeon 的 ring test 路径，确认 CP 侧写 scratch 未生效的原因
（寄存器偏移、CP 微码入口、或 ME/PFP 的启动状态）。

### 11.7 CP scratch 未写回的进一步观察（2026-11-26）

在 `gfx_v6_0_ring_test_ring()` 的测试包写入后立即 dump（而不是等超时）：

```
w0=0x100 (dword)  提交前 wptr
w1=0x200          提交后 wptr
CP_RB0_RPTR=0x1d0
```

注意 amdgpu 的 `ring->wptr` 与 `CP_RB0_RPTR/WPTR` 都**以 dword 计**，早先
按字节索引 dump 得到的“全是 NOP”是读错了位置，不作为证据。

`rptr(0x1d0) > w0(0x100)`，说明 CP 已读过测试包，但没有把
`SCRATCH_REG0` 写成 `0xDEADBEEF`。同时：

```
CP_ME_CNTL=0x0   CP_RB0_CNTL=0x90b   CP_RB0_BASE=0xff006190
```

与 radeon 通过时一致。因此剩余问题收敛为“CP 执行了 SET_CONFIG_REG 但没有
落到目标寄存器”，下一步应核对 SI 上 `PACKET3_SET_CONFIG_REG` 的偏移基准
（`mmSCRATCH_REG0` 与 `PACKET3_SET_CONFIG_REG_START` 的单位）以及 ME 的
初始化状态。

### 11.4 风险

- amdgpu 的 VRAM 管理器按 1 GiB 管理，只有 56 MiB 对 CPU 可见；扫描
  输出和 CPU 可见对象必须落在可见段内，需要确认 `amdgpu_bo_pin` 与
  显示代码的放置策略。
- 若 amdgpu 的某些路径仍然 CPU 映射 VRAM（例如固件上传、影子缓冲），
  同样会撞上宽度限制，需要逐个确认。
- 退路：保留现有 radeon Image 与源码备份，任何一步失败可以立即回滚。
