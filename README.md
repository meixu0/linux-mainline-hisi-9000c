English | [中文](docs/README_zh.md)

# linux-mainline-hisi-9000c

Build scripts, patches, kernel configuration, DTS files and tools for bringing up mainline Linux 6.18 on the HUAWEI Kirin 9000C SBC (codename `charlotte`, board `QHW-100` / `SP1PANGUXM`).

The board chain-loads Linux through UEFI -> GRUB. The stock vendor 5.10 kernel
(`5.10.97-38-9000c`) stays the default boot entry; the 6.18 image is a manual
one-shot test entry (`grub-reboot`).

## What is included

### Repository layout

- `patches/`: the 6.18 kernel patch series (`git am`-able, `v6.18` baseline)
- `defconfig/`: local kernel configuration used for the 6.18 build
- `drivers/`: local mirrors of the driver sources added/modified by the series
- `dts/`: local mirrors of the board DTS/DTSI used by the series
- `docs/`: bring-up notes, build guide and captured serial logs
- `firmware/`: firmware notes (no board firmware blob is required for 6.18)
- `packaging/`: notes on the vendor reference packages used during bring-up
- `tools/`: on-device helpers (MMIO diagnostic tool, initramfs scripts, chroot helper)
- `scripts/ci/`: non-interactive build script
- `scripts/local/`: scripts meant to be run on the board itself

### Feature status

| Feature                               | 5.10 vendor | 6.18 mainline                                                             |
| ------------------------------------- | ----------- | ------------------------------------------------------------------------- |
| 8c12t SMP boot (8 cores / 12 threads) | yes         | yes                                                                       |
| UART console (`uart_v500`)            | yes         | yes                                                                       |
| UFS (3 LUNs, 43 partitions)           | yes         | yes, HS-G4 / 2 lanes (~931 MB/s read)                                     |
| PCIe0 (onboard ASM1064 SATA)          | yes         | yes, Gen3 x1 + `ahci`                                                     |
| PCIe1 (external x1 slot)              | yes         | yes, Gen3 x1; needs FF-A `SET_MEM_UNSEC`; BAR0 at bus `0x90000000`, 56 MiB CPU window at `0xea200000` |
| USB Ethernet (RTL8153 / `r8152`)      | yes         | yes, 1000/full, DHCP + IPv6                                               |
| amdgpu                                | not built   | yes: X11 GNOME desktop, amdgpu DDX + glamor/radeonsi + DRI3, GL 4.6 / GLES 3.2 |
| Display (vendor DPU/HDMI)             | yes         | not yet (vendor `dkmd` only)                                              |
| Rootfs chroot from initramfs          | n/a         | yes (`sdd2-chroot`, read-only by default)                                 |
| Network in initramfs                  | n/a         | yes (`net-dhcp`)                                                          |

### Patch sources

All patches are local work for this board, derived from:

- the live vendor device tree (`dtc -I fs -O dts /proc/device-tree`), and
- the HUAWEI phone kernel source drop (`HBN-AL00,HBN-AL10,HBN-AL80`), in
  particular `platform_source/basicplatform/drivers/{pci,clk,gpio}` and
  `drivers/video/fbdev/dkmd`.

`hi3660`/`hi3670` code is deliberately **not** used as a reference: the
Kirin 9000C UFS/PCIe/clock blocks are different.

- `0001`: add the QHW-100 (charlotte) board DTS and SoC skeleton
- `0002`: add the `uart_v500` serial console driver (interactive console)
- `0003`: add the read-only DSS power probe used during display recon
- `0004`: add HUF S per-core queue support to the mainline UFS core
- `0005`: add the `ufshcd-charlotte` variant driver
- `0006`: negotiate HS-G4 through the private UniPro window
- `0007`: add the DesignWare PCIe host glue (`pcie-charlotte`)
- `0008`: give PCIe1 a 64-bit BAR window and inbound `dma-ranges`
- `0009`: release PCIe1 through the platdrv FF-A secure partition
- `0010`: amdgpu bus mastering + SI VRAM aperture refresh
- `0011`: report the 56 MiB charlotte aperture and force GTT write-combined
- `0012`: keep charlotte user-space VRAM mappings in GTT
- `0013`: add the SI PASID TLB flush callback
- `0014`: use I/O accessors for SI reset magic and UVD messages

### Tools

- `tools/charlotte-diag.c`: freestanding aarch64 MMIO dump tool (`dump`, `blocks`,
  `ranges`, `find`, `fill`, `fbbars`, `fbinfo`)
- `tools/sdd2-chroot`: initramfs helper that mounts the rootfs read-only and
  chroots into it
- `tools/initramfs-console-init.sh`: initramfs init with a respawning shell
- `tools/initramfs-diag-init.sh`: early framebuffer/MMIO diagnostic init

## Boot artifact layout

The 6.18 test entry uses a dedicated directory instead of the distro
`kernel-install` flow:

```
/boot/linux-6.18-charlotte/
├── Image                      # kernel (uncompressed arm64 Image)
├── Image.pre-pcie             # rollback image before the PCIe series
├── Image.safe                 # known-good early image
├── Image.ufsworking           # UFS-working image
├── charlotte-9000c.dtb        # matching device tree
└── initramfs-console.cpio.gz  # busybox initramfs with sdd2-chroot
```

The GRUB entry is named `Linux 6.18 charlotte bring-up (TEST - uart console)` and
is only selected through `grub-reboot`; the GRUB default must stay on 5.10.

## Getting started

- Build guide (Ubuntu/Debian on the board): [English](docs/build_guide_ubuntu_en.md) | [中文](docs/build_guide_ubuntu_zh.md)
- UFS bring-up notes: [中文](docs/ufs_bringup_zh.md)
- PCIe / amdgpu notes: [中文](docs/pci_amdgpu_zh.md)
- amdgpu bring-up and desktop evidence: [中文](docs/amdgpu_bringup_zh.md)
- USB Ethernet / network notes: [中文](docs/net_bringup_zh.md)
- Display bring-up plan: [中文](docs/display_bringup_zh.md)
- Patch set and rootfs chroot: [中文](docs/rootfs_chroot_zh.md)
- Full bring-up report: [中文](docs/bringup_report_zh.md)
- Serial logs: [docs/logs](docs/logs/)

## Build verification

The repository was verified end-to-end on 2026-11-24: a clean upstream `v6.18`
checkout, the patch series and `defconfig/charlotte_6.18_defconfig` build an
Image whose configuration is identical to the working 6.18 image on the board.
The image was booted and reproduces the expected functionality:

- kernel `6.18.0-00007-g0f907a9a9106`, Image md5 `4690ec04b1a04b2183411104d17014ee`
- UFS `sda`..`sdd` (HS-G4 link), PCIe0 Gen3 x1 + onboard SATA (`ahci`),
  UART console, built-in amdgpu
- matching DTB (`b5232b5772bdefccd8a2bb2da7c1cdb4`)

Details: [中文](docs/build_verification_zh.md).

## Safety notes

- Never change the GRUB default or overwrite the 5.10 kernel/rootfs.
- The board has a single 1 MiB unallocated region on `sdd` (sectors 256-511) used
  for write tests; always save and restore its contents.
- `/dev/ttyUSB0` is single-reader: stop any stale `cat` before capturing.
- Do not read unpowered display/PCIe MMIO blocks; on a cold 6.18 boot they can
  trigger a NoC SError and reset the board.

## References

- HUAWEI phone kernel source drop (`HBN-AL00,HBN-AL10,HBN-AL80`): vendor UFS,
  kport PCIe, extreme CRG and dkmd display implementations
- Live vendor device tree of the board (QHW-100)
