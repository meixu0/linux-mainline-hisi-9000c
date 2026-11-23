#!/usr/bin/env bash
# Non-interactive build of the charlotte (Kirin 9000C) 6.18 kernel.
#
# Environment:
#   KERNEL_TAG      upstream tag to build          (default: v6.18)
#   KERN_SRC        kernel source directory        (default: ./linux-6.18)
#   OUT             artifact output directory      (default: ./charlotte-out)
#   ARCH            kernel arch                    (default: arm64)
#   CROSS_COMPILE   toolchain prefix               (default: empty, native)
#   JOBS            parallel build jobs            (default: nproc)
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
KERNEL_TAG="${KERNEL_TAG:-v6.18}"
KERN_SRC="${KERN_SRC:-$PWD/linux-6.18}"
OUT="${OUT:-$PWD/charlotte-out}"
ARCH="${ARCH:-arm64}"
CROSS_COMPILE="${CROSS_COMPILE:-}"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$OUT"

if [[ ! -d "$KERN_SRC" ]]; then
  echo "==> cloning Linux $KERNEL_TAG into $KERN_SRC"
  git clone --depth 1 --branch "$KERNEL_TAG" \
    https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git "$KERN_SRC"
fi

cd "$KERN_SRC"

if ! git log --oneline -1 --grep "charlotte PCIe host support" | grep -q .; then
  echo "==> applying $REPO_DIR/patches"
  git config user.name  "hisi-9000c buildbot"
  git config user.email "buildbot@localhost"
  git am "$REPO_DIR"/patches/*.patch
else
  echo "==> patches already applied"
fi

echo "==> configuring"
mkdir -p arch/arm64/configs
cp "$REPO_DIR/defconfig/charlotte_6.18_defconfig" arch/arm64/configs/charlotte_defconfig
make ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" charlotte_defconfig

echo "==> building (jobs=$JOBS)"
make ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS" Image dtbs modules

echo "==> collecting artifacts into $OUT"
cp arch/arm64/boot/Image "$OUT/Image"
cp arch/arm64/boot/dts/hisilicon/charlotte-9000c.dtb "$OUT/charlotte-9000c.dtb"
cp System.map "$OUT/System.map-6.18"
make ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
     INSTALL_MOD_PATH="$OUT/modules" modules_install

echo "done:"
ls -l "$OUT"/Image "$OUT"/charlotte-9000c.dtb
