[English](../README.md) | 中文

# 构建验证记录

日期：2026-11-24
平台：QHW-100（Kirin 9000C，8c12t / 12 个逻辑 CPU）
编译器：aarch64 原生 `gcc (Debian 14.2.0-19) 14.2.0`

> **历史记录（2026-11-24）**：本文验证的是当时的 7 个补丁。之后补丁扩展到
> 0001–0014，6.18 defconfig 也增加了 `CONFIG_TMPFS_XATTR` /
> `CONFIG_TMPFS_POSIX_ACL`（logind 给 DRM 设备应用会话 ACL 所需）。
> 重新验证时以当前 `patches/` 和 `defconfig/` 为准；下面的版本号
> 只代表当次产物。

## 目的

验证本仓库自身的内容（`patches/` + `defconfig/` + `scripts/ci/build_kernel.sh`）
可以从**干净的 upstream v6.18** 构建出与当前板子功能相同的内核。

## 步骤

```sh
# 1. 干净的上游树（脚本自动完成）
git clone --depth 1 --branch v6.18 \
    https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git linux-6.18

# 2. 直接运行仓库脚本
KERN_SRC=$PWD/linux-6.18 OUT=$PWD/out JOBS=12 \
    bash scripts/ci/build_kernel.sh
```

脚本执行内容：

1. `git am patches/*.patch`（当时为 0001–0007，当前为 0001–0014）
2. 安装 `defconfig/charlotte_6.18_defconfig` 为 `arch/arm64/configs/charlotte_defconfig`
3. `make charlotte_defconfig && make -j12 Image dtbs modules`
4. 收集 `Image`、`charlotte-9000c.dtb`、`System.map-6.18`

## 结果

- 内核版本：`6.18.0-00007-g0f907a9a9106`
- 编译错误：0

### 配置一致性

把新构建的 `.config` 与板子上当前可用的 6.18 配置逐行对比：

```
$ diff <(sort current-6.18.config) <(sort new/.config)
(无输出)
```

即 **仓库 defconfig 能 1:1 复现当前工作内核的配置**。

### 关键符号

```
amdgpu_init
clt_pcie_probe
dw_pcie_host_init
uart_v500_early_setup
ufshcd_charlotte_core_doorbells
ufshcd_charlotte_core_rsr_enable
...
```

## 过程中修掉的问题

第一次用仓库构建时，补丁 0007 在真实 v6.18 上 `git am` 失败：

```
error: drivers/pci/controller/dwc/Kconfig: already exists in index
error: drivers/pci/controller/dwc/Makefile: already exists in index
```

原因：最早的 0007 是在“部分导入”的 bring-up 仓库里生成的，
`drivers/pci/controller/dwc/{Kconfig,Makefile}` 当时是未跟踪文件，
补丁把它们当成**新文件**整份添加。真实内核树里这两个文件已存在，因此冲突。

修复：在干净的 upstream v6.18 上重新生成了 0007（现在是正常的增量 diff，
Kconfig +10 行、Makefile +1 行、驱动 647 行、DTS 72 行），
整套补丁现在可以在 `v6.18` 上无冲突 `git am`。
