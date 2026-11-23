# drivers/

Local mirrors of the driver sources added or modified by `patches/`. They exist
so the code can be read and diffed without unpacking the patch series; the
patches remain the authoritative form.

| Path | Kernel location | Patch |
|---|---|---|
| `tty/uart-v500.c` | `drivers/tty/serial/uart-v500.c` | 0002 |
| `tty/uart-v500-earlycon.c` | `drivers/tty/serial/uart-v500-earlycon.c` | 0002 |
| `ufs/ufs-charlotte.c` | `drivers/ufs/host/ufs-charlotte.c` | 0005 |
| `pci/pcie-charlotte.c` | `drivers/pci/controller/dwc/pcie-charlotte.c` | 0007 |
| `misc/dss-power-probe.c` | `drivers/misc/dss-power-probe.c` | 0003 |

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
