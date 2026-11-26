# amdgpu 迁移与 Oland 显示打通记录（2026-11-26）

> 当前状态（2026-11-26）：amdgpu 已完成初始化，GART 正常，DRM/fb0
> 工作，DVI-I-1 已 enabled。X11 GNOME 桌面可用（amdgpu DDX +
> glamor/radeonsi + DRI3，GL 4.6 / GLES 3.2）；Weston Wayland 也已
> 跑通（radeonsi，GLES 3.2）。用户态 VRAM→GTT 策略（补丁 0012）
> 消除了 EL0 SError；第 2 轮的 555 秒停顿未复现。
>
> 当前状态：GNOME Wayland 可用，4K@30 vsync 约 29.7 FPS，1080p@60
> 约 52–59 FPS；4K↔1080p 切换已由补丁 0015 修复，并在 Wayland/X11
> 下做过往返和压力测试。仍有 GDM greeter 的 EGL 回退、`ttm_bo_unpin`
> / `ttm_bo_release` WARNING 等非阻塞问题。详见 8.8–8.10。
>
> 本文第 8 节给出复核结论、实测记录和证据边界；radeon 仍是已验证的
> 可用回退。

本文记录把 PCIe1 上的 AMD Oland 显卡从 radeon 迁移到 amdgpu 的过程、
已完成的修复、实测结果和剩余问题。PCIe1 的窗口与 FF-A 放行见
[`pcie1_ffa_bar_zh.md`](pcie1_ffa_bar_zh.md)，本文不重复。

## 0. 结论摘要（本轮实测）

| 项目 | 结果 |
|---|---|
| 驱动加载 | **通过**，无 panic |
| GMC / GART 初始化 | **通过**（恢复完整 `gmc_v6_0_hw_init`，GART 使能） |
| GTT 恢复 | **通过**（恢复 `amdgpu_gtt_mgr_recover`） |
| RLC / CSB 构建 | 通过 |
| 各 IP block 的 sw_init / hw_init | 通过（0–7） |
| CP 微码加载 | 通过（pfp v84 / ce v61 / me v145） |
| gfx / compute ring 初始化 | `amdgpu_device_ip_init()` 返回 0；未逐 ring 记录测试结果 |
| 延迟 IB 测试（含 UVD） | 未单独验证；本轮日志未再出现对应 SError/超时 |
| DRM 注册 | `Initialized amdgpu 3.64.0 ... on minor 0` |
| fb0 | `amdgpudrmfb`，3840x2160@30，32bpp |
| DVI-I-1 | `connected`、`enabled`、`dpms=On`、模式 3840x2160@30 |
| 屏幕输出 | **已点亮**，可显示 initramfs/内核控制台 |
| X11 桌面 | **通过**：amdgpu DDX + glamor/radeonsi + DRI3，GNOME X11 桌面可用 |
| Weston Wayland | **通过**：DRM 输出 + radeonsi/GLES 3.2，鼠标键盘和终端正常 |
| GNOME Wayland | **可用**：4K@30 vsync 约 29.7 FPS，1080p@60 约 52–59 FPS；4K↔1080p 切换已修复（补丁 0015） |
| 硬件渲染 | **通过**：`glxinfo`/`eglinfo` 报告 `direct rendering: Yes`、`radeonsi/oland`、GL 4.6、GLES 3.2 |
| vsync 性能 | GNOME Wayland 4K@30 约 29.7 FPS、1080p@60 约 52–59 FPS；关 vsync 3139 FPS；Weston 1080p 33 FPS / 4K 17 FPS |
| 555 秒停顿 | 第 2 轮出现一次（554.9 秒）；第 3 轮未复现（`dri2`→`glamoregl` 仅 0.263 秒） |
| 切分辨率 | **已修复（补丁 0015）**：Wayland 4K↔1080p 往返和 X11 4K→1080p→4K 均正常；仍偶见 `ttm_bo_unpin`/`ttm_bo_release` WARNING（非致命） |
| LightDM greeter | `SWcursor` 已消除 pinned 光标 BO 循环；scanout BO 仍有 VRAM↔GTT 往返 |
| 整机稳定性 | GNOME Wayland / X11 / Weston 连续运行，无 SError、无 panic、无 GPU reset |

## 1. 关键修复

### 1.1 恢复 GMC/GTT 初始化：ring 超时的真正原因

板上源码曾有两处诊断旁路：

- `gmc_v6_0_hw_init()` 开头直接 `return 0`，MC 编程、微码加载和
  `gmc_v6_0_gart_enable()` 全部没有执行；
- `gmc_v6_0_gart_enable()` 中跳过 `amdgpu_gtt_mgr_recover()`。

此时 ring 位于 GTT，GPU 地址落在 GART 窗口内，但没有有效页表项，CP
无法取指，表现就是 gfx ring test 超时、RPTR 停在启动包中间。

恢复完整路径后：

- GART 使能成功，`PCIE GART of 1024M enabled`；
- gfx ring 测试包提交后，RPTR 从 `0x1c0` 推进到 `0x600`；
- 三个测试包分别对应 gfx ring 和两个 compute ring；
- `amdgpu_device_ip_init()` 返回 0。

这否定了此前“CP 卡在 clear-state 包/微码”的判断。clear-state 编程本身
没有改动，恢复的是它依赖的 GMC/GART 初始化。

同时移除了会写入在用 VRAM 的块读写探针，以及 `amdgpu_gmc_set_pte_pde()`
里的 PTE 回读探针。

### 1.2 ring 测试与 dump 修正

`gfx_v6_0_ring_test_ring()` 里原有的诊断块写到测试包后 `goto diag_done`，
把下面的正式提交代码变成了死代码；dump 又用
`(w0 + k * 4) / 4` 混用了字节偏移和 dword 索引。

修正后：

- 只提交一次测试包；
- 按 dword 索引读取：`ring->ring[(w0 + k) & ring->buf_mask]`；
- 成功路径的 `r` 初始化为 0，避免返回未初始化值；
- 超时日志增加“最后提交的 8 个 dword”，仍按 dword 索引。

