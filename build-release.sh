#!/bin/sh
# Build static dimg-tool binaries for release.
#
# Usage:
#   ./build-release.sh [version] [arch...]
#
# Examples:
#   ./build-release.sh v0.2.0              # all architectures
#   ./build-release.sh v0.2.0 x86_64       # x86_64 only
#   ./build-release.sh v0.2.0 arm riscv64  # ARM + RISC-V only
#
# Requires musl-cross-make toolchains in MUSL_ROOT.

set -eu

VERSION="${1:-v0.2.0}"
shift || true

MUSL_ROOT="${MUSL_ROOT:-$(dirname "$(command -v x86_64-linux-musl-gcc 2>/dev/null || echo /usr/local/musl/bin/x86_64-linux-musl-gcc)")}"
LIBAARU="../libaaruformat"
DIST="$(cd "$(dirname "$0")" && pwd)/dist"

mkdir -p "$DIST"

# Architecture definitions: name, musl triple, cmake system processor
ARCHS="
x86_64:x86_64-linux-musl:x86_64
arm:arm-linux-musleabihf:arm
riscv64:riscv64-linux-musl:riscv64
"

# Filter to requested architectures (default: all)
requested="${*:-x86_64 arm riscv64}"

build_libaaruformat() {
    arch_name="$1"
    triple="$2"
    processor="$3"
    builddir="$LIBAARU/build-musl-${arch_name}"

    if [ -f "$builddir/libaaruformat.a" ]; then
        return
    fi

    echo "=== Building libaaruformat (${arch_name}) ==="
    mkdir -p "$builddir"
    cd "$builddir"

    # Cross-compilation: set CMAKE_CROSSCOMPILING so libaaruformat
    # skips its native arch flags. We provide our own via CMAKE_C_FLAGS.
    extra_flags=""
    case "$arch_name" in
        arm)
            # ARM with NEON for SIMD (CRC64, checksums)
            extra_flags="-march=armv7-a+fp -mfpu=neon -mfloat-abi=hard"
            ;;
        riscv64)
            extra_flags="-march=rv64gc -mabi=lp64d"
            ;;
    esac

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
    libdir="$LIBAARU/build-musl-${arch_name}"
    incdir="$LIBAARU/include"
    inc3p="$LIBAARU/3rdparty"
    binary="dimg-tool-${VERSION}-linux-${arch_name}"

    # Match arch flags used for libaaruformat
    extra_flags=""
    case "$arch_name" in
        arm)    extra_flags="-march=armv7-a+fp -mfpu=neon -mfloat-abi=hard" ;;
        riscv64) extra_flags="-march=rv64gc -mabi=lp64d" ;;
    esac

    echo "=== Building dimg-tool ${VERSION} (${arch_name}) ==="
    # shellcheck disable=SC2086
    "${MUSL_ROOT}/${triple}-gcc" -O2 -static -Wno-unknown-pragmas $extra_flags \
        -o "$DIST/$binary" \
        src/main.c src/cmd_info.c src/cmd_convert.c src/cmd_verify.c \
        src/disc.c src/fmt_aaru.c src/fmt_cue.c src/fmt_iso.c src/fmt_sbi.c \
        -Iinclude -I"$incdir" -I"$inc3p/BLAKE3/c" -I"$inc3p/uthash/src" \
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
for entry in $ARCHS; do
    arch_name="${entry%%:*}"
    rest="${entry#*:}"
    triple="${rest%%:*}"
    processor="${rest#*:}"

    case " $requested " in
        *" $arch_name "*)
            build_libaaruformat "$arch_name" "$triple" "$processor"
            build_dimg_tool "$arch_name" "$triple"
            ;;
    esac
done

echo ""
echo "=== Release artifacts ==="
ls -lh "$DIST"/dimg-tool-"${VERSION}"-* 2>/dev/null
echo ""
echo "Checksums:"
cat "$DIST"/dimg-tool-"${VERSION}"-*.sha256 2>/dev/null
