# GDM greeter scanout BO pin 失败：复现、根因与修复

- 日期：2026-11-26
- 平台：Kirin 9000C（`charlotte`）+ Oland `[1002:6611]`（PCIe1）
- 软件栈：Linux 6.18 + amdgpu（SI / DCE 6.0），GNOME / Mutter
- 相关补丁：0011–0015（56 MiB 窗口、GTT 策略、vblank/IRQ 修复）

## 结论

greeter 的 4K scanout BO pin 失败是**下游症状**，不是内核 pin
逻辑的独立 bug。完整链路：

1. GDM greeter 的 PAM 环境文件向 greeter 注入了面向 SoC 显示栈的
   兼容库；该兼容库与 AMD dGPU 的 EGL/GBM 冲突。
2. greeter 的 `gnome-shell` 因此 EGL 初始化失败（`No EGL display`），
   回退到 shadow buffer。
3. shadow BO 已经处于 pin 状态（`pin_count = 1`），但不是连续分配。
4. legacy `dce_v6_0_crtc_do_set_base()` 要求连续 VRAM，调用
   `amdgpu_bo_pin()`；已 pin 分支发现 `VRAM_CONTIGUOUS` 标志与
   resource 不匹配，返回 `-EINVAL`。
5. 日志打印 `amdgpu ... pin failed`，greeter 无法设置 scanout。

**修复（2026-11-26）**：注释 GDM greeter 的 `pam_env` 行，不再向
greeter 注入该 SoC 显示兼容库。greeter 的 `gnome-shell` 恢复：

```text
Obtained a high priority EGL context
Using Wayland display name 'wayland-0'
Registering session with GDM
```

`No EGL display`、`gdm3` SIGTRAP 循环和 greeter 的 `pin failed`
消失。用户会话不受影响，4K Wayland 正常。**因此不需要修改 kernel
的 pin 逻辑。**

非致命的 `pin failed` 仍可能在用户会话启动时出现（旧 FB 的
pin/unpin 计数问题），但不阻塞显示，属于单独的清理项。

## 修复结果（2026-11-26）

- 自动化验证：注释 GDM greeter 的 `pam_env` 行，关闭自动登录
  并重启 GDM；greeter 的 `gnome-shell` 输出
  `Obtained a high priority EGL context`、`Using Wayland display
  name 'wayland-0'`，并 `Registering session with GDM`。
- 恢复自动登录后用户会话仍为 Wayland；4K 下
  `es2gears_wayland` 约 27.7 FPS，DPMS On。

## 复现步骤

以下为修复前的诊断记录。

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

- 普通用户的 Mutter/GBM 路径不加载该 SoC 显示兼容库，直接使用
  AMD dGPU 的 EGL/GBM；scanout BO 使用 `NO_CPU_ACCESS`，按同一
  代码路径能通过 `amdgpu_bo_pin()` 的检查。
- greeter 在移除该注入前走 shadow buffer 路径，BO 已被预先 pin
  且非连续；legacy set_base 再要求连续 VRAM 时失败。

## 影响与规避

- **已修复**：GDM greeter 使用干净的 EGL/GBM 环境，Wayland greeter
  正常；自动登录不再是规避手段。
- 如果其他板或镜像也向 greeter 注入了面向 SoC 显示栈的兼容库，
  应只在 SoC 显示会话启用，不要加载到 AMD dGPU 的 greeter。
- 非致命的 `ttm_bo_unpin`/`ttm_bo_release` WARNING 仍属独立问题；
  不要把“已 pin 但非连续”的 BO 直接交给显示控制器。
- 该问题与补丁 0015 的 vblank/IRQ 修复无关；4K↔1080p 切换仍正常。

## 后续

1. **EGL 根因（已完成）**：已定位为 greeter 的板级 PAM 环境注入；
   去除后 greeter 恢复 Wayland/EGL。其他镜像应避免把 SoC 显示
   兼容库注入到 dGPU greeter。
2. **shadow buffer（不再需要）**：greeter 不再回退 shadow buffer；
   本节的 pin 分析保留为诊断证据。
3. **kernel 兼容（不需要）**：不需要修改 `dce_v6_0` 或
   `amdgpu_bo_pin()` 来接受非连续 scanout BO。
4. **非致命 pin/unpin 警告（待处理）**：单独清理
   `ttm_bo_unpin`/`ttm_bo_release` 和用户会话启动时的 `pin failed`。
5. **Xwayland 使用**：仍在主文档 TODO 中，后续验证 X11 应用。

## 证据边界

- 采样来自一次 6.18 测试启动；`pin failed` 打印的是内核 BO 指针，
  没有直接解析成 GEM handle。pin 失败的结论依据是同一时刻的 BO
  大小、`pin count`、`VRAM_CONTIGUOUS` 标志、可见窗口用量，以及
  `amdgpu_bo_pin()` 中唯一匹配的 `-EINVAL` 分支。
- PAM 环境修复已由 greeter 的成功日志（EGL context、Wayland
  display、Registering session）验证；私有环境文件的具体内容不
  进入公开文档。
- greeter 从未成功进入 shadow buffer scanout，因此“非连续 BO 直接
  扫描”的实际画面行为没有测试；修复后也不再需要测试。
- `[fbcon]` 的 4K framebuffer 仍存在；它在其他场景下仍可能占用
  可见窗口，是独立的容量问题。

## 参考

- `drivers/gpu/drm/amd/amdgpu/dce_v6_0.c`：
  `dce_v6_0_crtc_do_set_base()`
- `drivers/gpu/drm/amd/amdgpu/amdgpu_object.c`：`amdgpu_bo_pin()`
- `drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c`：用户态 GTT 策略
  （补丁 0012）
- 上游参考：
  [`mariobalanica/arm-pcie-gpu-patches`](https://github.com/mariobalanica/arm-pcie-gpu-patches)
- 显示与 Wayland 背景：[amdgpu_bringup_zh.md](amdgpu_bringup_zh.md) 第 8.10 节
