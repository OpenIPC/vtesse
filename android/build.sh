#!/bin/sh
# Build the APKs in the Android Docker image (ci/Dockerfile.android).
#
#   android/build.sh [gradle tasks]     default: assembleDebug assembleRelease
#
# Output: android/app/build/outputs/apk/{debug,release}/. Gradle and its
# dependency cache live in android/.gradle-cache (not in the repository).
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

docker build -q -t vtesse-android -f "$ROOT/ci/Dockerfile.android" "$ROOT/ci" >/dev/null
mkdir -p "$HERE/.gradle-cache/home"
# shellcheck disable=SC2086  # SIGN is a list of docker options
docker run --rm --user "$(id -u):$(id -g)" -e GRADLE_USER_HOME=/src/android/.gradle-cache \
    -e HOME=/src/android/.gradle-cache/home $SIGN \
    -v "$ROOT:/src" -w /src/android vtesse-android \
    gradle --no-daemon -q ${*:-assembleDebug assembleRelease}
ls -l "$HERE"/app/build/outputs/apk/*/*.apk
