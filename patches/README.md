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

## Apply

```sh
git clone --depth 1 --branch v6.18 <linux.git> linux-6.18
cd linux-6.18
git am /path/to/patches/*.patch
```

The series has been verified with `git am` on a clean `v6.18` checkout: all
seven commits replay without conflicts.

## Notes

- Patch 0004 touches the mainline UFS core; keep it before 0005/0006.
- Patch 0007 depends on `CONFIG_PCIE_DW_HOST`, which it selects through its own
  Kconfig entry.
- Debug prints are deliberately `dev_dbg`; enable dynamic debug only when
  investigating.
