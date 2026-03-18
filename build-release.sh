#!/bin/sh
set -eu

VERSION="${1:-v0.1.0}"
MUSL="MUSL_ROOT_PLACEHOLDER/x86_64-linux-musl"
LIBAARU="../libaaruformat"
DIST="$(dirname "$0")/dist"

# Build libaaruformat static library if needed
if [ ! -f "$LIBAARU/build-musl/libaaruformat.a" ]; then
    echo "Building libaaruformat (static, musl)..."
    mkdir -p "$LIBAARU/build-musl"
    cd "$LIBAARU/build-musl"
    cmake .. \
        -DCMAKE_C_COMPILER="$MUSL-gcc" \
        -DCMAKE_AR="$MUSL-gcc-ar" \
        -DCMAKE_RANLIB="$MUSL-gcc-ranlib" \
        -DCMAKE_BUILD_TYPE=Release \
        -DAARU_BUILD_PACKAGE=ON \
        -DBUILD_SHARED_LIBS=OFF
    make -j"$(nproc)"
    cd -
fi

LIBDIR="$LIBAARU/build-musl"
INCDIR="$LIBAARU/include"
INC3P="$LIBAARU/3rdparty"
BINARY="dimg-tool-${VERSION}-linux-x86_64"

echo "Building dimg-tool ${VERSION}..."
"$MUSL-gcc" -O2 -static -Wno-unknown-pragmas \
    -o "$DIST/$BINARY" \
    src/main.c src/cmd_info.c src/cmd_convert.c src/cmd_verify.c \
    src/disc.c src/fmt_aaru.c src/fmt_cue.c src/fmt_iso.c src/fmt_sbi.c \
    -Iinclude -I"$INCDIR" -I"$INC3P/BLAKE3/c" -I"$INC3P/uthash/src" \
    "$LIBDIR/libaaruformat.a" \
    "$LIBDIR/libzstd_static.a" \
    "$LIBDIR/libblake3.a" \
    "$LIBDIR/libxxhash.a" \
    -lm -lpthread

"$MUSL-strip" "$DIST/$BINARY"

cd "$DIST"
tar czf "$BINARY.tar.gz" "$BINARY"
sha256sum "$BINARY.tar.gz" > "$BINARY.tar.gz.sha256"

echo ""
echo "Release artifacts:"
ls -lh "$BINARY" "$BINARY.tar.gz" "$BINARY.tar.gz.sha256"
echo ""
cat "$BINARY.tar.gz.sha256"
