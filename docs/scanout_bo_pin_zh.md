# GDM greeter scanout BO pin 失败：复现、采样与根因

- 日期：2026-11-26
- 平台：Kirin 9000C（`charlotte`）+ Oland `[1002:6611]`（PCIe1）
- 软件栈：Linux 6.18 + amdgpu（SI / DCE 6.0），GNOME / Mutter
- 相关补丁：0011–0015（56 MiB 窗口、GTT 策略、vblank/IRQ 修复）

## 结论

greeter 的 4K scanout BO 在 legacy DCE set_base 路径中 pin 失败，
直接原因**不是 56 MiB 可见 VRAM 窗口耗尽**，而是“BO 已经 pin 在
VRAM、但底层 resource 不是连续分配”。按日志和采样推断的链路：

1. greeter 的 `gnome-shell` 先因 EGL 初始化失败（`No EGL display`）
   回退到 shadow buffer；
2. 该 shadow BO 已经处于 pin 状态（`pin_count = 1`），但不是连续
   分配；从 `No EGL display` / `prefers shadow buffer` 日志看，
   它来自 Mutter 的 shadow buffer 路径；
3. legacy `dce_v6_0_crtc_do_set_base()` 设置
   `AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS` 后调用
   `amdgpu_bo_pin(AMDGPU_GEM_DOMAIN_VRAM)`；
4. `amdgpu_bo_pin()` 的“已 pin”分支发现 BO 带
   `VRAM_CONTIGUOUS` 标志，但 TTM resource 没有
   `TTM_PL_FLAG_CONTIGUOUS`，返回 `-EINVAL`；
5. 日志打印 `amdgpu ... pin failed`，greeter 无法设置 scanout，
   随后退出。

同一内核下普通用户的 Wayland 会话正常：用户会话的 scanout BO 是
连续 pin 的 `NO_CPU_ACCESS` BO，不走 shadow buffer 路径。

GDM 的崩溃循环是另一个配置问题：所有 X11 session 文件被隐藏后，
greeter 失败时没有 X11 回退，`gdm3` 以 SIGTRAP 退出并触发 systemd
重启循环。它不是 pin 失败的根因，但会放大故障表现。

## 复现步骤

1. 保留 Wayland 的 `gnome.desktop`，临时关闭 GDM 自动登录；
2. 重启 `gdm.service`；
3. greeter 会在 tty1 反复启动，日志出现 EGL 与 pin 失败；
4. 在 greeter 循环期间以 root 每 0.2 秒采样以下节点：

```sh
head -5 /sys/kernel/debug/dri/0/amdgpu_vram_mm
grep -E '33423360|42369024|pin count' \
    /sys/kernel/debug/dri/0/amdgpu_gem_info
grep -B4 -A2 'size=3840x2160' \
    /sys/kernel/debug/dri/0/framebuffer
dmesg | tail -4
```

`amdgpu_vram_mm` 给出 VRAM 总量、总用量和可见窗口用量；
`amdgpu_gem_info` 给出每个 BO 的 mem_type、pin count 和
`VRAM_CONTIGUOUS` 等标志；`framebuffer` 给出 KMS framebuffer 的
尺寸、分配者和 BO 大小。

## 采样结果

greeter 失败瞬间：

```text
usage: 38539264          # 36.8 MiB / 1 GiB
vis usage: 4976640       # 4.7 MiB / 56 MiB

0x00000010: 33423360 byte VRAM pin count 1 NO_CPU_ACCESS \
            VRAM_CLEARED VRAM_CONTIGUOUS
0x00000011: 42369024 byte VRAM NO_CPU_ACCESS VRAM_CLEARED
```

`framebuffer` 同时列出：

```text
framebuffer[...]:
    allocated by = gnome-shell
    size=3840x2160
    obj[0]:
        size=33423360

framebuffer[...]:
    allocated by = [fbcon]
    size=3840x2160
```

关键点：

- 失败前后采样：可见窗口都只用了 **4.7 MiB**；总 VRAM 从
  177 MiB（旧会话尚未完全退出）降到 **36.8 MiB**。两者都远低于
  56 MiB / 1 GiB 的上限，所以不是窗口或 VRAM 被占满。
- greeter 的 4K BO 是 33,423,360 byte（4K，含对齐），已经
  `pin count 1`，并带 `VRAM_CONTIGUOUS` 标志。
- `fbcon` 的 4K framebuffer 仍然存在，但失败时它没有把可见窗口
  占满；可见窗口瓶颈假设可以排除。
- 内核在同一时刻重复打印：

```text
amdgpu 0000:01:00.0: amdgpu: 000000007bf88392 pin failed
```

## 代码路径

`dce_v6_0_crtc_do_set_base()` 的非 atomic 路径：

```c
if (!atomic) {
    abo->flags |= AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS;
    r = amdgpu_bo_pin(abo, AMDGPU_GEM_DOMAIN_VRAM);
    if (unlikely(r != 0)) {
        amdgpu_bo_unreserve(abo);
        return -EINVAL;
    }
}
```