测试包内容为 `SET_CONFIG_REG`，目标
`mmSCRATCH_REG0 - PACKET3_SET_CONFIG_REG_START = 0x140`，数据
`0xDEADBEEF`。`0xc0054400` 是 `ME_INITIALIZE`，不是
`SET_CONFIG_REG`（opcode `0x68`）。

### 1.3 VRAM 的 CPU 映射改为 Device

`amdgpu_vram_mgr.c` 默认把 VRAM 资源的 `bus.caching` 设为
`ttm_write_combined`，`ttm_bo_kmap()` 会走 `ioremap_wc()`。
本板对 Normal-NC 的宽访问不稳定，实验改为 Device 映射：

- 在 `amdgpu_ttm_io_mem_reserve()` 的 VRAM 分支，对本板
  （`1002:6611`）强制 `mem->bus.caching = ttm_uncached`；
- 跳过 `amdgpu_ttm_init()` 为整段 BAR 建立的 `aper_base_kaddr`
  写合并预映射，让每个 BO 各自走 `ioremap()`。

### 1.4 GTT（系统内存）采用 WC 策略

针对非一致性 PCIe DMA 路径，把 GTT 对象的 `ttm_cached` 改为
`ttm_write_combined`，避免 GPU 读取 CPU 缓存中的旧数据。具体对象的
映射属性、提交屏障与 GPU 可见性仍需按用途验证。

### 1.5 编译器宽访问：已获得直接证据

此前只知道 CSB 构建阶段会触发 SError，并把逐 u32 写改成
`volatile`。本轮拿到了反汇编证据，并发现另外三处同类问题。

**GART PTE 诊断读回（读方向）**

诊断代码用 `u64 *` 读 GART 表前两个 PTE，GCC 把两次相邻的 64 位读
合并成一条 16 字节 `ldp`：

```
ffff8000804eff24:  ldp x2, x3, [x19]   ← 16 字节读 VRAM
ffff8000804eff28:  bl  _dev_info
ffff8000804eff2c:  ...                 ← SError 在这里被接收
```

改成显式 `readq()`（8 字节）后不再触发。GART 表位于 VRAM，条目 0/1
未使用，读回值本身没有意义，后续不再 dump 它们。

**reset magic（读方向）**

`amdgpu_device_fill_reset_magic()` 用 `memcpy()` 从 `adev->gart.ptr`
（VRAM）拷 64 字节，生成 `ldp` 成对读；`amdgpu_device_check_vram_lost()`
又用 `memcmp()` 直接比较 VRAM。两处都改为 `memcpy_fromio()`，再在
普通内存上比较：

```c
memcpy_fromio(adev->reset_magic, adev->gart.ptr, AMDGPU_RESET_MAGIC_NUM);
...
u8 magic[AMDGPU_RESET_MAGIC_NUM];
memcpy_fromio(magic, adev->gart.ptr, sizeof(magic));
if (memcmp(magic, adev->reset_magic, sizeof(magic)))
	return true;
```

**UVD 消息缓冲（写方向）**

UVD 3.x 的非 64 位寻址路径会把 `uvd.ib_bo` 移到 VRAM。原代码用
`for (i = ...; i < 1024; ++i) msg[i] = 0;` 清零，GCC 将其转换成
`memset()`，对 VRAM 映射执行宽写。改为：

```c
memset_io(msg, 0, 1024 * sizeof(*msg));
writel(0x00000de4, (void __iomem *)&msg[0]);
/* ... 其余字段同样用 writel() */
```

CSB 构建路径继续保留 `volatile u32` 存储，避免逐 u32 写被合并。

**结论边界**

这些证据说明：在本板当前的内核路径上，用 8 字节 I/O accessor 替代
可能被编译器合并成 16 字节的事务，至少能让这几个点不再触发 SError。
它们**不能证明**：

- 平台普遍禁止所有大于 8 字节的访问；
- `volatile` 能改变硬件事务宽度（它只约束编译器，不约束 CPU/互连的
  合并与重排）；
- Xorg 的 EL0 SError 就是某条用户态宽读造成的。

另外，6.18 arm64 的 `memcpy_fromio()`/`memset_io()` 在
`lib/iomem_copy.c` 中确实实现为 8 字节 `__raw_readq/__raw_writeq`
循环（当前 arm64 没有架构覆盖），所以本文这几处改法与 8 字节假设
自洽；但这是实现细节，不是“所有 I/O 拷贝 API 都保证 8 字节”的
接口契约。GART 表在部分 ASIC 上位于系统内存，`memcpy_fromio()` 不能
无条件用于 `adev->gart.ptr`，需要按表所在位置分支处理。

## 2. 显示输出打通

### 2.1 初始现象

amdgpu 注册 fb0 并切换 fbcon，但屏幕不亮：

```
DP-1      disconnected
DVI-I-1   connected  mode=1680x1050  enabled=disabled
fb0       1680x1050, 32bpp
Console:  switching to colour frame buffer device 210x65
```

EDID 和首选模式都正确，问题是连接器/CRTC 没有被真正使能。

### 2.2 根因：SI 缺少 `flush_gpu_tlb_pasid` 回调

`amdgpu_vm_tlb_fence_work()` 无条件调用
`amdgpu_gmc_flush_gpu_tlb_pasid()`。该函数在非 KIQ 路径直接调用
`adev->gmc.gmc_funcs->flush_gpu_tlb_pasid()`；而 `gmc_v6_0_gmc_funcs`
没有实现这个回调，于是调用地址为 0：

```
Workqueue: events amdgpu_tlb_fence_work
pc : 0x0
lr : amdgpu_gmc_flush_gpu_tlb_pasid+0xcc/0x354
Call trace:
 0x0 (P)
 amdgpu_tlb_fence_work
```

TLB fence 无法完成，fbdev 的 `fb_set_par` 返回 `-EBUSY`，输出保持
关闭。

SI 没有 PASID/ATC 映射，但通用 VM 代码仍会发 PASID flush。修复是给
`gmc_v6_0` 增加一个全 VMID 失效实现：

```c
static void gmc_v6_0_flush_gpu_tlb_pasid(struct amdgpu_device *adev,
					 uint16_t pasid, uint32_t flush_type,
					 bool all_hub, uint32_t inst)
{
	WREG32(mmVM_INVALIDATE_REQUEST, 0xffff);
}
```

