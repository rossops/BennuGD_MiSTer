#!/bin/sh
# Cross-compile the MiSTer frontend plus the BennuGD_libretro core for the
# DE10-Nano HPS (armv7 hard float). Output: hps/out/bennugd
#   ZIG_DIR    zig 0.14.0 install (default ~/.local/opt/zig-macos-aarch64-0.14.0)
#   BUILD_DIR  cmake tree; keep it on local disk, the SMB share is far too slow
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ZIG_DIR=${ZIG_DIR:-$HOME/.local/opt/zig-macos-aarch64-0.14.0}
BUILD_DIR=${BUILD_DIR:-$HOME/.cache/bennugd-mister/build-armhf}
export PATH="$ZIG_DIR:$PATH"
zig version >/dev/null
# Local changes to the upstream core live in hps/patches (see docs/DESIGN.md);
# apply the ones a fresh submodule checkout does not have yet.
for p in "$HERE"/patches/*.patch; do
    [ -e "$p" ] || continue
    if git -C "$HERE/BennuGD_libretro" apply --check --reverse "$p" >/dev/null 2>&1; then
        continue                                   # already applied
    fi
    git -C "$HERE/BennuGD_libretro" apply "$p" && echo "applied $(basename "$p")"
done
cmake -B "$BUILD_DIR" -S "$HERE" \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/zig.toolchain.arm-linux-gnueabihf-a9" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
mkdir -p "$HERE/out"
cp "$BUILD_DIR/bennugd" "$HERE/out/bennugd"
file "$HERE/out/bennugd"
