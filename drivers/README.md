# drivers/

Local mirrors of the driver sources added or modified by `patches/`. They exist
so the code can be read and diffed without unpacking the patch series; the
patches remain the authoritative form.

| Path | Kernel location | Patch |
|---|---|---|
| `tty/uart-v500.c` | `drivers/tty/serial/uart-v500.c` | 0002 |
| `tty/uart-v500-earlycon.c` | `drivers/tty/serial/uart-v500-earlycon.c` | 0002 |
| `ufs/ufs-charlotte.c` | `drivers/ufs/host/ufs-charlotte.c` | 0005 |
| `pci/pcie-charlotte.c` | `drivers/pci/controller/dwc/pcie-charlotte.c` | 0007, 0009 |
| `misc/dss-power-probe.c` | `drivers/misc/dss-power-probe.c` | 0003 |
| `gpu/drm/amd/amdgpu/amdgpu_drv.c` | `drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c` | 0010 |
| `gpu/drm/amd/amdgpu/gmc_v6_0.c` | `drivers/gpu/drm/amd/amdgpu/gmc_v6_0.c` | 0010, 0011, 0013 |
| `gpu/drm/amd/amdgpu/amdgpu_ttm.c` | `drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c` | 0011, 0012 |
| `gpu/drm/amd/amdgpu/amdgpu_gem.c` | `drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c` | 0012 |

The UFS core changes (`drivers/ufs/core/ufshcd.c`, `include/ufs/*`) are only in
`patches/0004`; mirroring the ~10k-line core file here would add noise without
any benefit.

## Notes on `pcie-charlotte.c`

The driver is a minimal glue layer around the mainline DesignWare host core.
It reproduces the vendor cold-boot power-on sequence and nothing else:

- CRG gate enables and the FNPLL 100 MHz reference
- kphy v2 init table (the DTS phy node says `version = <1>`, which is
  `PCIE_PHY_VERSION_2` in the vendor driver)
- PERST via the board GPIO plus the controller's internal PERST
- NoC power-idle exit

The non-obvious register is `PCIECTRL0` bit 11 (`PCIEIO_OE_EN_HARD_BYPASS`).
The vendor driver clears it; leaving it set keeps the link in LTSSM polling
forever even though the PHY PLL is locked.

## Notes on the amdgpu mirrors

These files contain the board-specific amdgpu bring-up:

- `amdgpu_drv.c`: probe-time `pci_set_master()` (0010);
- `gmc_v6_0.c`: `MC_VM_FB_LOCATION` / `HDP_NONSURFACE_*` refresh (0010),
  the 56 MiB relocated aperture (0011) and the SI PASID TLB flush
  callback (0013);
- `amdgpu_ttm.c`: GTT write-combined policy (0011) and the visible-window
  check (0012);
- `amdgpu_gem.c`: keep user-space VRAM BOs in GTT on mmap/fault (0012).

The reset-magic and UVD message changes (0014) are small edits inside
`amdgpu_device.c` and `amdgpu_uvd.c`; only the patches carry them to
avoid mirroring two more large files.  See
`docs/amdgpu_bringup_zh.md` for the evidence and test results.
