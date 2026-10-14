#!/bin/sh
# Static musl build of vtesse-cli and of the patched vtund server for one target.
#
#   ci/build-musl.sh <target> [outdir]
#
# target: arm32   OpenIPC toolchain (hisilicon-hi3516cv100, arm musleabi) — 32-bit ARM cameras
#         mipsel  OpenIPC toolchain (ingenic-t31, mipsel musl) — Ingenic cameras
#         arm64 | mips | x86_64 | armv5 | armv7   Bootlin musl toolchains
#         (mips = big-endian MIPS32)
# Produces <outdir>/vtesse-<target> (lwIP + TAP) and <outdir>/vtesse-tap-<target>
# (TAP only, LWIP=0), stripped, and <outdir>/vtund-<vtun version>-<target>: the VTun
# server with our patches (server/build-vtund.sh). With QEMU=1 the unit tests are run under
# qemu-user as well. musl is used because a static glibc binary cannot resolve
# host names without the NSS libraries of the same glibc on the device.
set -eu

TARGET=$1
OUT=${2:-dist}
BOOTLIN_VERSION=${BOOTLIN_VERSION:-stable-2026.08-1}
CACHE=${TOOLCHAIN_CACHE:-$HOME/.cache/vtesse-toolchains}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

OPENIPC=https://github.com/OpenIPC/firmware/releases/download/toolchain
BOOTLIN=https://toolchains.bootlin.com/downloads/releases/toolchains
case "$TARGET" in
    arm32)   URL=$OPENIPC/toolchain.hisilicon-hi3516cv100.tgz; DIRNAME=arm-openipc-linux-musleabi_sdk-buildroot; QEMU_BIN=qemu-arm ;;
    mipsel)  URL=$OPENIPC/toolchain.ingenic-t31.tgz;           DIRNAME=mipsel-openipc-linux-musl_sdk-buildroot; QEMU_BIN=qemu-mipsel ;;
    *)
        case "$TARGET" in
            x86_64) TC=x86-64;       QEMU_BIN=qemu-x86_64 ;;
            armv5)  TC=armv5-eabi;   QEMU_BIN=qemu-arm ;;
            armv7)  TC=armv7-eabihf; QEMU_BIN=qemu-arm ;;
            arm64)  TC=aarch64;      QEMU_BIN=qemu-aarch64 ;;
            mips)   TC=mips32;       QEMU_BIN=qemu-mips ;;
            *) echo "unknown target: $TARGET" >&2; exit 2 ;;
        esac
        DIRNAME=$TC--musl--$BOOTLIN_VERSION
        URL=$BOOTLIN/$TC/tarballs/$DIRNAME.tar.xz ;;
esac

DIR=$CACHE/$DIRNAME
if [ ! -d "$DIR/bin" ]; then
    mkdir -p "$CACHE"
    echo "downloading $URL"
    # download first (with retries), then extract: a truncated stream piped
    # into tar looks like a corrupt archive
    curl -fsSL --retry 3 -o "$CACHE/toolchain.tar" "$URL"
    tar -xf "$CACHE/toolchain.tar" -C "$CACHE"
    rm -f "$CACHE/toolchain.tar"
    [ -x "$DIR/relocate-sdk.sh" ] && "$DIR/relocate-sdk.sh" >/dev/null 2>&1 || true
fi
GCC=$(ls "$DIR"/bin/*-linux-musl*-gcc | head -1)
CROSS=${GCC%gcc}

mkdir -p "$OUT"
cd "$ROOT"
for variant in full tap; do
    if [ $variant = full ]; then lwip=1; bin=vtesse-$TARGET; else lwip=0; bin=vtesse-tap-$TARGET; fi
    build=build-$TARGET-$variant
    make -s BUILD="$build" CROSS_COMPILE="$CROSS" STATIC=1 LWIP=$lwip CFLAGS=-Os all tests
    "${CROSS}strip" --strip-unneeded -o "$OUT/$bin" "$build/vtesse-cli"
    echo "$bin: $(wc -c < "$OUT/$bin") bytes"
done

VTUND=vtund-$("$ROOT/server/build-vtund.sh" --version)-$TARGET
"$ROOT/server/build-vtund.sh" --cross "$CROSS" --static "$OUT/$VTUND"

if [ "${QEMU:-0}" = 1 ]; then
    for t in test_proto test_route test_client; do
        echo "== $QEMU_BIN $t"
        "$QEMU_BIN" "build-$TARGET-full/$t" | tail -1
    done
    "$QEMU_BIN" "$OUT/vtesse-$TARGET" --version
    "$QEMU_BIN" "$OUT/vtesse-tap-$TARGET" --version
    # vtund has no --version; -h prints "VTun ver ..." and the usage, exit code 1
    "$QEMU_BIN" "$OUT/$VTUND" -h 2>&1 | grep "^VTun ver"
fi
