# 6.18 PCIe + amdgpu 方案

日期：2026-11-24


## 2. 5.10 上的 PCIe 现状（实测）

- 两个 kport 控制器：

| 控制器 | DBI | APB | PHY | config | 内存窗口 | 用途 |
|---|---|---|---|---|---|---|
| `f8000000.pci` | `0xf8000000` | `0xee0a3000` | `0xee0a0000` | `0xf8400000` | `0xf8500000` + **0x1d00000 (29 MB)** | **板载 ASM1064 SATA**（焊死） |
| `edc00000.pci` | `0xedc00000` | `0xee1a3000` | `0xee1a0000` | `0xea000000` | `0xea100000` + **0x3b00000 (59 MB)** | **外部 x1 插槽（现在空）** |

- 5.10 实测：`00:00.0`（Huawei 19e5:36b0 根端口）+ `01:00.0`
  （ASMedia ASM1064 SATA，`ahci` 驱动），链路 **8.0 GT/s ×1 (Gen3)**。
- 空插槽对应的 `edc00000` 在 5.10 上打印 `Link Fail, status [0x0]`
  并枚举失败，符合“没插卡”。
- 根端口归属 `f8000000.pci`，即 **板载 SATA 走 PCIe0**；
  x1 插槽走 **PCIe1**。

## 3. 6.18 现状与已确认的坑

- 6.18 配置里 `# CONFIG_PCI is not set`，DTS 里也没有 PCIe 节点。
- **直接读 PCIe 寄存器会 SError**：6.18 冷启动后
  `devmem 0xf8000000` → `SError Interrupt ... Asynchronous SError` → panic
  （`reboot_reason=AP_S_NOC`）。
  → 和 DPU 一样：**PCIe 块在上电/开时钟前不能被 AP 访问**。
- 因此不能“只加 DT + 打开 CONFIG_PCI”就完事，必须先把
  时钟/PLL/PHY/复位/NoC 上电序列补上。

## 4. 厂商上电序列（从 pcie-jun.c / pcie-kport*.c 提取）

关键寄存器（PCIe0）：

```
CRG  0xee262000:
  0x010 PEREN1   : bit3 pclk_phy, bit4 pclk_sys, bit5 aclk, bit10 tcu, bit11 tbu,
                   bit1 hp, bit2 debounce
  0x014 PERDIS1  : bit6 phyref(交 HW), bit7 io(交 HW)
  0x060 PERRSTEN0 / 0x064 PERRSTDIS0
        rc0: bit7 ctrl_por_n, bit4 asyncbrg, bit3 tcu, bit5 tbu
        rc0 复位值 0x300（assert_info）
  0x300 PCIECTRL0 : bit0 io hw bypass, bit1 phyref hw bypass
  0x190 PLL ctrl0 (EN bit0), 0x194 FCW=0x2ee00000 (100 MHz), 0x204 lock bit4
PLL  0xee263000: 25 个 FNPLL 参数寄存器（0x008..0x070）
PHY  0xee0a0000:
  +0x000 ctrl0 bit14 = 1 表示用 CIO 参考（RC 要清 0 = 用 FNPLL）
  +0x004 ctrl1 bit1 por_n, bit17 reset, bit19 pipe lane0 reset
  +0x09c ctrl39 bit10 rx termination
  +0x400 state0 bit19 pipe_clk_stable（0 = 稳定）
  +0x1400 PCS power status bit7 = PHY 上电完成
  +0x2000 起的 APB PHY 窗口
PMCTRL 0xffb81000: 0x380 idlereq, 0x384 idle status
  rc0: mask bit25, status bit9
SCTRL 0xfb21b000: 0x170 bit19 = PCIe0 aux gate (bit20 = PCIe1)
APB   0xee0a3000:
  +0x01c ctrl7  bit11 = LTSSM enable
  +0x030 ctrl12 PERST 控制
  +0x054 ctrl21 io clk 选择
  +0x410 state4 链路/LTSSM 状态
```

顺序（pcie_turn_on）：

1. 开 internal clock（apb_phy + apb_sys）
2. 解除 controller 复位 + async bridge 复位
3. AXI timeout、PHY POR
4. RC 模式、PERST 输出、参考时钟选择
5. FNPLL 配置 + 100 MHz FCW + lock
6. 开 external clock（aclk/tcu/tbu/aux）
7. PHY v1 初始化表（29 个寄存器）
8. 解除 ctrl_por_n
9. 释放 PERST
10. TCU/TBU 出复位
11. 等 pipe clk stable、等 PHY power-on
12. rx termination、退出 NoC power idle、DBI split 设置
13. `dw_pcie_setup_rc()` + LTSSM enable + 等链路

## 5. 6.18 实现（第一版）

新增 `drivers/pci/controller/dwc/pcie-charlotte.c`：

