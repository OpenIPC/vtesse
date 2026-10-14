#!/bin/sh
# Build the APKs in the Android Docker image (ci/Dockerfile.android).
#
#   android/build.sh [gradle tasks]     default: assembleDebug assembleRelease
#
# Output: android/app/build/outputs/apk/{debug,release}/. Gradle and its
# dependency cache live in android/.gradle-cache (not in the repository).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
docker build -q -t vtesse-android -f "$ROOT/ci/Dockerfile.android" "$ROOT/ci" >/dev/null
mkdir -p "$HERE/.gradle-cache/home"
docker run --rm --user "$(id -u):$(id -g)" -e GRADLE_USER_HOME=/src/android/.gradle-cache \
    -e HOME=/src/android/.gradle-cache/home \
    -v "$ROOT:/src" -w /src/android vtesse-android \
    gradle --no-daemon -q ${*:-assembleDebug assembleRelease}
ls -l "$HERE"/app/build/outputs/apk/*/*.apk
