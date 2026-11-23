# firmware/

No board-specific firmware blob is required for the 6.18 bring-up:

- UART, UFS, PCIe and the onboard SATA are driven directly by the kernel
  (the SoC firmware/UEFI leaves the secure-world state usable).
- amdgpu firmware (`amdgpu/*.bin`) is shipped by the distribution package
  `linux-firmware`; the GXDE OS 25 rootfs on `sdd2` already contains
  `/lib/firmware/amdgpu` (699 files).
- The GPU (Maleoon 910) and the vendor display path are not used by 6.18 yet.

If a future bring-up step needs a vendor blob (for example a PHY firmware for
PCIe1), it will be documented here with its origin and license.
