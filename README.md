English | [中文](docs/README_zh.md)

# linux-mainline-hisi-9000c

Build scripts, patches, kernel configuration, DTS files and tools for bringing up mainline Linux 6.18 on the HUAWEI Kirin 9000C SBC (codename `charlotte`, board `QHW-100` / `SP1PANGUXM`).

The board boots through UEFI. The stock vendor 5.10 kernel
(`5.10.97-38-9000c`) stays the default; the 6.18 image is a separate test build.

## What is included

### Repository layout

- `patches/`: the 6.18 kernel patch series (`git am`-able, `v6.18` baseline)
- `defconfig/`: local kernel configuration used for the 6.18 build
- `drivers/`: local mirrors of the driver sources added/modified by the series
- `dts/`: local mirrors of the board DTS/DTSI used by the series
- `docs/`: bring-up notes and build guide
- `firmware/`: firmware notes (no board firmware blob is required for 6.18)
- `tools/`: on-device helpers (MMIO diagnostic tool)
- `scripts/ci/`: non-interactive build script

### Feature status

| Feature                               | 5.10 vendor | 6.18 mainline                                                             |
| ------------------------------------- | ----------- | ------------------------------------------------------------------------- |
| 8c12t SMP boot (8 cores / 12 threads) | yes         | yes                                                                       |
| UART console (`uart_v500`)            | yes         | yes                                                                       |
| UFS (3 LUNs, 43 partitions)           | yes         | yes, HS-G4 / 2 lanes (~931 MB/s read)                                     |
| PCIe0 (onboard ASM1064 SATA)          | yes         | yes, Gen3 x1 + `ahci`                                                     |
| PCIe1 (external x1 slot)              | yes         | yes, Gen3 x1; needs FF-A `SET_MEM_UNSEC`; BAR0 at bus `0x90000000`, 56 MiB CPU window at `0xea200000` |
| USB Ethernet (RTL8153 / `r8152`)      | yes         | yes, 1000/full, DHCP + IPv6                                               |
| amdgpu                                | not built   | yes: X11 GNOME, Weston and GNOME Wayland; radeonsi/GLES 3.2; 4K↔1080p mode switching fixed; known issues below |
| Display (vendor DPU/HDMI)             | yes         | not yet (vendor `dkmd` only)                                              |
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
- `0015`: restore DCE vblank/pageflip IRQs after a modeset
- `0016`: only unpin the old FB when legacy set_base pinned it

### Known issues / current work

- GDM greeter works after disabling the board-specific SoC display
  compatibility library injection for the greeter. That injection comes
  from a PAM environment file and conflicts with the AMD dGPU's
  EGL/GBM; with it disabled the greeter obtains a high-priority EGL
  context and starts Wayland. The user session is unaffected. The
  earlier scanout BO `pin failed` was a shadow-buffer symptom of the
  EGL failure; no kernel pin change is needed for that path. Patch
  0016 only fixes the old-FB unpin boundary. Details:
  [中文](docs/scanout_bo_pin_zh.md).
- 4K↔1080p mode switching is fixed by patch 0015 (restore the DCE
  vblank/pageflip IRQ masks after a modeset). Verified under GNOME
  Wayland (six switches under load) and X11. The session-start
  `ttm_bo_unpin` warning is fixed by patch 0016: the legacy set_base
  path now unpins the old FB only when it holds a pin. The user-session
  startup `pin failed` messages and the X11-client-exit
  `ttm_bo_release` warning remain; neither is fatal.
- GNOME session startup can take 1–2 minutes because UPower/colord/IBus
  D-Bus activations time out; Wayland clients started too early may
  block on frame callbacks.
- Weston with vsync is half the monitor refresh (33 FPS at 1080p60,
  17 FPS at 4K30); with `vblank_mode=0` it reaches ~148 FPS / 115 FPS.
  This is a presentation-path issue (legacy non-atomic DRM), not GPU
  rendering capacity.
- GPU DPM works: `low` = 300/300 MHz, `high` = 825/1000 MHz, and
  `auto` ramps to 825/1000 within ~1 s under load and falls back to
  300/300 within ~10 s with GDM stopped. `amdgpu_pm_info` values are
  in 10 kHz units (30000 = 300 MHz). The desktop can stay at
  825/1000 because of compositor/WebKit background activity.
- TODO: exercise Xwayland under GNOME Wayland (X11 apps, DRI3, input,
  HiDPI scaling).

### Tools

- `tools/charlotte-diag.c`: freestanding aarch64 MMIO dump tool (`dump`, `blocks`,
  `ranges`, `find`, `fill`, `fbbars`, `fbinfo`)
- `tools/initramfs-console-init.sh`: initramfs init with a respawning shell
- `tools/initramfs-diag-init.sh`: early framebuffer/MMIO diagnostic init

## Getting started

- Build guide (Ubuntu/Debian on the board): [English](docs/build_guide_ubuntu_en.md) | [中文](docs/build_guide_ubuntu_zh.md)
- UFS bring-up notes: [中文](docs/ufs_bringup_zh.md)
- PCIe / amdgpu notes: [中文](docs/pci_amdgpu_zh.md)
- amdgpu bring-up and desktop evidence: [中文](docs/amdgpu_bringup_zh.md)
- GDM greeter scanout BO pin analysis: [中文](docs/scanout_bo_pin_zh.md)
- USB Ethernet / network notes: [中文](docs/net_bringup_zh.md)
- Display bring-up plan: [中文](docs/display_bringup_zh.md)
- Full bring-up report: [中文](docs/bringup_report_zh.md)

## Build verification

The patch series and `defconfig/charlotte_6.18_defconfig` build cleanly on a
fresh upstream `v6.18` checkout.  The historical verification, the configuration
consistency check and the `git am` conflict pitfall are recorded in
[中文](docs/build_verification_zh.md).

## Safety notes

- Do not read unpowered display/PCIe MMIO blocks; on a cold 6.18 boot they can
  trigger a NoC SError and reset the board.

## References

- HUAWEI phone kernel source drop (`HBN-AL00,HBN-AL10,HBN-AL80`): vendor UFS,
  kport PCIe, extreme CRG and dkmd display implementations
- Live vendor device tree of the board (QHW-100)
