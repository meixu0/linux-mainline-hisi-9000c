#!/usr/bin/env bash
# Interactive, on-device build helper for the charlotte 6.18 kernel.
#
# It only prepares the host, then hands over to scripts/ci/build_kernel.sh so
# both paths stay in sync.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
KERNEL_TAG="${KERNEL_TAG:-v6.18}"
KERN_SRC="${KERN_SRC:-$HOME/charlotte/linux-6.18}"
OUT="${OUT:-$HOME/charlotte/out}"

DISTRO="unknown"
if [[ -f /etc/os-release ]]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  DISTRO="${ID:-unknown}"
fi

echo "repo:      $REPO_DIR"
echo "kernel:    $KERNEL_TAG -> $KERN_SRC"
echo "output:    $OUT"
echo "distro:    $DISTRO"

read -r -p "Install build dependencies? [y/N] " answer
answer="${answer:-n}"
if [[ "$answer" =~ ^([yY][eE][sS]|[yY])$ ]]; then
  case "$DISTRO" in
    ubuntu|debian)
      sudo apt-get update
      sudo apt-get install -y build-essential bison flex bc libssl-dev \
        libelf-dev device-tree-compiler cpio kmod git
      ;;
    fedora)
      sudo dnf install -y gcc make bison flex bc openssl-devel elfutils-libelf-devel \
        dtc cpio kmod git
      ;;
    *)
      echo "unknown distro '$DISTRO'; install gcc/make/bison/flex/bc/openssl/elfutils/dtc/cpio yourself"
      ;;
  esac
fi

exec env KERNEL_TAG="$KERNEL_TAG" KERN_SRC="$KERN_SRC" OUT="$OUT" \
  "$REPO_DIR/scripts/ci/build_kernel.sh"
