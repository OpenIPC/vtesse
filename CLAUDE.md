# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Local builds and checks run in Docker

Nothing is installed on the host: no toolchains, JDK, Android SDK or ffmpeg. Every image tag
below is pinned, and each one runs with `--pull never`, so a check never pulls a different
version than the last run. Toolchains are cached on the host in
`~/.cache/compot/toolchains`, mounted read-only.

```sh
# native build and unit tests (what the CI "Unit tests" job runs)
docker run --rm --pull never -u "$(id -u):$(id -g)" -v "$PWD:/src" -w /src compot/cross:2026.10 \
  sh -c 'make -j"$(nproc)" BUILD=build-dev && make -j"$(nproc)" BUILD=build-dev test'

# static musl binaries for a camera or router target, unit tests under qemu-user
docker run --rm --pull never -u "$(id -u):$(id -g)" -v "$PWD:/src" \
  -v ~/.cache/compot/toolchains:/opt/tc:ro -w /src compot/cross:2026.10 \
  sh -c 'QEMU=1 ci/build-musl.sh arm32 dist'

# Android APKs
VTESSE_ANDROID_IMAGE=compot/android:2026.10 android/build.sh assembleDebug
```

`build-*/` and `dist/` are gitignored.

## Checking an RTSP stream

Use the pinned `jrottenberg/ffmpeg:6.1-alpine`. It includes `ffprobe` and the RTSP demuxer.
Never run `jrottenberg/ffmpeg` without a tag: Docker pulls `latest`, about 300 MB, which
nothing else uses.

```sh
docker run --rm --pull never --network host --entrypoint ffprobe jrottenberg/ffmpeg:6.1-alpine \
  -hide_banner -rtsp_transport tcp -i rtsp://<user>:<pass>@<host>:<port>/<path>
docker run --rm --pull never --network host jrottenberg/ffmpeg:6.1-alpine \
  -hide_banner -rtsp_transport tcp -i rtsp://... -t 5 -f null -
```

## Rules

- Local builds are for development only. Releases, pre-releases and builds for testers come
  from CI.
- `bench/bench.sh` builds and runs its own `vtesse-bench` image. The `vtesse-bench-server`
  container may be in use by another session: check `docker ps` before running `bench.sh` or
  removing anything.
