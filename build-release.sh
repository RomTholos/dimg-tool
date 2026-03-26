#!/bin/sh
# Build static dimg-tool binaries for release.
#
# Usage:
#   ./build-release.sh [--clean] [version] [arch...]
#
# Examples:
#   ./build-release.sh v0.3.1              # all architectures
#   ./build-release.sh v0.3.1 x86_64       # x86_64 only
#   ./build-release.sh v0.3.1 arm riscv64  # ARM + RISC-V only
#   ./build-release.sh --clean v0.3.1      # rebuild libaaruformat from scratch
#
# Requires musl-cross-make toolchains in MUSL_ROOT.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Parse --clean flag
CLEAN=0
if [ "${1:-}" = "--clean" ]; then
    CLEAN=1
    shift
fi

VERSION="${1:-v0.3.3}"
shift || true

MUSL_ROOT="${MUSL_ROOT:-$(dirname "$(command -v x86_64-linux-musl-gcc 2>/dev/null || echo /usr/local/musl/bin/x86_64-linux-musl-gcc)")}"
LIBAARU="${SCRIPT_DIR}/../libaaruformat"
DIST="${SCRIPT_DIR}/dist"

mkdir -p "$DIST"

ALL_ARCHS="x86_64 arm aarch64 riscv64"

# Filter to requested architectures (default: all)
requested="${*:-$ALL_ARCHS}"

# Architecture config: triple and extra C flags per arch
arch_triple() {
    case "$1" in
        x86_64)  echo "x86_64-linux-musl" ;;
        arm)     echo "arm-linux-musleabihf" ;;
        aarch64) echo "aarch64-linux-musl" ;;
        riscv64) echo "riscv64-linux-musl" ;;
    esac
}

arch_processor() {
    case "$1" in
        x86_64)  echo "x86_64" ;;
        arm)     echo "arm" ;;
        aarch64) echo "aarch64" ;;
        riscv64) echo "riscv64" ;;
    esac
}

arch_flags() {
    case "$1" in
        arm)     echo "-march=armv7-a+fp -mfpu=neon -mfloat-abi=hard" ;;
        riscv64) echo "-march=rv64gc -mabi=lp64d" ;;
        *)       echo "" ;;
    esac
}

build_libaaruformat() {
    arch_name="$1"
    triple="$2"
    processor="$3"
    extra_flags="$4"
    builddir="$LIBAARU/build-musl-${arch_name}"

    if [ "$CLEAN" = 1 ] && [ -d "$builddir" ]; then
        echo "=== Cleaning libaaruformat (${arch_name}) ==="
        rm -rf "$builddir"
    fi

    if [ -f "$builddir/libaaruformat.a" ]; then
        echo "=== libaaruformat (${arch_name}) up to date ==="
        return
    fi

    echo "=== Building libaaruformat (${arch_name}) ==="
    mkdir -p "$builddir"
    cd "$builddir"

    cmake .. \
        -DCMAKE_C_COMPILER="${MUSL_ROOT}/${triple}-gcc" \
        -DCMAKE_AR="${MUSL_ROOT}/${triple}-gcc-ar" \
        -DCMAKE_RANLIB="${MUSL_ROOT}/${triple}-gcc-ranlib" \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR="${processor}" \
        -DCMAKE_CROSSCOMPILING=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="${extra_flags}" \
        -DAARU_BUILD_PACKAGE=ON \
        -DBUILD_SHARED_LIBS=OFF
    make -j"$(nproc)"
    cd - > /dev/null
}

build_dimg_tool() {
    arch_name="$1"
    triple="$2"
    extra_flags="$3"
    libdir="$LIBAARU/build-musl-${arch_name}"
    incdir="$LIBAARU/include"
    inc3p="$LIBAARU/3rdparty"
    binary="dimg-tool-${VERSION}-linux-${arch_name}"

    echo "=== Building dimg-tool ${VERSION} (${arch_name}) ==="
    # shellcheck disable=SC2086
    "${MUSL_ROOT}/${triple}-gcc" -O2 -static -Wno-unknown-pragmas $extra_flags \
        -o "$DIST/$binary" \
        "${SCRIPT_DIR}/src/main.c" \
        "${SCRIPT_DIR}/src/cmd_info.c" \
        "${SCRIPT_DIR}/src/cmd_convert.c" \
        "${SCRIPT_DIR}/src/cmd_verify.c" \
        "${SCRIPT_DIR}/src/disc.c" \
        "${SCRIPT_DIR}/src/fmt_aaru.c" \
        "${SCRIPT_DIR}/src/fmt_cue.c" \
        "${SCRIPT_DIR}/src/fmt_iso.c" \
        "${SCRIPT_DIR}/src/fmt_sbi.c" \
        -I"${SCRIPT_DIR}/include" -I"$incdir" -I"$inc3p/BLAKE3/c" -I"$inc3p/uthash/src" \
        "$libdir/libaaruformat.a" \
        "$libdir/libzstd_static.a" \
        "$libdir/libblake3.a" \
        "$libdir/libxxhash.a" \
        -lm -lpthread

    "${MUSL_ROOT}/${triple}-strip" "$DIST/$binary"

    cd "$DIST"
    tar czf "$binary.tar.gz" "$binary"
    sha256sum "$binary.tar.gz" > "$binary.tar.gz.sha256"
    cd - > /dev/null

    echo "  $(ls -lh "$DIST/$binary" | awk '{print $5}') $binary"
}

# Build each requested architecture
for arch_name in $ALL_ARCHS; do
    case " $requested " in
        *" $arch_name "*)
            triple="$(arch_triple "$arch_name")"
            processor="$(arch_processor "$arch_name")"
            extra_flags="$(arch_flags "$arch_name")"

            if ! command -v "${MUSL_ROOT}/${triple}-gcc" > /dev/null 2>&1; then
                echo "=== Skipping ${arch_name}: ${triple}-gcc not found ==="
                continue
            fi
            build_libaaruformat "$arch_name" "$triple" "$processor" "$extra_flags"
            build_dimg_tool "$arch_name" "$triple" "$extra_flags"
            ;;
    esac
done

echo ""
echo "=== Release artifacts ==="
ls -lh "$DIST"/dimg-tool-"${VERSION}"-* 2>/dev/null
echo ""
echo "Checksums:"
cat "$DIST"/dimg-tool-"${VERSION}"-*.sha256 2>/dev/null
