# patches/

6.18 kernel patch series for the HUAWEI Kirin 9000C (`charlotte`) board.
Baseline: upstream `v6.18`. Apply in order with `git am`.

| # | Subject | Area |
|---|---|---|
| 0001 | arm64: dts: hisilicon: add QHW-100 (charlotte) board support | DTS |
| 0002 | tty: serial: add uart_v500 serial console driver | console |
| 0003 | misc: add dss-power-probe diagnostic driver | display recon |
| 0004 | ufs: core: add HUF S per-core queue support for charlotte | UFS core |
| 0005 | ufs: host: add ufshcd-charlotte variant driver | UFS host |
| 0006 | ufs: charlotte: negotiate HS-G4 through the private UniPro window | UFS speed |
| 0007 | pci: dwc: add charlotte PCIe host support | PCIe |
| 0008 | arm64: dts: charlotte: enable PCIe1 with a 64-bit BAR window | DTS |
| 0009 | pci: dwc: charlotte: release PCIe1 through the platdrv FF-A partition | PCIe |
| 0010 | drm/amdgpu: enable bus mastering and refresh the SI VRAM aperture | GPU |
| 0011 | drm/amdgpu: report the 56 MiB charlotte aperture and force GTT WC | GPU |
| 0012 | drm/amdgpu: keep charlotte user-space VRAM mappings in GTT | GPU |
| 0013 | drm/amdgpu: add the SI PASID TLB flush callback | GPU |
| 0014 | drm/amdgpu: use I/O accessors for SI reset magic and UVD messages | GPU |
| 0015 | drm/amdgpu: restore DCE vblank and pageflip IRQs after a modeset | GPU |

## Status

0001-0015 are functional.  PCIe1 (the external x1 slot) trains, enumerates
and gives the card a 256 MiB BAR0; on this board BAR0 is relocated to bus
`0x90000000` and the CPU reaches 56 MiB of it through the low window at
`0xea200000`.  0009 also programs the inbound iATU windows for device DMA
(do **not** use `dma-ranges` for this board, see the doc).

amdgpu brings up VBIOS, VRAM, GART, DPM and the display connectors, and
reaches X11 GNOME, Weston and GNOME Wayland with radeonsi (GL 4.6 /
GLES 3.2).  The SI VRAM window, GTT cache policy, user-space GTT policy,
PASID TLB flush and 8-byte-safe VRAM accessors are patches 0010-0014;
patch 0015 restores the DCE vblank/pageflip IRQ masks after a modeset,
which fixes 4K↔1080p switching (verified under Wayland and X11).  See
`docs/amdgpu_bringup_zh.md` for the evidence and the remaining
observations.


## Apply

```sh
git clone --depth 1 --branch v6.18 <linux.git> linux-6.18
cd linux-6.18
git am /path/to/patches/*.patch
```

The series has been verified with `git am` on a clean `v6.18` checkout: all
fifteen commits replay without conflicts.

## Notes

- Patch 0004 touches the mainline UFS core; keep it before 0005/0006.
- Patch 0007 depends on `CONFIG_PCIE_DW_HOST`, which it selects through its own
  Kconfig entry.
- Debug prints are deliberately `dev_dbg`; enable dynamic debug only when
  investigating.
