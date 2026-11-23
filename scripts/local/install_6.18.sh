#!/usr/bin/env bash
# Install a freshly built charlotte 6.18 kernel as a one-shot GRUB test entry.
#
# This never touches the 5.10 kernel, its rootfs or the GRUB default entry.
#
# Usage:
#   install_6.18.sh --image Image --dtb charlotte-9000c.dtb [--initrd cpio.gz]
#                   [--write-grub] [--boot]
set -euo pipefail

BOOT_DIR="/boot/linux-6.18-charlotte"
ENTRY_TITLE="Linux 6.18 charlotte bring-up (TEST - uart console)"
IMAGE=""
DTB=""
INITRD=""
WRITE_GRUB=0
BOOT_NOW=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --image) IMAGE="$2"; shift 2 ;;
    --dtb) DTB="$2"; shift 2 ;;
    --initrd) INITRD="$2"; shift 2 ;;
    --write-grub) WRITE_GRUB=1; shift ;;
    --boot) BOOT_NOW=1; shift ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

[[ -n "$IMAGE" && -n "$DTB" ]] || { echo "--image and --dtb are required" >&2; exit 1; }

sudo mkdir -p "$BOOT_DIR"
if [[ -f "$BOOT_DIR/Image" ]]; then
  sudo cp -n "$BOOT_DIR/Image" "$BOOT_DIR/Image.previous" || true
fi
sudo cp "$IMAGE" "$BOOT_DIR/Image"
sudo cp "$DTB" "$BOOT_DIR/charlotte-9000c.dtb"
if [[ -n "$INITRD" ]]; then
  sudo cp "$INITRD" "$BOOT_DIR/initramfs-console.cpio.gz"
fi
sudo sync

echo "installed:"
ls -l "$BOOT_DIR"

if [[ "$WRITE_GRUB" == 1 ]]; then
  if ! sudo grep -qF "$ENTRY_TITLE" /etc/grub.d/40_custom 2>/dev/null; then
    sudo tee -a /etc/grub.d/40_custom >/dev/null <<EOF

menuentry "$ENTRY_TITLE" {
    linux  /linux-6.18-charlotte/Image earlycon=uart_v500,0xff894000 console=ttyHW0,115200n8 loglevel=7 arm64.nopauth panic=20
    devicetree /linux-6.18-charlotte/charlotte-9000c.dtb
    initrd /linux-6.18-charlotte/initramfs-console.cpio.gz
}
EOF
    sudo grub-mkconfig -o /boot/grub/grub.cfg
  else
    echo "GRUB entry already present, not touching grub.cfg"
  fi
fi

if [[ "$BOOT_NOW" == 1 ]]; then
  sudo grub-reboot "$ENTRY_TITLE"
  sudo systemctl reboot
fi
