# scanout BO pin 失败：greeter、fbcon 与 ttm 生命周期

- 日期：2026-11-26
- 平台：Kirin 9000C（`charlotte`）+ Oland `[1002:6611]`（PCIe1）
- 软件栈：Linux 6.18 + amdgpu（SI / DCE 6.0），GNOME / Mutter
- 相关补丁：0011–0016（56 MiB 窗口、GTT 策略、vblank、旧 FB unpin）

## 结论

这里的 pin 问题分三条路径，不能混在一起：

1. **greeter EGL / shadow buffer**：GDM greeter 的 PAM 环境文件向
   greeter 注入了面向 SoC 显示栈的兼容库，导致 EGL 初始化失败
   （`No EGL display`）并回退 shadow buffer。注释该 `pam_env` 行后，
   greeter 以 Wayland 启动，greeter 的 `pin failed` 消失。
2. **启动期 4K fbcon（已定位/已规避）**：`#168` 的栈回溯直接显示
   4K fbcon 在 `drm_fb_helper_set_par` 中调用
   `dce_v6_0_crtc_do_set_base -> amdgpu_bo_pin`，`ttm_bo_validate`
   返回 `-ENOMEM`：
   `size=33,177,600`、`domain=VRAM`、`pin=0`、连续 placement。
   这不是“已 pin、连续标志不匹配”的 `-EINVAL` 分支，而是 4K fbcon
   BO 在 56 MiB 可见窗口内无法完成连续分配。
   在 6.18 测试启动行关闭 DRM fbdev
   （`drm_kms_helper.fbdev_emulation=0`）后，启动期 `pin failed`
   为 0，4K Wayland 约 28.9 FPS。代价是启动时没有 `fb0`/显示控制台，
   串口控制台仍可用。
3. **ttm 生命周期**：`ttm_bo_unpin` 的零计数旧 FB 下溢由补丁 0016
   修复；`ttm_bo_release` 在近期 Wayland、Xwayland 和独立 Xorg
   测试中未复现，保留为 X11/DDX 侧观察项，不能据此认为已修复。

## greeter EGL / PAM

- GDM greeter 的 PAM 环境文件注入了面向 SoC 显示栈的兼容库；
  该库与 AMD dGPU 的 EGL/GBM 冲突。
- 注释该 `pam_env` 行后，greeter 的 `gnome-shell` 输出：
  ```
  Obtained a high priority EGL context
  Using Wayland display name 'wayland-0'
  Registering session with GDM
  ```
- `No EGL display`、`gdm3` SIGTRAP 重启循环和 greeter 的
  `pin failed` 消失；普通用户 Wayland 会话不受影响。
- 详细复现和采样保留在本节后面的诊断记录中；私有环境文件的具体
  内容不进入公开文档。

## 启动期 4K fbcon（直接栈回溯）

`#168` 在 `amdgpu_bo_pin()` 的 `ttm_bo_validate()` 失败分支加入
`dump_stack()`，启动时得到：

```text
amdgpu ... pin failed: ret=-12 size=33177600 flags=0x229
    domain=0x4 pin=0 res_type=2 res_flags=0x0
    pl_type=2 pl_flags=0x1 comm=swapper/0

Call trace:
 amdgpu_bo_pin
 dce_v6_0_crtc_do_set_base
 dce_v6_0_crtc_mode_set
 drm_crtc_helper_set_mode
 drm_crtc_helper_set_config
 ...
 drm_fb_helper_set_par
 fbcon_init
 ...
 drm_fb_helper_initial_config
 drm_fbdev_client_hotplug
 amdgpu_pci_probe
```

- `ret=-12` 是 `-ENOMEM`，不是 `-EINVAL`。
- 失败 BO 是 4K 大小（33,177,600 byte），请求连续 VRAM。
- 调用者是 **fbcon**，不是 GDM/用户会话/合成器。
- 4K fbcon BO 需要连续可见 VRAM；56 MiB 窗口无法同时容纳该
  fbcon 缓冲和同类的可见 BO。用户会话的 scanout BO 使用
  `NO_CPU_ACCESS`，通常可放在非可见 VRAM，因此桌面仍能起来。

### 规避

在 6.18 测试启动行加入：

```text
drm_kms_helper.fbdev_emulation=0
```

验证结果：

- 启动日志 `pin failed` 计数为 **0**；
- `/sys/class/graphics/fb0` 不存在；启动时没有显示控制台，
  串口控制台仍可用；