`amdgpu_bo_pin()` 的已 pin 分支：

```c
if (bo->tbo.pin_count) {
    uint32_t mem_type = bo->tbo.resource->mem_type;
    uint32_t mem_flags = bo->tbo.resource->placement;

    if (!(domain & amdgpu_mem_type_to_domain(mem_type)))
        return -EINVAL;

    if ((mem_type == TTM_PL_VRAM) &&
        (bo->flags & AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS) &&
        !(mem_flags & TTM_PL_FLAG_CONTIGUOUS))
        return -EINVAL;

    ttm_bo_pin(&bo->tbo);
    return 0;
}
```

采样到的 BO 是 VRAM、`pin_count = 1`，请求域也是 VRAM，所以
第一个判断通过；唯一匹配的 `-EINVAL` 分支就是“已 pin、带
`VRAM_CONTIGUOUS` 标志、但 resource 没有连续标志”。

## 为什么用户会话正常

- 普通用户的 Mutter/GBM 路径分配 scanout BO 时使用
  `NO_CPU_ACCESS`，按同一代码路径能通过 `amdgpu_bo_pin()` 的检查，
  说明它的 resource 在 pin 时带连续标志（或还没有被预先 pin）。
- greeter 在 EGL 失败后选择 shadow buffer，该 BO 的分配/pin
  路径不同：先被预先 pin，且不是连续分配。legacy set_base 再
  要求连续 VRAM 时，已 pin 的 BO 无法在不移动/不破坏现有 pin
  的情况下重新满足条件。

## 影响与规避

- **自动登录**：greeter 不参与，Wayland 用户会话正常，当前即用此
  方式运行。
- **X11 greeter 回退（建议，未在本轮验证）**：恢复一个 X11 session
  文件，让 greeter 失败时有 X11 回退；预期可避免 `gdm3` 的 SIGTRAP
  重启循环。
- **不要**把“已 pin 但非连续”的 BO 直接交给显示控制器；显示扫描
  非连续 VRAM 可能错位或撕裂。任何 kernel 侧兼容都必须实际验证
  画面。
- 该问题与补丁 0015 的 vblank/IRQ 修复无关；4K↔1080p 切换仍正常。

## 后续选项

1. **查 EGL 根因（优先）**：同一 dGPU 上用户会话 EGL 正常、greeter
   的 `gnome-shell` 失败。对比 greeter 用户与普通用户的
   `eglinfo`、`XDG_RUNTIME_DIR`、DRM ACL、Mesa 环境和启动时序，
   确认是启动竞态、权限还是 Mesa 上下文问题。
2. **修 shadow buffer 路径**：如果 greeter 必须使用 shadow buffer，
   需要在分配时就按连续 VRAM pin，或在 Mutter/GBM 层避免把
   已 pin 的非连续 BO 交给 legacy set_base。
3. **kernel 兼容（不优先）**：在 `dce_v6_0_crtc_do_set_base()` 中
   对已 pin 的 BO 不再强行加 `VRAM_CONTIGUOUS`，或让
   `amdgpu_bo_pin()` 接受该组合。必须验证显示无错位/撕裂。
4. **补充确认**：如需把 `pin failed` 的 BO 指针直接对应到 GEM
   handle，可在 `amdgpu_bo_pin()` 的失败分支临时打印 `mem_type`、
   `mem_flags`、`flags` 和 `pin_count`；这会增加一次内核重建。

## 证据边界

- 采样来自一次 6.18 测试启动；`pin failed` 打印的是内核 BO 指针，
  没有直接解析成 GEM handle。结论依据是同一时刻的 BO 大小、
  `pin count`、`VRAM_CONTIGUOUS` 标志、可见窗口用量，以及
  `amdgpu_bo_pin()` 中唯一匹配的 `-EINVAL` 分支。因此
  “shadow buffer 预先 pin”是从日志和 `pin_count` 推断，不是直接
  抓到的调用栈。
- greeter 从未成功进入 scanout，因此“非连续 BO 直接扫描”的实际
  画面行为尚未测试。
- 采样期间 `[fbcon]` 的 4K framebuffer 仍然存在；它在其他场景下
  仍可能占用可见窗口，是独立的容量问题。

## 参考

- `drivers/gpu/drm/amd/amdgpu/dce_v6_0.c`：
  `dce_v6_0_crtc_do_set_base()`
- `drivers/gpu/drm/amd/amdgpu/amdgpu_object.c`：`amdgpu_bo_pin()`
- `drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c`：用户态 GTT 策略
  （补丁 0012）
- 上游参考：
  [`mariobalanica/arm-pcie-gpu-patches`](https://github.com/mariobalanica/arm-pcie-gpu-patches)
- 显示与 Wayland 背景：[amdgpu_bringup_zh.md](amdgpu_bringup_zh.md) 第 8.10 节
