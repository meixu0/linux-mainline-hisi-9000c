# 6.18 补丁固化 + sdd2 rootfs chroot

本文记录两件事：

1. 把 6.18 bring-up 的全部主线改动整理成 **git format-patch 补丁集**（`git am` 可重放），
   并把调试打印降为 `dev_dbg`。
2. 让 6.18 能挂载板载 rootfs（`/dev/sdd2`）并 **chroot** 进去。

---

## 1. 调试打印降级

| 文件 | 改动 |
|---|---|
| `drivers/ufs/host/ufs-charlotte.c` | 22 处 `dev_info()` → `dev_dbg()`（SoC init / MPHY / 链路 / QoS / IRQ 全部 bring-up 日志） |
| `drivers/ufs/core/ufshcd.c` | 4 处 charlotte 诊断日志 → `dev_dbg()` |

保留 `dev_err()` / `dev_warn()`。默认 `loglevel=7` 下不再刷屏，
需要时可以用 dynamic debug 打开：

```sh
echo 'file ufs-charlotte.c +p' > /sys/kernel/debug/dynamic_debug/control
```

（前提是内核编译了 `CONFIG_DYNAMIC_DEBUG`。）

---

## 2. 补丁集（gaokun-buildbot 风格）

目录：

- 板端：`~/9000c/kernel-port/patches/`（含 `README.md`）
- 本机存档：`~/9000c/patches/`

格式：`git format-patch` 生成的标准邮件补丁，可直接 `git am`。

| 序号 | Subject | 内容 |
|---|---|---|
| 0001 | arm64: dts: hisilicon: add QHW-100 (charlotte) board support | `charlotte.dtsi`、`charlotte-9000c.dts`、dts `Makefile` |
| 0002 | tty: serial: add uart_v500 serial console driver | `uart-v500.c`、`uart-v500-earlycon.c`、serial `Kconfig/Makefile` |
| 0003 | misc: add dss-power-probe diagnostic driver | `dss-power-probe.c`、misc `Kconfig/Makefile` |
| 0004 | ufs: core: add HUF S per-core queue support for charlotte | `include/ufs/ufshci.h`、`include/ufs/ufshcd.h`、`drivers/ufs/core/ufshcd.c` |
| 0005 | ufs: host: add ufshcd-charlotte variant driver | `drivers/ufs/host/ufs-charlotte.c`、host `Kconfig/Makefile` |
| 0006 | ufs: charlotte: negotiate HS-G4 through the private UniPro window | `include/ufs/ufshcd.h`、`drivers/ufs/core/ufshcd.c`、`drivers/ufs/host/ufs-charlotte.c` |

### 在干净的主线 v6.18 上重放

```sh
git clone --depth 1 -b v6.18 \
    https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
cd linux
git am /path/to/patches/*.patch
```

已实测：在全新 v6.18 上 `git am` 全部补丁成功，得到与 bring-up 树一致的提交。

### 板端 bring-up 树的 git 状态

```
265c2ee ufs: host: add ufshcd-charlotte variant driver
7f7e2fc ufs: core: add HUF S per-core queue support for charlotte
e72739c misc: add dss-power-probe diagnostic driver
4864af3 tty: serial: add uart_v500 serial console driver
e225b3f arm64: dts: hisilicon: add QHW-100 (charlotte) board support
8908f91 v6.18 baseline          <-- tag v6.18（未改动的基线）
```

基线是用 `raw.githubusercontent.com/torvalds/linux/v6.18` 的原始文件重建的，
所以每个提交只包含对应改动，没有夹杂主线内容。

> 注意：因为树里现在有 `.git`，内核版本字符串变成了
> `6.18.0-00005-g265c2ee21042`（`scripts/setlocalversion` 自动追加）。
> 不想要这个后缀可以用 `LOCALVERSION=`/`scripts/config --set-str LOCALVERSION ""`
> 后重新编译。

### 需要的配置项（不属于补丁）

```
CONFIG_ARCH_HISI=y
CONFIG_SERIAL_V500=y
CONFIG_SERIAL_V500_EARLYCON=y
CONFIG_SCSI_UFSHCD=y
CONFIG_SCSI_UFSHCD_PLATFORM=y
CONFIG_SCSI_UFS_CHARLOTTE=y
CONFIG_BLK_DEV_SD=y          # 不开就只有 SCSI 设备，没有 /dev/sd*
CONFIG_EXT4_FS=y             # 挂载 sdd2 用
```

---

## 3. 让 6.18 挂载 sdd2 并 chroot

### 前提

- 6.18 已经能枚举 UFS（`sda`..`sdd`），见 `ufs_bringup_zh.md`。
- `CONFIG_EXT4_FS=y`：6.18 内核自带 ext4，可以直接挂载 `sdd2`。
- `sdd2` 就是板子上安装的系统盘（GXDE OS 25 / Debian 系，ext4）。

### 用法

6.18 的 initramfs 里新增了 `/bin/sdd2-chroot`（安装脚本在
`~/9000c/kernel-port/diag/root-console/bin/sdd2-chroot`）：