- GNOME Wayland 仍是 4K，`es2gears_wayland` 约 **28.9 FPS**；
- `ttm_bo_unpin` / `ttm_bo_release` 计数为 0。

这是启动配置修复，不是 kernel pin 代码改动。若必须保留显示
控制台，可评估降低 fbcon 模式（例如
`video=DVI-I-1:1920x1080@60`），但本轮未验证，且需确认它不会
把合成器也限制到 1080p。

## 补丁 0016：旧 FB unpin 边界

legacy `dce_v6_0_crtc_do_set_base()` 可能继承一个不是该路径 pin
的旧 FB（例如 fbdev framebuffer），原代码无条件
`amdgpu_bo_unpin()` 会触发零计数下溢。补丁 0016 改为：

```c
if (abo->tbo.pin_count)
    amdgpu_bo_unpin(abo);
```

效果：启动时 `ttm_bo_unpin` WARNING 消失；4K↔1080p 切换仍正常
（28.8 / 56.1 / 29.6 FPS，IRQ 持续增长）。该保护只针对该 legacy
边界，不证明所有 pin 所有权都已核对。

## ttm_bo_release

- `#165` 曾在独立 Xorg 的 X11 客户端退出时出现一次
  `ttm_bo_release` 的 `pin_count` WARNING。
- `#166`/`#168` 的 Wayland、Xwayland `glxgears`、独立 Xorg
  `glxgears` 测试均未复现；当前没有可用现场，不能据此改
  `ttm_bo_release()` 或强制清零 pin。
- 保留为 X11/DDX 侧观察项。若再次出现，按下列维度记录：窗口化/
  全屏/异常退出、Xorg 整体退出、最后一次成功 pin、对应 unpin、
  未完成 flip/fence。

## 早期采样记录（修复前，供对照）

greeter 修复前的采样显示失败时可见窗口只用 4.7 MiB、VRAM 36.8 MiB，
曾据此排除了窗口耗尽；但 `pin failed` 日志来自
`amdgpu_bo_pin()` 的 `ttm_bo_validate()` 失败路径，采样中已 pin 的
4K BO 并不必然是该日志对应的失败 BO。后续 `#168` 的 fbcon 栈回溯
才是直接证据；早先“已 pin 连续标志不匹配导致 `-EINVAL`”的推断不再
作为结论。

## 后续

1. **完整 greeter 往返**：greeter → 登录 Wayland → 运行应用 →
   注销回 greeter → 再登录；确认硬件 EGL、输入输出和资源释放。
2. **Xwayland**：在 GNOME Wayland 下运行 X11 应用，验证
   radeonsi/Oland、窗口缩放、模式切换和退出；不以加载 amdgpu DDX
   为通过条件。
3. **IRQ 生命周期**：保留 0015 基线，补验输出关闭、DPMS、VT 切换
   和 compositor 退出。
4. **ttm_bo_release**：仅在再次复现时抓取；不预先修改 release 路径。
5. **启动控制台**：如需显示控制台，评估 1080p fbcon 或保留
   `drm_kms_helper.fbdev_emulation=0` + 串口控制台。

## 证据边界

- 启动期 `pin failed` 的结论来自 `#168` 的直接栈回溯和
  `ttm_bo_validate()` 返回值；不是采样关联推断。
- greeter 的 PAM 修复由成功日志验证；私有环境文件内容不进入
  公开文档。
- `ttm_bo_release` 在当前内核/测试中未复现；`#165` 的单次告警
  不能作为当前回归或修复依据。
- 56 MiB 可见窗口是硬件/平台约束；关闭 fbdev 只是避免 4K fbcon
  占用连续可见 VRAM，不改变窗口大小。

## 参考

- `drivers/gpu/drm/amd/amdgpu/dce_v6_0.c`：
  `dce_v6_0_crtc_do_set_base()`
- `drivers/gpu/drm/amd/amdgpu/amdgpu_object.c`：`amdgpu_bo_pin()`
- `drivers/gpu/drm/ttm/ttm_bo.c`：`ttm_bo_unpin()` /
  `ttm_bo_release()`
- `patches/0016-drm-amdgpu-only-unpin-the-old-FB-when-legacy-set_base-pinned.patch`
- 上游参考：
  [`mariobalanica/arm-pcie-gpu-patches`](https://github.com/mariobalanica/arm-pcie-gpu-patches)
- 显示与 Wayland 背景：[amdgpu_bringup_zh.md](amdgpu_bringup_zh.md)
