# VTesse for Android

VTun client for Android **without `VpnService`** (#13): a foreground service with a
regular notification runs the tunnel and the lwIP stack and offers a local SOCKS5
proxy and TCP port forwards on `127.0.0.1`. Other apps use them directly, e.g. VLC
with `rtsp://127.0.0.1:8554/...` for a forward `8554:CAMERA_IP:554`. No VPN icon, no
VPN permission; Android 5.0 (API 21) and later.

- Device ID: generated once (random locally administered unicast MAC, second
  character `2`, `6`, `A` or `E`) and kept; it can be replaced by hand. Add it as a
  session on the VTun server (long press copies it).
- The tunnel runs in its own process (`:tunnel`): the native stack can be started
  once per process, so *Disconnect* ends that process.
- Network changes (Wi-Fi ↔ mobile) reconnect at once (`vt_network_changed()`);
  a partial wake lock keeps keepalives going with the screen off.
- Native code: the repository's C sources built by CMake
  ([`app/src/main/cpp`](app/src/main/cpp)); JNI bridge `vtesse_jni.c`.

## Build

```sh
android/build.sh                    # debug + release APK in Docker (ci/Dockerfile.android)
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

Release signing reads `android/keystore.properties` (`storeFile`, `storePassword`,
`keyAlias`, `keyPassword`) or the `VTESSE_KEYSTORE*` environment variables; without
them the release APK is unsigned.
