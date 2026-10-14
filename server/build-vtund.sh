#!/bin/sh
# Build the VTun server (vtund) with our patches. Every vtund build in this
# repository goes through this script: the bench image and the CI release builds.
#
#   server/build-vtund.sh [--cross PREFIX] [--static] OUTFILE
#
#   --cross PREFIX  cross compiler prefix, e.g. /opt/tc/bin/aarch64-linux-  (default: native cc)
#   --static        link statically
#
# Downloads vtun-$VTUN_VERSION.tar.gz (cached in $VTUN_CACHE, sha256 checked),
# applies every server/patches/vtun-$VTUN_VERSION-*.patch in name order (the build
# fails if there are none or one does not apply) and configures it like the target
# deployment: no OpenSSL (XOR challenge response), no LZO, no zlib.
# Needs bison and flex; for --cross also current config.sub/config.guess
# (autotools-dev or automake), since the 2001 ones in the tarball do not know
# aarch64 or musl.
#
#   server/build-vtund.sh --version   prints the VTun version this script builds
set -eu

VTUN_VERSION=3.0.2
VTUN_SHA256=db26d4dbc35a2b3d65519f68b8ae55c80d7ec4e6569aeec7a432aa82f24c56d6
VTUN_URL=http://prdownloads.sourceforge.net/vtun/vtun-$VTUN_VERSION.tar.gz
HERE=$(cd "$(dirname "$0")" && pwd)
CACHE=${VTUN_CACHE:-$HOME/.cache/vtesse-vtun}

CROSS= STATIC=0 OUT=
while [ $# -gt 0 ]; do
    case "$1" in
        --cross) CROSS=$2; shift ;;
        --static) STATIC=1 ;;
        --version) echo "$VTUN_VERSION"; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) OUT=$1 ;;
    esac
    shift
done
[ -n "$OUT" ] || { sed -n '5,10p' "$0"; exit 2; }
case "$OUT" in /*) ;; *) OUT=$(pwd)/$OUT ;; esac

PATCHES=$(ls "$HERE"/patches/vtun-"$VTUN_VERSION"-*.patch 2>/dev/null || true)
[ -n "$PATCHES" ] || { echo "no patches for vtun $VTUN_VERSION in $HERE/patches" >&2; exit 1; }

TARBALL=$CACHE/vtun-$VTUN_VERSION.tar.gz
if ! echo "$VTUN_SHA256  $TARBALL" | sha256sum -c - >/dev/null 2>&1; then
    mkdir -p "$CACHE"
    curl -fsSL --retry 3 -o "$TARBALL.tmp" "$VTUN_URL"
    echo "$VTUN_SHA256  $TARBALL.tmp" | sha256sum -c - >/dev/null \
        || { echo "sha256 mismatch for $VTUN_URL" >&2; rm -f "$TARBALL.tmp"; exit 1; }
    mv "$TARBALL.tmp" "$TARBALL"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
tar -xzf "$TARBALL" -C "$WORK"
SRC=$WORK/vtun-$VTUN_VERSION
cd "$SRC"
for p in $PATCHES; do
    echo "applying $(basename "$p")"
    patch -p1 --no-backup-if-mismatch --quiet < "$p"
done

BUILD_TRIPLE=$(uname -m)-pc-linux-gnu
set -- --build="$BUILD_TRIPLE" --disable-lzo --disable-zlib --disable-ssl --prefix=''
CC=${CROSS}gcc STRIP=${CROSS}strip
if [ -n "$CROSS" ]; then
    for f in config.sub config.guess; do
        new=$(ls /usr/share/misc/$f /usr/share/automake-*/$f 2>/dev/null | head -1)
        [ -n "$new" ] || { echo "$f not found: install autotools-dev or automake" >&2; exit 1; }
        cp "$new" "$f"
    done
    HOST=$("$CC" -dumpmachine)
    set -- "$@" --host="$HOST"
    [ "$(uname -m)" = x86_64 ] && BUILD_TRIPLE=x86_64-pc-linux-gnu
fi
# The vtun Makefile sets LDFLAGS to @LIBS@ and links with $(CFLAGS): -static goes in CFLAGS.
CFLAGS="-Os -fcommon -std=gnu89"
[ $STATIC = 1 ] && CFLAGS="$CFLAGS -static"
# -fcommon: the 2008 sources define globals in headers (modern GCC defaults to -fno-common);
# -std=gnu89: they also rely on implicit declarations, an error in C99+ since GCC 14.
CC=$CC CFLAGS=$CFLAGS ./configure "$@" >configure.log 2>&1 \
    || { tail -30 configure.log; exit 1; }
make -s >make.log 2>&1 || { tail -30 make.log; exit 1; }
mkdir -p "$(dirname "$OUT")"
"$STRIP" --strip-unneeded -o "$OUT" vtund
echo "$(basename "$OUT"): vtun $VTUN_VERSION + $(echo "$PATCHES" | wc -l) patch(es), $(wc -c < "$OUT") bytes"
