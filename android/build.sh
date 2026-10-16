#!/bin/sh
# Build the APKs in the Android Docker image (ci/Dockerfile.android).
#
#   android/build.sh [gradle tasks]     default: assembleDebug assembleRelease
#
# Output: android/app/build/outputs/apk/{debug,release}/. Gradle and its
# dependency cache live in android/.gradle-cache (not in the repository).
#
# VTESSE_ANDROID_IMAGE=<image> uses a ready Android image instead of building
# ci/Dockerfile.android. The image must provide JDK 17, the SDK/NDK/CMake of
# that Dockerfile, `gradle` on PATH, and export ANDROID_HOME (or
# ANDROID_SDK_ROOT) - the script checks the last two before building. Without
# the variable the image is built as vtesse-android.
#
# Release signing (optional; without it the release APK is unsigned):
#   VTESSE_KEYSTORE=/path/to/key.jks plus VTESSE_KEYSTORE_PASSWORD, VTESSE_KEY_ALIAS,
#   VTESSE_KEY_PASSWORD in the environment (CI: from repository secrets), or
#   ~/.config/vtesse/vtesse-release.jks with ~/.config/vtesse/android-signing.env
#   holding those three variables (local builds).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
LOCAL_KEYS=$HOME/.config/vtesse

if [ -z "${VTESSE_KEYSTORE:-}" ] && [ -f "$LOCAL_KEYS/vtesse-release.jks" ] && [ -f "$LOCAL_KEYS/android-signing.env" ]; then
    VTESSE_KEYSTORE=$LOCAL_KEYS/vtesse-release.jks
    . "$LOCAL_KEYS/android-signing.env"
fi
SIGN=
if [ -n "${VTESSE_KEYSTORE:-}" ]; then
    [ -f "$VTESSE_KEYSTORE" ] || { echo "keystore not found: $VTESSE_KEYSTORE" >&2; exit 1; }
    KDIR=$(cd "$(dirname "$VTESSE_KEYSTORE")" && pwd)
    SIGN="-v $KDIR:/keys:ro -e VTESSE_KEYSTORE=/keys/$(basename "$VTESSE_KEYSTORE")
          -e VTESSE_KEYSTORE_PASSWORD -e VTESSE_KEY_ALIAS -e VTESSE_KEY_PASSWORD"
    export VTESSE_KEYSTORE_PASSWORD VTESSE_KEY_ALIAS VTESSE_KEY_PASSWORD
fi

IMAGE=${VTESSE_ANDROID_IMAGE:-}
ENTRY=
if [ -z "$IMAGE" ]; then
    IMAGE=vtesse-android
    docker build -q -t "$IMAGE" -f "$ROOT/ci/Dockerfile.android" "$ROOT/ci" >/dev/null
else
    ENTRY=--entrypoint=   # a ready image's own ENTRYPOINT must not swallow the commands
    docker run --rm $ENTRY "$IMAGE" sh -c 'command -v gradle >/dev/null && [ -n "${ANDROID_HOME:-}${ANDROID_SDK_ROOT:-}" ]' || {
        echo "$IMAGE: needs gradle on PATH and ANDROID_HOME or ANDROID_SDK_ROOT set" >&2; exit 1; }
fi
mkdir -p "$HERE/.gradle-cache/home"
# shellcheck disable=SC2086  # SIGN and ENTRY are lists of docker options
docker run --rm --user "$(id -u):$(id -g)" -e GRADLE_USER_HOME=/src/android/.gradle-cache \
    -e HOME=/src/android/.gradle-cache/home $SIGN $ENTRY \
    -v "$ROOT:/src" -w /src/android "$IMAGE" \
    gradle --no-daemon -q ${*:-assembleDebug assembleRelease}
ls -l "$HERE"/app/build/outputs/apk/*/*.apk
