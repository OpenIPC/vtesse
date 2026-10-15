#!/bin/sh
# Build VTesse for big-endian MIPS (static) and run the test suite under qemu-mips.
#   ci/test-mips.sh
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=vtesse-ci-mips
docker build -q -t "$IMAGE" -f "$HERE/ci/Dockerfile.mips" "$HERE/ci" >/dev/null
docker run --rm --user "$(id -u):$(id -g)" -v "$HERE:/src" -w /src "$IMAGE" sh -c '
    set -e
    make -s BUILD=build-mips CC=mips-linux-gnu-gcc AR=mips-linux-gnu-ar LDFLAGS=-static all build-mips/test_proto build-mips/test_client build-mips/test_route
    file_info=$(od -An -tx1 -j5 -N1 build-mips/vtesse-cli | tr -d " ")
    [ "$file_info" = "02" ] && echo "ELF data: big-endian"
    for t in build-mips/test_proto build-mips/test_route build-mips/test_client; do
        echo "== qemu-mips $t"; qemu-mips "$t"
    done
    ls -l build-mips/vtesse-cli
'
