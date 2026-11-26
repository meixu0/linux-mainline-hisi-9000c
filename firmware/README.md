# firmware/

No board-specific firmware blob is required for the 6.18 bring-up:

- UART, UFS and the onboard SATA (PCIe0) are driven directly by the kernel.
- PCIe1 is **not** usable as-is: its address block stays owned by the secure
  world until the kernel asks the platdrv FF-A partition to release it.  See
  `docs/pcie1_ffa_bar_zh.md`.
- amdgpu firmware (`amdgpu/*.bin`) is shipped by the distribution package
  `linux-firmware`; the installed rootfs on `sdd2` already contains
  `/lib/firmware/amdgpu` (699 files).
- The 6.18 test initramfs has no `/lib/firmware`, so the Oland files have to be
  copied in when building it: `oland_mc/smc/k_smc/rlc/pfp/uvd.bin` plus
  `oland_ce.bin` and `oland_me.bin` (the latter two are symlinks to
  `hainan_ce.bin`/`hainan_me.bin`, dereference them when copying).
- The GPU (Maleoon 910) and the vendor display path are not used by 6.18 yet.

If a future bring-up step needs a vendor blob (for example a PHY firmware for
PCIe1), it will be documented here with its origin and license.
