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
# Profile-guided optimisation (docs/DESIGN.md, 2026-09-19):
#   PGO=gen            instrumented binary; run it on the device with
#                      LLVM_PROFILE_FILE=/media/fat/bennugd/bennugd.profraw and
#                      let it exit normally (the game's exit chord or SIGTERM)
#   PGO=use FILE       optimise with the merged profile (llvm-profdata merge)
# Each mode gets its own build tree so the flags cannot leak into a normal build.
#   (default)          use hps/pgo/bennugd.profdata when it exists (PGO=off to skip)
PGO_FLAGS=
if [ -z "${PGO:-}" ] && [ -f "$HERE/pgo/bennugd.profdata" ]; then
    PGO=use; PGO_PROFILE="$HERE/pgo/bennugd.profdata"
fi
case "${PGO:-}" in
    gen) PGO_FLAGS="-fprofile-instr-generate"; BUILD_DIR="$BUILD_DIR-pgo-gen"; PGO_CMAKE="-DPGO_GEN=ON" ;;
    use) [ -f "${PGO_PROFILE:?PGO=use needs PGO_PROFILE=<file.profdata>}" ] || exit 2
         PGO_FLAGS="-fprofile-instr-use=$PGO_PROFILE -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
         [ "${PGO_PROFILE}" = "$HERE/pgo/bennugd.profdata" ] || BUILD_DIR="$BUILD_DIR-pgo-use" ;;
esac
# through the environment: -DCMAKE_C_FLAGS would replace the toolchain's -target
CFLAGS="${CFLAGS:-} $PGO_FLAGS" LDFLAGS="${LDFLAGS:-} $PGO_FLAGS" \
cmake -B "$BUILD_DIR" -S "$HERE" \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/zig.toolchain.arm-linux-gnueabihf-a9" \
    -DCMAKE_BUILD_TYPE=Release ${PGO_CMAKE:-}
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
mkdir -p "$HERE/out"
cp "$BUILD_DIR/bennugd" "$HERE/out/bennugd"
file "$HERE/out/bennugd"
