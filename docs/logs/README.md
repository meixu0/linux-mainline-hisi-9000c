# docs/logs/

Captured serial logs used as evidence in the bring-up notes. They are kept
verbatim (CR characters normalised to LF) so register values and timings can be
re-checked.

| Log | What it shows |
|---|---|
| `6.18-ufs-first-success.log` | first 6.18 boot with working UFS: `sda`..`sdd`, 43 partitions, rootfs readable |
| `6.18-ufs-hs-g4.log` | UFS after the HS-G4 fix: `gear=4 lanes=2 hs_rate=2`, ~931 MB/s read |
| `6.18-pcie-link-up.log` | PCIe0 Gen3 x1 link up, ASM1064 SATA enumerated, `ahci` bound |
| `6.18-sdd2-chroot-final.log` | initramfs `sdd2-chroot` into GXDE OS 25 and `uname -r` inside |

Capture command (the port is single-reader):

```sh
pkill -f 'cat /dev/ttyUSB0' || true
sg dialout -c 'stty -F /dev/ttyUSB0 115200 raw -echo -crtscts -ixon -ixoff;
               timeout 900 cat /dev/ttyUSB0' > log.txt
```
