[English](../README.md) | 中文

# 6.18 网络 bring-up：USB RTL8153（r8152）

日期：2026-11-24
平台：QHW-100（Kirin 9000C，8c12t）

## 1. 5.10 原厂系统的网络现状

5.10 上唯一的网卡是**USB 以太网**，不是板载 GMAC：

```
$ lspci -nn
00:00.0 PCI bridge   [19e5:36b0]
01:00.0 SATA         [1b21:1064]     <- ASM1064，仅 SATA

$ lsusb
Bus 002 Device 005: ID 0bda:8153 Realtek RTL8153 Gigabit Ethernet Adapter
Bus 002 Device 002: ID 0bda:0411 Realtek Hub (RTS5411)
...
$ ethtool -i enx<usb>
driver: r8152
version: v2.17.1.nosg (2025/08/27)     <- Realtek 外置驱动，编入 5.10
```

拓扑：

```
dwc3 (efc00000.hisi_usb) -> xhci-hcd -> RTS5411 hub -> RTL8153 (0bda:8153)
```

5.10 上的网络配置（DHCP）：

```
enx<usb>  inet 192.0.2.10/24
default via 192.0.2.1
nameserver 192.0.2.1
```

## 2. 6.18 的问题

USB 控制器本身在 6.18 已经是好的（`dwc3` + `dwc3-of-simple` + `xhci-hcd`，
`0xefc00000`，两个 root hub 都能枚举）。真正的问题在**内核配置**：

```
$ grep CONFIG_NET .config
# CONFIG_NET is not set
```

也就是说 6.18 的 bring-up 配置里**整个网络子系统都没有编译**，自然也没有
USB 网卡驱动。串口 initramfs 里 `ip` 命令存在，但内核没有 netdev。

## 3. 修改

### 3.1 内核配置

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

### 3.2 initramfs

`tools/initramfs-console-init.sh` 新增：

- 创建 `/etc` 和 `/bin/sh -> /bin/busybox`（udhcpc 的脚本依赖 `/bin/sh`）
- 启动时打印 `net:` 一行，列出 `eth0`
- 交互 shell 尽量用 `setsid -c` 启动，给它一个控制终端，
  这样卡住的前台命令可以用串口 Ctrl-C 打断

initramfs 里额外放入：

- `/lib/firmware/rtl_nic/rtl8153*.fw`（5 个文件，共 ~7 KB）
- `/net-dhcp`：先 `ip link set eth0 up`，再用 busybox `udhcpc` 取地址，
  写入 `/etc/resolv.conf`；整条命令有 30 秒超时

## 4. 验证结果

启动日志：`docs/logs/6.18-net-r8152-boot.log`

```
r8152-cfgselector 2-1.1: reset SuperSpeed USB device number 3 using xhci-hcd
r8152 2-1.1:1.0 eth0: v1.12.13
...
~ # ip link set eth0 up
r8152 2-1.1:1.0 eth0: carrier on
```

链路与地址：

```
eth0: link/ether <mac>        (与 5.10 相同)
speed=1000 duplex=full
udhcpc: lease of 192.0.2.10 obtained from 192.0.2.1
inet 192.0.2.10/24
inet6 fd00::.../64 scope global dynamic   (路由器 RA)
default via 192.0.2.1 dev eth0
```

连通性：

```
ping 192.0.2.1     -> 0.17-0.86 ms, 0% loss
ping 223.5.5.5       -> 23.1-23.3 ms, 0% loss
nslookup www.baidu.com 192.0.2.1 -> 153.3.238.127
wget http://mirrors.aliyun.com/debian/README -> 1197 bytes
```

chroot 进 sdd2 上的 GXDE OS 25 之后（共享同一个 netns）：

```
sdd2-chroot rw /bin/bash -c 'ping 223.5.5.5; getent hosts mirrors.aliyun.com'
-> 0% packet loss; 119.36.231.159 ...
```

## 5. 踩坑记录

`udhcpc` 在接口还是 down 的时候会无限打印：

```
udhcpc: read error: Network is down, reopening socket
```

旧 initramfs 的交互 shell 没有控制终端，Ctrl-C 也打不断它，串口就卡死了。
所以 `net-dhcp` 先 `ip link set eth0 up`，并且 `udhcpc` 外面套 `timeout 30`；
新 init 还会用 `setsid -c` 启动 shell。

## 6. 当前镜像

```
/boot/linux-6.18-charlotte/Image     6.18.0-00007-g57305409a010
md5 ad8f629d0cf4b926ff170e5a792bb03e
```

回滚镜像：`/boot/linux-6.18-charlotte/Image.pre-net`
（`8247f40db86c917609d81daac7efa5ef`，无网络栈）。

下次要进 6.18：

```sh
sudo grub-reboot "Linux 6.18 charlotte bring-up (TEST - uart console)"
sudo systemctl reboot
# 串口里：
net-dhcp
```
