[English](../README.md) | 中文

# 6.18 网络 bring-up：USB RTL8153（r8152）

日期：2026-11-24
平台：QHW-100（Kirin 9000C，8c12t）

> **现状（2026-11-26）**：USB 网卡在 5.10 和 6.18 下都能工作；6.18
> 需要打开 `CONFIG_USB_RTL8152`。6.18 rootfs 由 systemd-networkd 通过
> DHCP 自动接管（见第 4 节）；5.10 默认启动项网络不变。GDM/ACL
> 问题已修复，见 amdgpu 文档。

## 1. 6.18 的问题

USB 控制器本身在 6.18 已经是好的（`dwc3` + `dwc3-of-simple` + `xhci-hcd`，
`0xefc00000`，两个 root hub 都能枚举）。真正的问题在**内核配置**：

```
$ grep CONFIG_NET .config
# CONFIG_NET is not set
```

也就是说 6.18 的 bring-up 配置里**整个网络子系统都没有编译**，自然也没有
USB 网卡驱动。早期 initramfs 里 `ip` 命令存在，但内核没有 netdev。

## 2. 修改

### 2.1 内核配置

在 `defconfig/charlotte_6.18_defconfig` 里打开：

```
CONFIG_NET=y
CONFIG_PACKET=y
CONFIG_UNIX=y
CONFIG_INET=y
CONFIG_IPV6=y
CONFIG_NETDEVICES=y
CONFIG_USB_NET_DRIVERS=y
CONFIG_USB_RTL8152=y
```

`USB_RTL8152` 会自动 select `MII`、`PHYLIB`、`CRC32`、`CRYPTO_LIB_SHA256`。

这里用的是**主线 r8152 驱动**（`drivers/net/usb/r8152.c`，加载后报
`v1.12.13`），没有移植 5.10 的 Realtek 外置驱动，也不需要厂商代码。
RTL8153 是主线驱动支持很久的芯片。

## 3. 踩坑记录

`udhcpc` 在接口还是 down 的时候会无限打印：

```
udhcpc: read error: Network is down, reopening socket
```

如果 shell 没有控制终端，Ctrl-C 也打不断它；因此启动 DHCP 前先
`ip link set <if> up`，并给 `udhcpc` 加超时，交互 shell 要有控制终端。

## 4. 6.18 rootfs 网络自动接管（DHCP / systemd-networkd）

6.18 测试启动项保留 `systemd.mask=NetworkManager.service`，把 USB
网卡交给 rootfs 的 systemd-networkd：

`/etc/systemd/network/10-<usb-if>.network`：

```ini
[Match]
Name=<usb-if>

[Network]
DHCP=yes
IPv6AcceptRA=yes
```

6.18 启动行增加：

```text
systemd.wants=systemd-networkd.service
systemd.wants=systemd-networkd.socket
```

停用临时 `charlotte-net.service` 及其 drop-in；它只适用于 6.18
测试，脚本已损坏，会与 networkd 抢接口。5.10 默认启动项不改：
其 NetworkManager/原有网络配置继续工作；networkd 只在 6.18 启动项
里被 want 起来，`.network` 文件不会被 NM 读取。

验证：

```sh
systemctl is-active systemd-networkd
networkctl status <usb-if>
ip -4 route
cat /run/systemd/netif/leases/*
```

DHCP 地址是动态的，不要写死旧的静态地址。IPv4 变化时，从
`networkctl`/lease 读取新地址，或用 USB 网卡的 IPv6 link-local
（`ssh <user>@fe80::...%<host-if>`）恢复。当前 rootfs 没有
systemd-resolved；DHCP 的 DNS/domain 在 lease 中可见，
`/etc/resolv.conf` 仍由 rootfs 原有配置提供。若需要 DHCP DNS
自动写入，可另行启用 resolved 或 resolvconf。
