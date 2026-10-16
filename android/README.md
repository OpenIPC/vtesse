# VTesse for Android

VTun client for Android **without `VpnService`** (#13): a foreground service with a
regular notification runs the tunnel and the lwIP stack and offers a local SOCKS5
proxy and TCP port forwards on `127.0.0.1`. Other apps use them directly, e.g. VLC
with `rtsp://127.0.0.1:8554/...` for a forward `8554:CAMERA_IP:554`. No VPN icon, no
VPN permission; Android 5.0 (API 21) and later, phones and Android TV boxes (TV
launcher banner, no touchscreen required).

- Servers: up to three (name, VTun server, port, device ID, password), selected with
  the buttons at the top. Selecting another server while connected disconnects and
  connects with it; the notification shows `VTesse · Server N · name`.
- Cameras: up to 8 per server (name, IP). For every camera the checked services
  are forwarded on fixed local ports, **local port = server·1000 + service·10 +
  camera** (#27):

  | Service | Camera port | Server 1 | Server 2 | Server 3 |
  |---|---|---|---|---|
  | RTSP (55) | 554 | 1551–1558 | 2551–2558 | 3551–3558 |
  | HTTP (80) | 80 | 1801–1808 | 2801–2808 | 3801–3808 |
  | SSH (22), off by default | 22 | 1221–1228 | 2221–2228 | 3221–3228 |
  | SOCKS5 | — | 1080 | 2080 | 3080 |

  Camera 3 of server 1 in a player: `rtsp://user:pass@127.0.0.1:1553/stream=0`; its
  web UI: `http://127.0.0.1:1803/`. Anything else goes into *Other forwards*
  (`LPORT:IP:RPORT`).
- RTP/UDP from cameras (#28): with *Receive RTP/UDP* checked, datagrams sent to the
  device's tunnel address on port **S559** (1559, 2559, 3559 — "camera 9" of the RTSP
  block) are relayed to `127.0.0.1:S559`. The status shows what to configure: on the
  camera `outgoing` → `udp://<tunnel address>:1559` (majestic.yaml: `server:` or
  `servers:` depending on the Majestic version, see
  [usage](../docs/usage.md#udp-relay-rtp-from-cameras)), in the player `udp://@:1559`
  (OpenIPC Decoder: a `udp://` slot, shown as `UDP push`). The tunnel address stays
  the same across reconnects. Verified on the Android TV box: a camera pushing H.264
  1080p, Decoder playing it at full rate through the relay.
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
  --es android.intent.extra.TEXT 'rtsp://user:pass@127.0.0.1:1551/stream=0' \
  --es transport tcp -n com.openipc.decoder/.Decoder
```

Keep Decoder's **S** mode (key frames only) off for full-rate video. VLC works too
(`rtsp://127.0.0.1:1551/...`). Smooth 1080p through the tunnel was confirmed on an
Android TV box (#25); if a stream shows far fewer frames than the camera is set to,
check the camera's own uplink first (transmit drops in its `/metrics`): the tunnel
carries several Mbit/s per connection (#24).

## Build

```sh
android/build.sh                    # debug + release APK in Docker (ci/Dockerfile.android)
VTESSE_ANDROID_IMAGE=<image> android/build.sh   # use a ready image (JDK 17, SDK/NDK/CMake, gradle, ANDROID_HOME)
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
