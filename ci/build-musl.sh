#!/bin/sh
# Static musl build of vtesse-cli for one target (and of the patched vtund server for x86_64).
#
#   ci/build-musl.sh <target> [outdir]
#
# target: arm32   OpenIPC toolchain (hisilicon-hi3516cv100, arm musleabi) — 32-bit ARM cameras
#         mipsel | mips   OpenWrt toolchains (ramips/mt7621, ath79/generic; musl, soft-float):
#                 run on every MIPS32r2 core with or without an FPU — Ingenic cameras and
#                 MediaTek/Atheros routers (MT7620/7621/7628, ath79), whose kernels often
#                 lack FPU emulation, so hard-float code dies with SIGILL (#30)
#         arm64 | x86_64 | armv5 | armv7   Bootlin musl toolchains
# Produces <outdir>/vtesse-<target> (lwIP and TAP backends, --tap), stripped; a TAP-only
# build stays a build option (make LWIP=0) and is not published. For x86_64 also <outdir>/vtund-<vtun version>-x86_64:
# the VTun server with our patches (server/build-vtund.sh), a PC/server binary only;
# VTUND=1 builds it for another target too, VTUND=0 skips it. With QEMU=1 the unit tests are run under
# qemu-user as well. musl is used because a static glibc binary cannot resolve
# host names without the NSS libraries of the same glibc on the device.
set -eu

TARGET=$1
OUT=${2:-dist}
BOOTLIN_VERSION=${BOOTLIN_VERSION:-stable-2026.08-1}
# The archives are checked before their compiler is run: SHA256 pinned below for the
# default Bootlin and OpenWrt versions (TOOLCHAIN_SHA256 overrides it, required with
# another version); the OpenIPC toolchain is a rolling release asset, checked
# against the digest GitHub reports for it.
CACHE=${TOOLCHAIN_CACHE:-$HOME/.cache/vtesse-toolchains}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

OPENIPC=https://github.com/OpenIPC/firmware/releases/download/toolchain
BOOTLIN=https://toolchains.bootlin.com/downloads/releases/toolchains
OPENWRT_VERSION=${OPENWRT_VERSION:-23.05.5}
OPENWRT=https://downloads.openwrt.org/releases/$OPENWRT_VERSION/targets
SUBDIR=     # toolchain directory inside the archive, if not its top level
case "$TARGET" in
    arm32)   URL=$OPENIPC/toolchain.hisilicon-hi3516cv100.tgz; DIRNAME=arm-openipc-linux-musleabi_sdk-buildroot; QEMU_BIN=qemu-arm
             SHA=github ;;
    mipsel|mips)
        # OpenWrt toolchains: musl, soft-float (24kc), as on the routers themselves
        if [ "$TARGET" = mipsel ]; then OW=ramips/mt7621; OWA=mipsel_24kc; QEMU_BIN=qemu-mipsel
            SHA=bb08478a46869009a2a8b85f88e266cc6b6d3ccefc34de61907ead2a986e08c9
        else OW=ath79/generic; OWA=mips_24kc; QEMU_BIN=qemu-mips
            SHA=6cf197b2705307057548e65158396ca4d26b343f557bf81c97fd5643978cc991; fi
        [ "$OPENWRT_VERSION" = 23.05.5 ] || SHA=
        DIRNAME=openwrt-toolchain-$OPENWRT_VERSION-$(echo $OW | tr / -)_gcc-12.3.0_musl.Linux-x86_64
        URL=$OPENWRT/$OW/$DIRNAME.tar.xz
        SUBDIR=/toolchain-${OWA}_gcc-12.3.0_musl ;;
    *)
        case "$TARGET" in
            x86_64) TC=x86-64;       QEMU_BIN=qemu-x86_64
                    SHA=78d3a4683d6ac47b5ee73bd5bce210b55eb93dff1b137c61298af97eb0d2b5a6 ;;
            armv5)  TC=armv5-eabi;   QEMU_BIN=qemu-arm
                    SHA=a7db571eaa0f1ba2eea9a3932e8ad64bfaf33bfb7e7b96dd09702fe3ecedcf1a ;;
            armv7)  TC=armv7-eabihf; QEMU_BIN=qemu-arm
                    SHA=9147bafae4aa272321a3c6440d04d83b7e23411b2d344f875541d84c4444ba9b ;;
            arm64)  TC=aarch64;      QEMU_BIN=qemu-aarch64
                    SHA=b388c480a48e8e9f9b99e3d14e69219c4d61e5a2424a82faecb88a015b781a60 ;;
            *) echo "unknown target: $TARGET" >&2; exit 2 ;;
        esac
        DIRNAME=$TC--musl--$BOOTLIN_VERSION
        [ "$BOOTLIN_VERSION" = stable-2026.08-1 ] || SHA=
        URL=$BOOTLIN/$TC/tarballs/$DIRNAME.tar.xz ;;