并注册到 `gmc_v6_0_gmc_funcs`。修复后实测：

```
status=connected enabled=enabled dpms=On mode=1680x1050
无 Internal error / fb_set_par 错误 / TLB flush 失败
```

屏幕已能显示 initramfs/内核控制台。

### 2.3 BAR 失败与显示无关

启动日志中的：

```
pci 0000:00:00.0: BAR 0 [mem size 0x01000000]: failed to assign
pci 0000:01:00.0: BAR 4 [io  size 0x0100]: failed to assign
```

分别是根端口自身的 16 MB BAR 和 GPU 的 256 字节 I/O BAR，不是 GPU
显存 BAR0。显存 BAR0 已重定位到总线 `0x90000000`，寄存器窗口在
`0xea100000`；amdgpu 能读取 VBIOS、初始化 GART、跑 ring 并注册 fb0。
radeon 在相同 BAR 失败下可正常显示，因此这些失败不是黑屏原因。

## 3. 历史问题：Xorg 启动时的 SError（已由 GTT 策略解决）

> **状态（2026-11-26 第 3 轮）：已解决。** 用户态 VRAM→GTT 策略
> （补丁 0012）让 Xorg 不再直接映射 VRAM，EL0 SError 不再出现，
> X11 GNOME 桌面已正常工作。本节保留为历史分析。

2026-11-26 之前的一次 6.18 启动记录到：

- `lightdm` 为 `active`；
- Xorg 启动后出现 EL0 SError：

```
SError Interrupt ... -- SError
CPU: 5 UID: 0 PID: 729 Comm: Xorg
pstate: ... -PAN ... 
pc : 0000ffffad001ba0
...
__el0_error_handler_common
el0t_64_error_handler
```

这是**旧一轮**的 PC；本轮两轮的实测（visible 检查、GTT 策略）见
第 8.4 节，不能把不同镜像的 PC/寄存器混在一起解释。

**证据边界**：`pc` 是 CPU 取到异常时的位置，不是必然的故障指令；
`Comm: Xorg` 只说明当时的 CPU 上下文。异步 SError 可能来自更早的
CPU 访问、已提交的 posted write，或 GPU/DCE 的 DMA。没有
`/proc/<pid>/maps`、库偏移和 PCIe AER 记录，不能证明这是 Xorg 的
某条 SIMD 宽读。

“Xorg 曾成功启动并完成输入设备初始化”来自更早的 radeon/5.10 日志，
不能作为 6.18 的证据。板上 Xorg 没有显式指定 amdgpu DDX，5.10 日志
显示的是 **modesetting DDX**（`Matched modesetting as autoconfigured
driver 0`、`modeset(0)`），且 glamor 因只有 llvmpipe 而被拒绝；6.18
那次的 Xorg 日志已被 5.10 启动覆盖。因此“改走 modesetting DDX”不是
一个未尝试的新方案，必须先确认 6.18 那次到底走了 glamor 还是
ShadowFB。

候选原因至少包括：

- BO 位于 56 MiB CPU 可见窗口之外（见第 8 节）；
- Device 与 Normal-NC 映射属性的差异；
- BO 迁移后残留的旧 PTE；
- GPU/DCE 对非法地址的 DMA，而不是 CPU 访问；
- runtime PM / PCIe 链路状态；
- GTT WC 的提交屏障或 ring/IB 同步不完整；
- 真正的 16 字节 CPU 事务限制。

### 3.1 按优先级排列的修复步骤

1. **先补 visible 检查/日志（已做，结果见 8.4）**：在
   `amdgpu_ttm_io_mem_reserve()` 的 VRAM 分支加 radeon 同款检查，
   并在 `amdgpu_ttm_io_mem_pfn()` 和 fault 路径打印 BO 的
   start/size/visible。重跑 Xorg，先排除“窗口外访问”。
2. **做受控宽度测试（待做）**：同一块 visible VRAM，分别用 Device 和
   WC 映射，用 4/8/16/32 字节对齐读写，记录 ESR/FAR/AER，确认到底是
   “宽度”“映射属性”还是“地址窗口”。
3. **抓 Xorg 的映射（待做）**：关闭 ASLR，后台采样 `/proc/$pid/maps`，
   开 coredump，定位 PC 属于哪个库、哪个 BO；同时看 PCIe AER，
   区分 CPU 访问和 GPU DMA。
4. **如果确认是宽度（部分提前实施）**：内核侧做统一的 8 字节 iomem
   helper，审计所有 `__iomem` 访问；用户态不要 mmap VRAM，走 GTT
   shadow + GPU copy。注意非 DC 的 SI 路径
   `amdgpu_display_supported_domains()` 只返回 VRAM，scanout 仍需
   VRAM，只能 GPU copy 进去。用户态 mmap/fault → GTT 的策略已在
   第 2 轮提前加入，但完整 staging 尚未实现。
5. **顺手修 SI PASID flush（待做）**：`gmc_v6_0_flush_gpu_tlb_pasid()`
   现在只写 `VM_INVALIDATE_REQUEST`，没有读 `VM_INVALIDATE_RESPONSE`；
   SI 有 `0x147c`，建议加回读/轮询。GTT WC 的 barrier 和 ring/IB
   同步也要审计，这两者也可能造成“看起来像宽访问”的 SError。

不再尝试用捕获 SError 后重试拆分指令的方式修复；异步异常无法可靠
定位故障指令，也不能保证事务未部分完成。

## 4. 构建与配置要点

- 配置：`CONFIG_DRM_AMDGPU=y`、`CONFIG_DRM_AMDGPU_SI=y`、
  `CONFIG_DRM_RADEON=n`；
- 内嵌固件：`amdgpu/oland_{ce,k_smc,mc,me,pfp,rlc,smc,uvd}.bin`
  （`CONFIG_EXTRA_FIRMWARE`；内建驱动在挂载 rootfs 前 probe，不能依赖
  `/lib/firmware`）；
- 应用第 1、2 节和第 8.4 节的修改；
- 内核命令行加 `amdgpu.rebar=0`，避免它修改已重定位的 BAR0；
- 每轮确认修改文件确实重新编译，并保留可回退的基线镜像。