- 用 mainline DW core（`dw_pcie_host_init`），只补厂商 glue：
  - 映射 `dbi/apb/phy/config/crg/pll/sctrl/pmctrl` 八个窗口
  - 按上面的顺序做上电（PHY 表、FNPLL、CRG 门控、PERST、NoC idle）
  - `start_link` 写 APB CTRL7 bit11
  - MSI 暂时 stub（走 INTx；x1 插槽的 MSI 以后再加）
- DTS 先只加 **PCIe0（板载 SATA）**，验证通过后再加 PCIe1（x1 插槽）。
  这样可以用“焊死的 SATA 芯片”当零风险测试目标。
- 同时打开：
  `CONFIG_PCI/PCI_MSI/PCIE_DW/PCIE_DW_HOST/PCIE_CHARLOTTE`、
  `CONFIG_ATA/SATA_AHCI`、`CONFIG_DRM_AMDGPU(+DC)`。

验收标准（第一步）：

```
6.18 串口日志:
  charlotte-pcie f8000000.pcie: PCIe0 power-on
  charlotte-pcie f8000000.pcie: DBI id=0x19e536b0 link=1
  ... pci 0000:00:00.0 ...
  ... pci 0000:01:00.0: [1b21:1064] SATA controller
```

## 6. amdgpu 与 x1 插槽的已知风险

**内存窗口太小**：x1 插槽只有 59 MB（PCIe1 的 ranges）。
普通 AMD 亮机卡的 VRAM BAR 一般是 256 MB，无法放进这个窗口。
在插卡之前需要先在 5.10 上确认卡的 BAR 大小：

```
lspci -vv -s 01:00.0 | grep -E 'Region|LnkCap|LnkSta'
cat /sys/bus/pci/devices/0000:00:00.0/resource   # 桥窗口
```

- 如果 BAR2 ≤ 59 MB → 可以直接试。
- 如果 BAR2 = 256 MB → 需要扩大窗口：
  - 4 GB 以下基本没有 256 MB 连续空洞（0xc0000000 起有厂商
    `aod_dynamic_cma` 164 MB，之后到 0xe4000000 之间可能有 ~400 MB 空洞，
    但需要 NoC 地址译码支持，风险较高）；
  - 或者用 64 位窗口（CPU 地址 >4 GB，NoC 是否支持未知）。
- 这一步要等有卡时用 5.10 实测，不能盲试。

## 7. 下一步

1. 编 6.18（PCIe0 + amdgpu 内置），串口验证 SATA 链路。
2. 成功后加 PCIe1（x1 插槽）节点，验证空槽 `Link Fail` 不崩。
3. 用户插 AMD 卡后：先 5.10 `lspci -vv` 看 BAR，再决定是否直接试 6.18。
4. 6.18 下 amdgpu 起来后，fbcon 应该直接出现在 AMD 卡的 HDMI 上。

---

## 8. 结果：6.18 PCIe0（板载 SATA）已经打通

最终 Image：`/boot/linux-6.18-charlotte/Image`，md5 `8247f40db86c917609d81daac7efa5ef`
（对应内核提交 `5730540 pci: dwc: add charlotte PCIe host support`，
补丁 `patches/0007-pci-dwc-add-charlotte-PCIe-host-support.patch`）。

6.18 串口日志（`logs/` 里也保存了）：

```
charlotte-pcie f8000000.pcie: PCIe0 power-on
charlotte-pcie f8000000.pcie: DBI id=0x36b019e5 link=0
charlotte-pcie f8000000.pcie: iATU: unroll T, 8 ob, 8 ib, align 64K, limit 4G
charlotte-pcie f8000000.pcie: PCIe Gen.3 x1 link up
pci 0000:01:00.0: [1b21:1064] type 00 class 0x010601 PCIe Endpoint
pci 0000:01:00.0: BAR 0 [mem 0xf8580000-0xf8581fff]: assigned
pci 0000:01:00.0: BAR 5 [mem 0xf8582000-0xf8583fff]: assigned
ahci 0000:01:00.0: AHCI vers 0001.0301, 32 command slots, 6 Gbps
scsi host0: ahci ... scsi host3: ahci
charlotte-pcie f8000000.pcie: host ready, link=1
```

`ata2/3/4: SATA link down` 是正常的：ASM1064 的 4 个 SATA 口上没接硬盘
（板子的系统盘是 UFS）。

### 8.1 关键修复：PCIECTRL0 bit11

对比 5.10（链路正常）和 6.18（链路死活不通）的 CRG `0xee262300`：

| | PCIECTRL0 |
|---|---|
| 5.10 运行态 | `0x0FCE0400` |
| 6.18（修复前） | `0x0FCE0C00` |