esac

DIR=$CACHE/$DIRNAME$SUBDIR
if [ ! -d "$DIR/bin" ]; then
    mkdir -p "$CACHE"
    echo "downloading $URL"
    # download first (with retries), then extract: a truncated stream piped
    # into tar looks like a corrupt archive
    curl -fsSL --retry 3 -o "$CACHE/toolchain.tar" "$URL"
    SHA=${TOOLCHAIN_SHA256:-$SHA}
    if [ "$SHA" = github ]; then
        # authenticated when a token is available: anonymous calls share a 60/hour limit per IP
        command -v jq >/dev/null ||
            { echo "jq is needed to verify $URL (or set TOOLCHAIN_SHA256)" >&2; rm -f "$CACHE/toolchain.tar"; exit 1; }
        TOKEN=${GITHUB_TOKEN:-${GH_TOKEN:-}}
        REL=$(curl -fsSL --retry 3 ${TOKEN:+-H "Authorization: Bearer $TOKEN"} \
              "https://api.github.com/repos/OpenIPC/firmware/releases/tags/toolchain") ||
            { echo "GitHub API request for the toolchain digest failed (rate limit?)" >&2; rm -f "$CACHE/toolchain.tar"; exit 1; }
        SHA=$(printf '%s' "$REL" | jq -r --arg n "${URL##*/}" '.assets[] | select(.name == $n) | .digest // "" | sub("^sha256:"; "")')
    fi
    [ -n "$SHA" ] || { echo "no SHA256 for $URL: set TOOLCHAIN_SHA256" >&2; rm -f "$CACHE/toolchain.tar"; exit 1; }
    echo "$SHA  $CACHE/toolchain.tar" | sha256sum -c - >/dev/null ||
        { echo "SHA256 mismatch for $URL" >&2; rm -f "$CACHE/toolchain.tar"; exit 1; }
    tar -xf "$CACHE/toolchain.tar" -C "$CACHE"
    rm -f "$CACHE/toolchain.tar"
    [ -x "$DIR/relocate-sdk.sh" ] && "$DIR/relocate-sdk.sh" >/dev/null 2>&1 || true
fi
GCC=$(ls "$DIR"/bin/*-linux-musl*-gcc | head -1)
export STAGING_DIR="$DIR"     # OpenWrt's gcc wrapper warns without it
CROSS=${GCC%gcc}

# lwIP memory profile (#12): medium for cameras and routers (32-bit ARM, MIPS),
# large for arm64 boards and PCs
case "$TARGET" in
    arm64|x86_64) PROFILE=${LWIP_PROFILE:-large} ;;
    *)            PROFILE=${LWIP_PROFILE:-medium} ;;
esac

mkdir -p "$OUT"
cd "$ROOT"
build=build-$TARGET-full
bin=vtesse-$TARGET
rm -rf "$build"     # objects of another toolchain or profile are never rebuilt by make
make -s BUILD="$build" CROSS_COMPILE="$CROSS" STATIC=1 LWIP=1 LWIP_PROFILE=$PROFILE CFLAGS=-Os all tests
"${CROSS}strip" --strip-unneeded -o "$OUT/$bin" "$build/vtesse-cli"
echo "$bin: $(wc -c < "$OUT/$bin") bytes (lwIP profile $PROFILE)"

# vtund: PC/server only by default; for cameras and routers it is built in the OpenIPC
# environment when needed (server/README.md)
[ "$TARGET" = x86_64 ] && VTUND_DEFAULT=1 || VTUND_DEFAULT=0
VTUND_BIN=
if [ "${VTUND:-$VTUND_DEFAULT}" = 1 ]; then
    VTUND_BIN=vtund-$("$ROOT/server/build-vtund.sh" --version)-$TARGET
    "$ROOT/server/build-vtund.sh" --cross "$CROSS" --static "$OUT/$VTUND_BIN"
fi

if [ "${QEMU:-0}" = 1 ]; then
    for t in test_proto test_route test_client test_env; do
        echo "== $QEMU_BIN $t"
        # no pipefail in sh: keep the test's own exit status, not that of tail
        out=$("$QEMU_BIN" "build-$TARGET-full/$t" 2>&1) || { echo "$out"; echo "$t failed under $QEMU_BIN" >&2; exit 1; }
        echo "$out" | tail -1
    done
    "$QEMU_BIN" "$OUT/vtesse-$TARGET" --version
    # vtund has no --version; -h prints "VTun ver ..." and the usage, exit code 1
    [ -z "$VTUND_BIN" ] || "$QEMU_BIN" "$OUT/$VTUND_BIN" -h 2>&1 | grep "^VTun ver"
fi