## 5. 与上游补丁集的关系

[`mariobalanica/arm-pcie-gpu-patches`](https://github.com/mariobalanica/arm-pcie-gpu-patches)
是本次 Device 映射、对齐处理及系统内存 WC 策略的重要参考。
同步对齐异常处理不能直接用于异步 SError 恢复；本轮的宽访问观察
来自本板特定调用点，不能据此断言参考补丁集的适用范围。

## 6. 桌面与输入验证

- X11 GNOME 和 Weston Wayland 均可用；X11 使用 amdgpu DDX +
  glamor/radeonsi + DRI3，Weston 使用 DRM + radeonsi。
- `CONFIG_INPUT_EVDEV=y`；USB 鼠标/复合键盘生成 `/dev/input/event*`，
  Xorg 和 Weston 都能识别。
- 显示管理器和会话选择属于部署配置，不在此记录；关键结论是：
  硬件光标/scanout BO 的 CPU 映射会影响 greeter，软件光标可绕开
  pinned 光标 BO 的缺页循环。

## 7. 涉及文件

| 路径 | 改动 |
|---|---|
| `drivers/gpu/drm/amd/amdgpu/gmc_v6_0.c` | 恢复完整 `hw_init`；恢复 `gtt_mgr_recover`；移除 VRAM 探针；覆盖 `aper_base`/`visible_vram_size`；新增 SI `flush_gpu_tlb_pasid`（全 VMID 失效） |
| `drivers/gpu/drm/amd/amdgpu/gfx_v6_0.c` | CSB 写入改 `volatile`；ring 测试单次提交；dump 改 dword 索引；`r` 初始化 |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c` | CSB 写入改 `volatile` |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_device.c` | reset magic 改用 `memcpy_fromio` |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_uvd.c` | UVD 消息缓冲改用 `memset_io`/`writel` |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c` | VRAM 资源 caching → `ttm_uncached` |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c` | 跳过 `aper_base_kaddr`；GTT → `ttm_write_combined`；VRAM visible 检查（越界返回 `-EINVAL`）；fault/IO 映射日志 |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_object.c` | 非 visible VRAM BO 的 fault 路径日志；原有迁移逻辑保留 |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c` | 用户态 mmap/fault 强制 VRAM BO → GTT；pinned VRAM BO 拒绝映射 |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c` | ring 提交增加 DMA 同步（本板非一致性路径） |
| `drivers/pci/controller/dwc/pcie-designware-host.c` | BAR0 重定位（与 radeon 共用） |
| `arch/arm64/boot/dts/hisilicon/charlotte.dtsi` | 1 MiB + 56 MiB 窗口（与 radeon 共用） |

## 8. 2026-11-26 复核：56 MiB 可见窗口与证据边界

### 8.1 已确认的代码差异

上游 `radeon_ttm_io_mem_reserve()` 在 VRAM 分支先检查：

```c
mem->bus.offset = mem->start << PAGE_SHIFT;
if ((mem->bus.offset + bus_size) > rdev->mc.visible_vram_size)
	return -EINVAL;
mem->bus.offset += rdev->mc.aper_base;
mem->bus.is_iomem = true;
mem->bus.caching = ttm_write_combined;
```

amdgpu 的 `amdgpu_ttm_io_mem_reserve()` 没有这个 visible 检查，只
依赖缺页时 `amdgpu_bo_fault_reserve_notify()` 把非 visible 的 BO
搬到 visible VRAM 或 GTT。`ttm_bo_kmap()`、已经建立的 PTE、pinned BO
等路径不一定经过这个缺页通知；BO 在 56 MiB 之外时，CPU PTE 可能
指向窗口外，访问就会 SError，**与 16 字节宽度无关**。

radeon 在同一板子上能显示，且它使用 `ttm_write_combined`
（Normal-NC）并显式拒绝非 visible VRAM。这个对照至少说明：不能把
Normal-NC 或宽访问直接判定为硬件绝对禁区；amdgpu 与 radeon 在
visible 检查、映射属性和显示路径上的差异更值得先查。

### 8.2 判定标准

按第 3.1 节的顺序执行，用下面的结果决定下一步：

| 观察 | 结论 |
|---|---|
| 加 visible 检查后 Xorg SError 消失 | 根因是窗口外访问/BO 放置，不是宽度 |
| 受控测试中 8 字节正常、16 字节在 Device/WC 下都失败 | 宽度限制成立 |
| 4/8 字节在某种映射下也失败 | 映射属性、地址解码或链路问题 |
| 只在 GPU/显示活动时失败 | 优先查 GPU/DCE DMA、GTT 一致性 |
| 同一地址在 radeon 下正常、amdgpu 下失败 | 优先查 amdgpu 的地址转换/映射/显示编程 |

### 8.3 执行状态

- [x] 第 1 项：visible 检查与 fault 日志（已测，单独不够）
- [ ] 第 2 项：受控宽度测试（桌面已通过 GTT 策略可用，宽度限制未确认为
  根因；保留为硬件特性调查，不是当前阻塞项）
- [x] 第 3 项：Xorg 映射/PC/AER 定位（Xorg 日志和线程栈已拿到；
  第 3 轮未复现停顿）
- [x] 第 4 项：用户态 mmap/fault → GTT 已实现并验证（X11 GNOME 桌面）；
  8 字节 helper 和完整 staging 未做，桌面路径不需要
- [x] 第 5 项：SI PASID flush 回调已补（补丁 0013）；GTT/ring 屏障审计
  未单独做

### 8.4 2026-11-26 实测：visible 检查与用户态 GTT 策略

> 编号说明：本节按实际实施顺序记录；用户态 GTT 策略对应第 3.1 节
> 第 4 项的前半，**不是**第 2 项受控宽度测试。

**第 1 项（visible 检查）结果：不够。**

第 1 轮测试中，Xorg 先命中一个非 visible VRAM BO：

```
CPU fault (Xorg): VRAM bo start=0x3ee00000 size=0x200000 visible=0x3800000 pinned=0
CPU fault on non-visible VRAM BO (Xorg): start=0x3ee00000 size=0x200000 visible=0x3800000 pinned=0
```

随后同一个 2 MiB BO 出现在 visible 的 `0x1000000`，紧接着仍然发生
EL0 SError（`pc=0xffff9a21392c`），并触发内核 panic：

```
CPU fault (Xorg): VRAM bo start=0x1000000 size=0x200000 visible=0x3800000 pinned=0
SError Interrupt on CPU10 ... -- SError
Kernel panic - not syncing: Asynchronous SError Interrupt
```

结论：56 MiB 窗口检查确实拦到了非 visible BO，但迁移到 visible
区域后仍观察到 SError，说明单靠这项检查不够。**尚不能排除窗口外
访问**：SError 是异步异常，可能对应更早的访问、迁移前的旧 PTE
或其他 BO；日志中的 BO 偏移也不能证明实际 CPU PFN 正确。
用户态避免直接映射 VRAM 是下一步的隔离方案，尚不是根因结论。

**用户态 GTT 策略（第 4 项前半）结果：SError 消失，但出现 555 秒停顿。**

第 2 轮加入了：

- `amdgpu_gem_object_mmap()`：VRAM BO 在用户态 `mmap` 时迁移到 GTT；
  pinned VRAM BO 返回 `-EPERM`；
- `amdgpu_gem_fault()`：VRAM BO 在缺页时迁移到 GTT；pinned 时
  `SIGBUS`/`NOPAGE`；
- 内核 fbdev 的 `ttm_bo_kmap()` 路径不变，仍走 8 字节 I/O。

启动后观察到：

```
lightdm: active
Xorg:    /usr/lib/xorg/Xorg ... :0
DVI-I-1: connected, enabled, dpms=On
```

该轮 `/var/log/Xorg.0.log.old`（OS `6.18.0-00007-g57305409a010-dirty #163`）
保留了下来，提供了比上一版文档更明确的证据：

```
[43.532] (==) Matched amdgpu as autoconfigured driver 0
[43.533] (II) LoadModule: "amdgpu"
...
[598.461] (II) AMDGPU(0): glamor X acceleration enabled on AMD Radeon R7 200
           Series (radeonsi, oland, ACO, DRM 3.64, 6.18.0-...-dirty)
[598.797] (II) AMDGPU(0): Output DVI-I-0 using initial mode 1680x1050 +0+0
[598.878] (==) AMDGPU(0): DRI3 enabled
[598.916] (II) AMDGPU(0): Use GLAMOR acceleration.
[599.142] (II) XINPUT: Adding extended input device ...
```

也就是说：

- 这次 Xorg **不是** modesetting DDX，而是 amdgpu DDX；glamor 使用
  radeonsi/EGL，DRI3 已启用；
- 没有再次出现 EL0 SError，也没有 panic；
- 但 Xorg 日志时间从 `[43.539] Loading sub module "dri2"` 直接跳到
  `[598.407] Loading sub module "glamoregl"`，中间约 **554.9 秒**
  没有任何 Xorg 日志。之后它继续完成初始化，日志最后到 `[600.765]`。

因此现在可以确认：**用户态 VRAM→GTT 策略让 Xorg 走过了之前触发
SError 的阶段**，但启动路径上仍有一个长时间停顿。这个停顿发生在
amdgpu DDX 的 `PreInit` 阶段（dri2 之后、glamor 之前），可能对应
内核里的 GPU 等待、DDX 的 BIOS/DRM 查询或显示资源探测；现有 Xorg
日志无法区分。

该轮内核 dmesg 没有持久化，无法确认停顿期间内核在做什么。

### 8.5 诊断环境与工具踩坑

- **journald 持久化**：`Storage=persistent` 必须写在 `[Journal]` 段内，
  否则 journald 按 `auto` 运行，6.18 日志不会落盘。
- **`amdgpu_vram` 不是摘要**：`/sys/kernel/debug/dri/0/amdgpu_vram` 是
  1 GiB 的显存原始转储。诊断脚本按文本读取它，30 分钟就把日志写到
  4.7 GB，单个采样点耗时数分钟。读取 debugfs 前必须确认文件大小，
  大文件只取头部或跳过。

### 8.7 2026-11-26 第 3 轮实测：X11 GNOME 桌面可用，555 秒停顿未复现

本轮 6.18 实测内核：`6.18.0-00007-g57305409a010-dirty`（第 3 轮）。

**Xorg 本轮没有出现 555 秒停顿。** 同一位置的日志为：

```
[53.024] (II) Loading sub module "dri2"
[53.287] (II) Loading sub module "glamoregl"
[53.807] (II) AIGLX: Loaded and initialized radeonsi
[53.808] (II) Initializing extension DRI2
```

`dri2` → `glamoregl` 仅间隔 **0.263 秒**，而第 2 轮 `Xorg.0.log.old`
在同一位置是 **554.9 秒**。因此该停顿是间歇性的，不是每轮必现。

**桌面与硬件渲染验证：**

```
$ xrandr --current
Screen 0: minimum 320 x 200, current 1680 x 1050, maximum 16384 x 16384
DVI-I-0 connected primary 1680x1050+0+0

$ glxinfo -B
direct rendering: Yes
Vendor: AMD (0x1002)
Device: AMD Radeon R7 200 Series (radeonsi, oland, ACO, DRM 3.64, ...) (0x6611)
Accelerated: yes
Max core profile version: 4.6
Max GLES[23] profile version: 3.2
```

即 X11 GNOME 桌面在 amdgpu DDX + glamor/radeonsi + DRI3 下正常工作，
硬件加速已启用。

**自动诊断服务的采样结果：**

- 第 1 个采样点（12:03:09，Xorg 启动早期）：Xorg 主线程位于
  `folio_wait_bit_common` → `filemap_fault` → `do_page_fault`，
  即等待磁盘上的模块/文件页，属于正常加载，不是 GPU 等待。
- 第 5 个采样点（12:32:23，桌面空闲）：Xorg 主线程位于
  `do_epoll_wait`，其余线程位于 `__futex_wait`，均为正常空闲。
- 没有观察到 `amdgpu_*` 等待、fence 等待或 ring 超时。

**诊断脚本的缺陷与修正：**

`/sys/kernel/debug/dri/0/amdgpu_vram` 在 SI 上是 **1 GiB 的显存原始
转储**，不是显存管理器摘要。原脚本每个采样点都 `cat` 它一次，导致：

- `/var/log/amdgpu-diag.log` 在 30 分钟内涨到 **4.7 GB**；
- 每个采样点耗时约 7 分钟，48 个点无法在合理时间内完成。

读取 debugfs 前必须确认文件大小；大文件只取头部或跳过，不要整份读入
诊断日志。

**对 555 秒停顿的当前判断：** 第 3 轮未复现，且采样点里没有 GPU
等待证据；现有材料不足以把它归因于 amdgpu。它更可能是一次性的
首次加载/磁盘 I/O 或某个尚未定位的 DDX 路径。保留为观察项，不再
作为桌面可用的阻塞问题；若后续再次出现，用修正后的诊断服务抓取。

**固化到补丁：** 工作树中的功能改动已整理为 `patches/0011` 到
`patches/0014`：

- 0011：报告 56 MiB 重定位 aperture，并强制 GTT 使用 WC
  （`gmc_v6_0.c`、`amdgpu_ttm.c`）；
- 0012：用户态 VRAM BO 在 mmap/fault 时迁移到 GTT，并保留
  visible 窗口检查（`amdgpu_gem.c`、`amdgpu_ttm.c`）；
- 0013：补上 SI 的 PASID TLB flush 回调（`gmc_v6_0.c`）；
- 0014：reset magic 和 UVD 消息缓冲区改用 I/O 访问器
  （`amdgpu_device.c`、`amdgpu_uvd.c`）。

仅用于定位的逐次 fault 日志、GART 调试读回和 ring dump 没有进入
补丁。补丁已在 `v6.18 + 0010` 基线上用 `git am` 顺序验证通过。

### 8.8 2026-11-26 Wayland 准备与 X11 卡死定位

**现象 1：LightDM greeter 卡死，键鼠无响应**

串口日志显示 greeter 的 Xorg 反复缺页在一个 **pinned VRAM BO**
（VRAM 偏移 `0x20c000`，大小 `0x4000`，即 DCE 光标缓冲区）：

```
mmap/fault: refusing pinned VRAM BO (Xorg): start=0x20c000 size=0x4000
...
amdgpu_gem_bo_move_to_gtt_locked: 8203168 callbacks suppressed
```

本地源码 `amdgpu_gem.c` 和补丁 0012 的实际处理是：

```c
if (r == -EBUSY || r == -ERESTARTSYS)
        ret = VM_FAULT_NOPAGE;
else
        ret = VM_FAULT_SIGBUS;
```

pinned VRAM BO 返回 `-EBUSY` 后，这个分支没有建立 PTE，也没有等待
能解除 pin 的事件，就返回 `NOPAGE`，足以解释同一指令反复缺页和
数百万次日志被抑制。此前“SIGBUS 后 Xorg 主动重试”的描述不符合
本地实现；仍需核对板上测试镜像与该代码是否完全一致。

内核仍在运行；USB 键鼠在日志中正常枚举。

**现象 2：X11 切换分辨率卡死**

同一轮日志在 greeter 之前还有一次 Xorg 的 `ttm_bo_unpin` 告警：

```
WARNING: ... at drivers/gpu/drm/ttm/ttm_bo.c:648 ttm_bo_unpin
Call trace:
 ttm_bo_unpin
 amdgpu_bo_unpin
 dce_v6_0_crtc_do_set_base
 dce_v6_0_crtc_set_base
 drm_crtc_helper_set_config
 amdgpu_display_crtc_set_config
 drm_mode_setcrtc
```

告警说明发生了不配对的 unpin，但尚不能证明由 GTT 迁移造成，也
不能仅凭该栈确定整个卡死的原因。当前 helper 已拒绝迁移 pinned BO，
应独立追踪显示路径的引用与 pin 生命周期。**尚未修复。**

### 8.9 代码复核后的修复重点与执行顺序

第 3 轮已证明 radeonsi/glamor 硬件桌面可以工作。当前重点从“让 GPU
跑起来”转向**补齐 GTT 策略与显示 BO 生命周期的边界**。完整 staging
降为备用方案；以下均为待实施、待验证的修复计划。

**1. 消除 pinned fault 的无进展循环，再验证软件光标**

- 区分在当前策略下不允许映射的 pinned VRAM 与可重试的暂态错误。
  前者应明确失败，不能在未建立 PTE、也没有进展条件时返回
  `VM_FAULT_NOPAGE`；暂态错误按 fault 的锁及重试约定处理。
- 不采用 dummy page 作为修复。它会让写入看似成功，实际光标内容
  没有进入 VRAM，掩盖映射接口错误。
- 给 Xorg amdgpu 配置 `Option "SWcursor" "true"`，让软件光标参与
  已有绘制/合成路径，避开硬件光标 BO 的 CPU 更新。

明确失败只解决内核侧无进展循环，可能仍导致用户态报错或退出；
软件光标才是当前让用户态正常工作的绕行方案，两者需分别验证。

**2. 独立定位模式切换的 pin/unpin 配对**

在 `dce_v6_0_crtc_do_set_base()` 及对应 pin/unpin 路径记录：

- 新旧 framebuffer 的 BO 标识，以及两者是否相同；
- 每次 pin/unpin 前后的计数、内存域和操作返回值；
- 新 BO pin 失败后，是否仍执行旧 BO 释放；
- 重复设置同一模式、切换模式、关闭 CRTC 分别执行的分支。

重点查 pin 失败仍清理、重复 unpin、新旧 BO 相同却处理不配对。
不要简单用 `if (pin_count)` 跳过告警，这会隐藏生命周期错误。

**3. 补齐映射与显示用途之间的生命周期约束**

“mmap/fault 时搬到 GTT”不保证 BO 后续一直可被 CPU 安全访问。
需要验证是否出现以下过程，而非把它当作已确认事实：

```text
用户建立映射 → BO 在 GTT
显示路径将它迁入 VRAM 并 pin
用户再次访问 → fault 无法迁回 GTT
```

“只迁移非 pinned BO”已经是当前行为，不构成新增修复。应追踪
同一个 BO 从创建、映射、迁移到显示使用及释放的全过程，同时核对
迁移时旧 PTE 的撤销。

`amdgpu_gem_object_mmap()` 当前在 reservation 锁外检查 `resource`
和 `pin_count`。决定迁移或拒绝的状态检查应统一放到锁内，避免
检查与使用之间状态改变；仅加锁仍不能替代后续用途变化的约束。

**4. 下一轮验证顺序**

1. 核对板上源码、补丁和镜像，修正 pinned fault 的无进展返回，
   开启 Xorg 软件光标。
2. 验证 greeter、登录桌面、退出回 greeter，确认没有缺页风暴。
3. 定位并修复模式切换的 pin/unpin 配对，覆盖重复设模、切换模式
   及关闭输出，保留完整日志。
4. 从独立会话启动 Weston，固定分辨率、优先软件光标，确认 GL
   renderer 实际使用 Oland。Xorg 的 `SWcursor` 配置不适用于
   Weston，须按安装版本核对其软件光标设置。
5. 再测 Wayland 分辨率切换、退出后的控制台恢复及 GNOME Wayland。

Weston 不必依赖 LightDM greeter 启动，避免 Xorg greeter 故障阻挡
Wayland 验证。优先 GL 路径：当前硬件加速已有成功证据，而 Pixman
可能再次遇到 CPU 映射输出缓冲的限制。尚无证据表明切换到 Wayland
本身能修复内核 pin/unpin 错误。

扩大窗口、受控宽访问实验和完整 staging 暂时后排。555 秒停顿保留
为观察项，仅在再次出现时抓取，不再以“整机失联”作为当前主线。

**执行更新（2026-11-26 晚）：**

- 第 1 项已完成：Xorg `Option "SWcursor" "true"` 已验证，pinned 光标
  BO 的缺页循环消失；未采用 dummy page。
- 第 4 项已完成：Weston 独立会话验证通过，GL renderer 为 radeonsi；
  GNOME Wayland 也已进入桌面（ACL 已修，补丁 0015 修复切分辨率）。
- 已完成：重建 6.18 内核启用 `CONFIG_TMPFS_POSIX_ACL` /
  `CONFIG_TMPFS_XATTR`，GDM Wayland 会话已验证。
- 第 2、3 项（pin/unpin 配对、映射生命周期）仍待处理。

### 8.10 2026-11-26 Wayland、GDM 与性能实测

**Weston（已通过）**

- 在 6.18 上以 root 启动 Weston 14.0.2，DRM 输出 DVI-I-1，鼠标/键盘
  正常，`weston-terminal` 可用。
- `eglinfo -p gbm`：`AMD Radeon R7 200 Series (radeonsi, oland, ...)`，
  EGL 1.5，GLES 3.2。
- 1080p@60：vsync `es2gears` 33 FPS / `weston-simple-egl` 33 FPS；
  `vblank_mode=0` 时 148 FPS / 144 FPS。
- 4K@30：vsync 17 FPS；`vblank_mode=0` 全屏 115 FPS。
- 说明 GPU 渲染路径可用；vsync 下只有刷新率的一半，瓶颈在每帧
  提交（legacy 非 atomic DRM、双缓冲等 vblank），不是 GPU 算力。

**GPU 频率**

- 空闲时 `amdgpu_pm_info` 可能停在 power level 0（30 MHz）；
  `power_dpm_force_performance_level=high` 可到 825/1000 MHz。
- 强制 high 后 vsync FPS 不变（仍受呈现路径限制），但 no-vsync
  吞吐明显提高。DPM auto 的升频策略仍需观察。

**GDM / GNOME Wayland（可用，但会话选择有坑）**

- 6.18 缺 `CONFIG_TMPFS_POSIX_ACL` / `CONFIG_TMPFS_XATTR` 会导致
  logind 报 `Failed to apply ACLs`，已通过 defconfig 打开。
- 即使 ACL 修好，GDM 仍可能按 AccountsService/DefaultSession 或
  `xsessions` 里的同名 `gnome.desktop` 启动 X11；强制只保留 Wayland
  的 `gnome.desktop`、设置 `DefaultSession=gnome` 后，会话类型为
  `Type=wayland`。
- GNOME Wayland 下 `eglinfo` 显示 radeonsi/GLES 3.2；4K@30 时
  vsync `es2gears` 约 29.7 FPS（对应 30 Hz），60 秒稳定；
  1080p@60 时约 52–59 FPS；`vblank_mode=0` 时达到 **3139 FPS**，
  GPU 渲染无问题。
- GDM greeter 仍可能打印 `No EGL display` 并回退 X11，但 autologin
  的 Wayland 用户会话不受影响；这是 greeter 的独立问题。

**切分辨率：vblank/IRQ 停摆与修复**

- 通过 Mutter `ApplyMonitorsConfig` 从 3840x2160@30 切到 1920x1080@60，
  模式切换本身成功。
- 之后 `amdgpu` IRQ 计数 2 秒内 **152957 → 152957**，完全不递增；
  `es2gears_wayland` / `weston-simple-egl` 等 frame callback 不返回，
  客户端阻塞；切回 4K 也不能恢复；无 SError/panic，GPU fence 均
  signaled。
- 重启 GDM 后 IRQ 恢复递增（160391 → 163105/2s），说明是 DCE
  modeset 后 vblank/pageflip 中断掩码丢失，和 X11 切分辨率卡死同源。
- 修复（补丁 0015）：在 `dce_v6_0_crtc_commit()` 的 DPMS_ON 之后显式
  恢复该 CRTC 的 vblank 和 pageflip 中断掩码。modeset 期间 amdgpu IRQ
  引用计数可能为 0，`dce_v6_0_crtc_dpms()` 里的 `amdgpu_irq_update()`
  会把掩码重新关掉，导致 modeset 后的第一个 flip 永远等不到中断。
- 验证（6.18 #165，Image SHA256 `436585609ba5a3667ba1c1234fcdf1d707f4ca6a482f72e74c2b556c929e22a1`）：
  - GNOME Wayland：4K@30 29.7 FPS；切到 1080p@60 后 52–58 FPS；
    4K→1080p→4K→1080p→4K 往返正常；在 `es2gears_wayland` 持续
    渲染时连续切 6 次，FPS 随刷新率变化，amdgpu IRQ 从 134100 涨到
    207512，无 SError/panic/GPU reset/ring timeout。
  - X11（独立 Xorg，amdgpu DDX + SWcursor）：4K 30.3 FPS、1080p
    59.5 FPS、切回 4K 30.2 FPS，IRQ 随渲染增长。

**桌面状态**

- LightDM greeter 的 pinned 光标 BO 循环已由 Xorg `SWcursor` 消除；
  Xorg scanout BO 仍有 VRAM↔GTT 往返。
- 启动时 `dce_v6_0_crtc_do_set_base()` 仍会触发一次 `ttm_bo_unpin`
  WARNING；X11 客户端退出时偶见 `ttm_bo_release` WARNING。两者都
  不阻塞显示，和 vblank 中断停摆是独立问题。
- GNOME Wayland 可用；4K↔1080p 切换已验证。GDM 自动登录后
  GNOME 会话初始化（UPower/colord/IBus 等 D-Bus 超时）可能需要
  1–2 分钟，期间太早启动 Wayland 客户端可能等不到 frame callback；
  等会话稳定后再测。

## 9. 后续方案：小窗口下的显存访问与 GTT 中转

本节为后续优化设计与验证计划。**当前桌面已经可用**（用户态 BO 走
GTT，scanout/fbdev 留在 VRAM），因此本节不是阻塞项；只有需要在
CPU 侧直接读写 VRAM，或做完整的 GTT staging/scanout 中转时才需要
继续。第 3.1 节保留原始计划，当前修复顺序以第 8.9 节为准。

### 9.1 56 MiB 限制的是 CPU 可见范围

讨论中的“约 59 MB 窗口”即 56 MiB（58.7 MB，`0x3800000`）。
假设窗口映射 VRAM 的起始段，CPU 直接访问一个 BO 的前提是其实际
映射范围完全处于该窗口内。例如 `0x3ee00000` 处的 2 MiB BO
在窗口外，`0x01000000` 处的 2 MiB BO 在窗口内。

这不意味着 GPU 只能使用 56 MiB 显存。GPU 访问自身 VRAM 不直接
受该 CPU 窗口大小限制；窗口外的 BO 可以继续供 GPU 使用，CPU
需要访问时再迁移或中转。小窗口会增加迁移压力，但正确处理后，
不应仅因窗口小就崩溃。

还须区分 BO 的显存偏移与 CPU 侧 BAR 物理地址：前者可见，不代表
后者的地址转换正确。应联合核对实际 PFN、BAR、PCIe 地址转换窗口
及 `visible_vram_size`，并检查迁移后的旧 PTE 是否撤销。

### 9.2 可选路径与优先方案

| 路线 | 做法 | 限制及状态 |
|---|---|---|
| 迁入 visible VRAM | CPU 访问前把 BO 搬入前 56 MiB | 空间有限，pinned BO 不一定能搬；且本轮 visible BO 附近仍观察到异常 |
| **GTT 中转** | CPU 访问系统内存，GPU 在 GTT 与 VRAM 间复制 | 备用方案；需要复制、DMA 一致性及 fence 同步 |
| 扩大硬件窗口 | 协调 PCIe 地址转换、固件放行、BAR 和驱动可见范围 | 平台能力待调查，不能只调大 `visible_vram_size` |
| 动态切换窗口 | 让 CPU 窗口轮流覆盖不同显存段 | 硬件支持未确认；必须协调所有存量映射及内核访问，当前不采用 |

更换 mmap 虚拟地址无法绕过硬件未开放的地址窗口。当前建议绕开
“CPU 直接访问高处 VRAM”的需求，而不是尝试访问未映射的物理空间。

### 9.3 GTT staging 与 scanout 的具体做法

```text
写入：CPU → GTT staging → GPU copy → VRAM
读回：VRAM → GPU copy → GTT staging → 等待完成及 CPU 可见性处理 → CPU

软件绘制显示：CPU 绘制 GTT shadow
                        ↓ GPU copy，完成后才允许显示使用
                    VRAM scanout → 显示控制器
```

SI 非 DC 路径的 scanout 仍保留在 VRAM，用户态映射独立的 GTT
shadow。需确定复制由哪个内核/用户态路径发起，衔接绘制完成、copy
fence 和显示更新，避免 CPU 改写 GPU 正在读取的 staging 或显示
正在扫描的目标缓冲。读回同样须等待 GPU 完成并确保 CPU 可见。

现有 mmap/fault 强制 BO → GTT 的策略没有建立这套独立缓冲与复制
流程，但第 3 轮已验证现有加速桌面可用，无需先实现通用 staging。
仍需核对其边界：

- BO reservation、迁移完成等待、旧用户 PTE 撤销和 fault 重试；
- 映射后是否可能再次被迁回 VRAM；
- Xorg 是否映射已 pin 的 scanout BO，以及拒绝映射后的错误处理；
- 非一致性 DMA 下 CPU/GPU 数据可见性，不能仅以 GTT 使用 WC
  映射或 ring 已加同步作为整个提交链正确的依据。

### 9.4 后续调查项（非当前阻塞）

当前执行顺序见第 8.9 节。补丁 0011–0014 已整理并验证可顺序应用，
旧版“本地仅有两份源码、先复现整机失联”的计划已被后续记录取代。

- **TLB 与 DMA 完成语义：** PASID flush 回调已补；response 完成
  条件和 GTT/ring 屏障仍需单独核实。若增加等待，应先确认寄存器
  语义，再使用有界等待及超时日志；DMA 同步范围包括 ring、IB、
  页表和 writeback/fence，而不仅是 ring。
- **VRAM 宽度实验：** 使用专门分配、固定且完全位于 visible 窗口
  内的测试 BO，分别改变映射属性、读写方向和访问宽度，核对机器
  指令及可获得的异常/PCIe 状态。避免对同一物理页同时建立不同
  属性的映射，也避免触碰在用 GART/scanout。
- **完整 GTT staging：** 仅在现有加速路径和软件光标无法满足需求、
  确需 CPU 更新 VRAM 输出内容时，继续设计第 9.3 节的中转路径。
- **扩大窗口：** 作为独立的平台能力调查，不代替 fault 返回值和
  BO 生命周期修复。
