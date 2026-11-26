# packaging/

The image/package pipeline of `linux-gaokun-buildbot` is not reproduced here
yet: the board still boots the vendor 5.10 rootfs and 6.18 is installed by hand
as a one-shot GRUB entry.

## Vendor reference kernel

A vendor 5.10 desktop kernel package was used privately as a reference during
the PCIe/amdgpu work.  It is **not** redistributed in this repository and its
filenames, configuration or contents are deliberately not recorded here; no
vendor kernel binary (image, modules, headers) is stored in this tree.

What the reference was used for, and what came out of it, is summarised in
`docs/pci_amdgpu_zh.md` and `docs/pcie1_ffa_bar_zh.md`: the vendor PCIe stack
reaches the secure world through an FF-A secure partition rather than a
platform SMC, and the vendor `power_misc` driver enables the PMIC rails
`pcieSwitch` (LDO16, 2.5 V) and `pcie2sata` (LDO41, 1.1 V), which the 6.18
kernel currently does not.
