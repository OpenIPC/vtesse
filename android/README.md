# VTesse for Android

VTun client for Android **without `VpnService`** (#13): a foreground service with a
regular notification runs the tunnel and the lwIP stack and offers a local SOCKS5
proxy and TCP port forwards on `127.0.0.1`. Other apps use them directly, e.g. VLC
with `rtsp://127.0.0.1:8554/...` for a forward `8554:CAMERA_IP:554`. No VPN icon, no
VPN permission; Android 5.0 (API 21) and later, phones and Android TV boxes (TV
launcher banner, no touchscreen required).

- Server profiles: up to three (name, server, port, device ID, password, SOCKS5 port,
  forwards each), selected with the buttons at the top. Selecting another profile
  while connected disconnects and connects with it; the notification shows
  `VTesse · N · name`. Profile 1 holds the settings of earlier versions.
- Device ID: generated once (random locally administered unicast MAC, second
  character `2`, `6`, `A` or `E`) and kept; it can be replaced by hand. Add it as a
  session on the VTun server (long press copies it).
- The tunnel runs in its own process (`:tunnel`): the native stack can be started
  once per process, so *Disconnect* ends that process.
- Network changes (Wi-Fi ↔ mobile) reconnect at once (`vt_network_changed()`);
  a partial wake lock keeps keepalives going with the screen off.
- Native code: the repository's C sources built by CMake
  ([`app/src/main/cpp`](app/src/main/cpp)); JNI bridge `vtesse_jni.c`.

## Players

Recommended: [OpenIPC Decoder](https://github.com/OpenIPC/decoder) with the slot's
transport set to **TCP** — RTP over UDP cannot pass a TCP port forward (Auto falls
back to TCP after 3 s). A camera can be opened from the shell:

```sh
adb shell am start -a android.intent.action.SEND -t text/plain \
  --es android.intent.extra.TEXT 'rtsp://user:pass@127.0.0.1:8554/stream=0' \
  --es transport tcp -n com.openipc.decoder/.Decoder
```

Keep Decoder's **S** mode (key frames only) off for full-rate video. VLC works too
(`rtsp://127.0.0.1:8554/...`). Smooth 1080p through the tunnel was confirmed on an
Android TV box (#25); if a stream shows far fewer frames than the camera is set to,
check the camera's own uplink first (transmit drops in its `/metrics`): the tunnel
carries several Mbit/s per connection (#24).

## Build

```sh
android/build.sh                    # debug + release APK in Docker (ci/Dockerfile.android)
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

Release signing: `VTESSE_KEYSTORE` (path) with `VTESSE_KEYSTORE_PASSWORD`,
`VTESSE_KEY_ALIAS`, `VTESSE_KEY_PASSWORD` in the environment, or
`android/keystore.properties` (`storeFile`, `storePassword`, `keyAlias`,
`keyPassword`); `android/build.sh` also picks up a key kept outside the repository in
`~/.config/vtesse/`. CI signs with the repository secrets `VTESSE_KEYSTORE_BASE64`,
`VTESSE_KEYSTORE_PASSWORD`, `VTESSE_KEY_ALIAS`, `VTESSE_KEY_PASSWORD`; without a key
the release APK is unsigned and the release job refuses it. Keep a backup of the
key: updates must be signed with the same key.

Release certificate (SHA-256):
`5B:EF:22:D7:BB:4F:1B:CB:DA:0F:4E:DE:AD:2B:6D:D7:18:C1:48:AA:F4:48:17:BB:69:2A:15:72:A8:FB:0D:E2`