```sh
# 只读挂载（默认，安全：ro,noload，不重放 journal、不写盘）
sdd2-chroot

# 读写挂载（会重放 journal，慎用）
sdd2-chroot rw

# 进 rootfs 只跑一条命令
sdd2-chroot ro uname -a
sdd2-chroot ro cat /etc/os-release
```

脚本做的事：

```
mount -t ext4 -o ro,noload /dev/sdd2 /mnt/sdd2
mount --bind /dev /mnt/sdd2/dev
mount -t proc  proc  /mnt/sdd2/proc
mount -t sysfs sysfs /mnt/sdd2/sys
chroot /mnt/sdd2 /bin/bash -l --noediting
```

退出 chroot（`exit`）会回到 initramfs shell，挂载保留在 `/mnt/sdd2`。

### 实测

```
~ # sdd2-chroot
--- /dev/sdd2 mounted on /mnt/sdd2 (ro,noload)
PRETTY_NAME="GXDE OS 25"
--- entering chroot, type 'exit' to return
root@(none):/# uname -r
6.18.0-00005-g265c2ee21042
root@(none):/# head -2 /etc/os-release
PRETTY_NAME="GXDE OS 25"
root@(none):/# ls /
bin boot dev etc home ...
root@(none):/# exit
--- left chroot (mounts kept under /mnt/sdd2)
```

### 三个坑（都已处理）

1. **交互 shell 必须用 `--noediting`**
   bash 的 Readline 用 `select()` 等串口输入，而这个 bring-up 版 UART 驱动的
   poll 唤醒还不完整，于是 `bash`（带 Readline）看起来像卡死，必须发一个
   串口 BREAK 才醒。
   用 `bash --noediting`（纯 `read(2)` 路径，和 busybox sh 一样）就正常。

2. **串口关一次会把 chroot 里的 shell 带走（已修）**
   主机侧关闭 `/dev/ttyUSB0`（例如 `Ctrl-A Ctrl-X` 退出 picocom、或 kill 掉读串口的
   进程）时，USB-UART 可能给板子注入 BREAK/挂断，内核会把 `SIGINT`/`SIGHUP`
   发给 tty 前台进程组，chroot 里的 shell 因此退出，之后串口只剩回显、命令不执行。

   修法：`sdd2-chroot` 在 `exec chroot` 之前

   ```sh
   trap '' HUP INT QUIT TTIN TTOU PIPE
   ```

   被忽略的信号处置会跨 `exec` 继承，所以 chroot 里的 shell 不会再被主机侧
   关串口弄死。

   实测：进入 chroot 后 kill 掉主机侧读串口的进程（等于关闭端口），重新打开
   串口再敲 `echo CH2; uname -r; pwd` → 正常执行且 `pwd` 仍是 `/`。

3. **initramfs 的 shell 退出不再 panic（已修）**
   以前 `init` 最后是 `exec sh -i`，shell 就是 **PID 1**，`exit` 会触发
   `Kernel panic - not syncing: Attempted to kill init!`。

   现在 `init` 改成 respawn 循环（PID 1 是 init 脚本本身）：

   ```sh
   trap '' TTIN TTOU
   while :; do
       $BB sh -i </dev/ttyHW0 >/dev/ttyHW0 2>&1
       echo "[init] shell exited, respawning in 1s (PID 1 stays alive)"
       $BB sleep 1
   done
   ```

   实测：在 initramfs 提示符下 `exit` 两次，都能看到
   `[init] shell exited, respawning in 1s (PID 1 stays alive)`
   并拿到新 shell，命令照常执行。

   仍然建议重启用 `reboot -f`；chroot 里的 `exit` 只是回到 initramfs。

### 安全说明

- 默认 **只读 + noload**：不重放 journal，不写回，5.10 的 rootfs 不会被改动。
- 只有显式 `sdd2-chroot rw` 才会读写；那之前请确保 5.10 是正常关机
  （用 `systemctl reboot`，不要 `reboot -f`），减少 journal 重放。
- 5.10 仍然是 GRUB 默认项（`GRUB_DEFAULT=0`），6.18 依旧只能手动
  `grub-reboot` 进入。

---

## 4. 当前产物

| 产物 | 位置 | md5 |
|---|---|---|
| 6.18 内核（PCIe0/SATA 可用，当前默认测试镜像） | `/boot/linux-6.18-charlotte/Image` | `8247f40db86c917609d81daac7efa5ef` |
| 仓库构建验证镜像 | `/boot/linux-6.18-charlotte/Image.repotest` | `4690ec04b1a04b2183411104d17014ee` |
| 验证前镜像备份 | `/boot/linux-6.18-charlotte/Image.before-repotest` | `8247f40db86c917609d81daac7efa5ef` |
| 早期 UFS 可用镜像 | `/boot/linux-6.18-charlotte/Image.ufsworking` | `aeab4f60a8ad6ba50d49a6ebba77558b` |
| 6.18 initramfs（含 sdd2-chroot、respawn 循环） | `/boot/linux-6.18-charlotte/initramfs-console.cpio.gz` | `7ab07e420283f07a9cfb2e596f780d07` |
| 回滚镜像 | `/boot/linux-6.18-charlotte/Image.safe` | 未改动 |
| 补丁集（权威） | `linux-mainline-hisi-9000c/patches/` | — |
