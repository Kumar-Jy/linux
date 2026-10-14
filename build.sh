#!/usr/bin/env bash
#
# Cross-build helper for the nabu (Xiaomi Pad 5) arm64 kernel.
#   ./build.sh [command]
#
# Works in place, or from a standalone copy anywhere via NABU_REPO:
#   NABU_REPO=/path/to/linux-nabu ~/build.sh [command]
#
# Commands:
#   defconfig   (re)generate out/.config from nabu_defconfig   [default if no .config]
#   build       compile kernel + modules + dtbs                 [default]
#   menuconfig  interactive config editor (needs libncurses-dev)
#   package     create dist/ tarball with Image, System.map, DTB and modules
#   clean       remove built objects (keeps .config)
#   distclean   remove the whole out/ directory
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Resolve the kernel tree: explicit override > script sits inside a repo > default
if [ -n "${NABU_REPO:-}" ]; then
  REPO="$NABU_REPO"
elif [ -f "$SCRIPT_DIR/Makefile" ]; then
  REPO="$SCRIPT_DIR"
else
  REPO="$HOME/GitHub/linux-nabu"
fi

if [[ ! -f "$REPO/Makefile" ]]; then
  echo "ERROR: kernel repo not found at $REPO (set NABU_REPO to override)" >&2
  exit 1
fi
cd "$REPO"

ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-
O=out
DEFCONFIG=nabu_defconfig
JOBS="$(nproc)"

cmd="${1:-auto}"

case "$cmd" in
  defconfig)
    make O="$O" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE "$DEFCONFIG"
    ;;
  menuconfig)
    make O="$O" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE menuconfig
    ;;
  build|auto)
    if [ ! -f "$O/.config" ]; then
      make O="$O" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE "$DEFCONFIG"
    fi
    make O="$O" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE -j"$JOBS" all
    # Device tree (DTC_FLAGS=-@ keeps symbols for overlay/fixup, as in CI)
    make O="$O" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE DTC_FLAGS="-@" dtbs
    ;;
  package)
    KREL="$(cat "$O/include/config/kernel.release")"
    STAGING="$O/pkgroot"
    DIST=dist
    rm -rf "$STAGING"
    mkdir -p "$DIST"

    # Install modules with the standard lib/modules layout
    make O="$O" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE \
         INSTALL_MOD_PATH="../$STAGING" modules_install

    # Kernel images + symbol map
    mkdir -p "$STAGING/boot"
    cp "$O/arch/arm64/boot/Image"     "$STAGING/boot/Image"
    cp "$O/arch/arm64/boot/Image.gz"  "$STAGING/boot/Image.gz"
    cp "$O/System.map"                "$STAGING/boot/System.map"

    # Device tree (same layout as the CI package: /boot/dtb-<kver>)
    DTB="$O/arch/arm64/boot/dts/qcom/sm8150-xiaomi-nabu.dtb"
    if [ -f "$DTB" ]; then
      cp "$DTB" "$STAGING/boot/dtb-$KREL"
      # convenient stable name, matching the CI package's dtb-linux-nabu symlink
      ln -sf "dtb-$KREL" "$STAGING/boot/dtb-linux-nabu"
    else
      echo "WARNING: $DTB not found - run './build.sh build' first" >&2
    fi

    TARBALL="$DIST/nabu-kernel-$KREL.tar.gz"
    tar -C "$STAGING" -czf "$TARBALL" .
    rm -rf "$STAGING"
    echo
    echo "Packaged: $TARBALL"
    ls -lh "$TARBALL"
    ;;
  clean)
    make O="$O" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE clean
    ;;
  distclean)
    rm -rf "$O"
    ;;
  *)
    echo "Usage: $0 [defconfig|build|menuconfig|package|clean|distclean]" >&2
    exit 1
    ;;
esac