只差 **bit11 = `PCIEIO_OE_EN_HARD_BYPASS`**。
厂商 `pcie_io_adjust`/`pcie_oe_config()` 会把它清掉（交给 CLKREQ 硬件控制），
我们第一版没清，结果 LTSSM 一直在 polling、TXPLL 正常也练不上链路。
清掉 bit11 后第一次启动就 `Gen.3 x1 link up`。

### 8.2 其他必要的坑

- PHY 表：DT 里 `version = <1>` 对应厂商的 **`PCIE_PHY_VERSION_2`**（v2 表），
  不是 v1 表。用错表时 PHY power-on 超时。
- PERST 不走控制器内部：板载 SATA 的 PERST 是 `gpio@ff806000` 的 pin14
  （5.10 debugfs 里 `gpio-46 pcie_reset out hi`）。6.18 没有 GPIO 驱动，
  驱动里直接写 hiv500 GPIO 的 data/control 寄存器：
  `data=0x04, mask=bit(n+16)`，`ctrl=0x08+4*n = 0x00100010`。
- 时钟：CRG `0xee262000` 的 PEREN1(0x10) / PEREN0(0x00) 直接写 gate 位，
  FNPLL 用 `0x190/0x194` + 锁定位 `0x204 bit4`。
- DBI 映射必须自己先 map（`devm_ioremap`），因为 `dw_pcie_host_init()`
  里才做 `dw_pcie_get_resources()`，之前 `pci->dbi_base` 还是 NULL。
- 不能给 `dw_pcie_ops.link_up` 回调：6.18 的 `dw_pcie_link_up()` 本身就会
  回调 `ops->link_up`，会无限递归爆栈。
- 共享的 CRG/PLL/SCTRL/PMCTRL 窗口第二个控制器会 `-EBUSY`，
  要用 `devm_ioremap()`（不要 request resource）。

## 9. amdgpu 编译状态

- 配置：`CONFIG_PCI=y`、`CONFIG_PCIE_DW=y`、`CONFIG_PCIE_DW_HOST=y`、
  `CONFIG_PCIE_CHARLOTTE=y`、`CONFIG_ATA=y`、`CONFIG_SATA_AHCI=y`、
  `CONFIG_DRM_AMDGPU=y`、`CONFIG_...`（SI/CIK/USERPTR 全开）。
- `vmlinux` 里有 `amdgpu_init`，说明 amdgpu 已经编进 6.18（不是模块，
  因为 `DRM_AMDGPU` 被 olddefconfig 提升成了 y；功能等价，启动即加载）。
- 用户态固件：sdd2 的 `/lib/firmware/amdgpu`（699 个文件）和 mesa
  `radeonsi_dri.so` 都已具备，不需要额外安装。

## 10. PCIe1（x1 插槽）参考

结论：**厂商 5.10 的 PCIe1 实现和现在跑的 5.10 厂商内核是同一套**，
配置项逐项一致（`PCIE_KPORT=y/JUN/PC/KPHY/DBI_SPLT` 等），
没有额外的“桌面版私有实现”。真正有参考价值的是它的 **电源**：

5.10 设备树里：

```
power_misc {
    compatible = "armpc,power_misc";
    pcieSwitch-supply = <&ldo16>;    /* 2.5V */
    pcie2sata-supply  = <&ldo41>;    /* 1.1V */
    regulator_names_list = "pcieSwitch", "pcie2sata", "mipi2hdmi", "dp2vga";
    regulator_values = <0x2625a0 0x10c8e0 0x129da0 0x1312d0>;
};
```

- `pcieSwitch` = **PMIC LDO16**（2.5V）：给 x1 插槽/PCIe1 供电；
- `pcie2sata` = **PMIC LDO41**（1.1V）：给板载 ASM1064 供电；
- 5.10 运行态两个都是 `enabled`（厂商 `pcie_power.c` 启动时
  `regulator_set_voltage + regulator_enable`）。

6.18 没有 PMIC/power_misc 驱动，也没有 GPIO 驱动；PCIe1 在冷启动时
读 DBI 直接 SError（`edc00000`），说明它的电源域没起来。
因此 **PCIe1 目前在 DTS 里是 `status = "disabled"`**，
等有 AMD 卡可测时再做：

1. 在 6.18 里补一个最小的 PMIC LDO16 使能（或 UEFI 保持 LDO16 常开）；
2. 使能 PCIe1 节点，验证空槽时 `link down` 不崩；
3. 插卡后先 5.10 `lspci -vv` 看 BAR（x1 窗口只有 59MB，普通亮机卡
   VRAM BAR 256MB 放不下，这一点仍然要先确认）。

## 11. 当前结论

- **6.18 的 PCIe 链路已经打通**（板载 SATA 作为验收目标）。
- **amdgpu 已经编进 6.18**，只差一张卡和 PCIe1 的电源。
- 下一步的阻塞点是 **PCIe1 的 LDO16（pcieSwitch）电源** 和
  **AMD 卡的 BAR 大小**，两者都需要插卡后现场验证。
