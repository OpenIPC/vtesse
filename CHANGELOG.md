# Changelog

The version lives in `include/vtesse/version.h`. Binaries are published only as the
[`latest`](https://github.com/compot-dev/vtesse/releases/tag/latest) release, rebuilt
from every push to `main`; this file is the change history.

## 1.11 — 2026-10-15

- The Android APK of every new version is posted to the OpenIPC Telegram dev channel
  by CI (#33), as OpenIPC/decoder does; pushes that do not change the version only
  update the `latest` release.
- README: a screenshot of the Android app with example settings (#31).

## 1.10 — 2026-10-14

- MIPS releases are soft-float (#30): `vtesse-mipsel` and `vtesse-mips` are built with
  the OpenWrt 23.05 toolchains (ramips/mt7621, ath79/generic; musl, soft-float). The previous hard-float builds died
  with `Illegal instruction` on routers without an FPU (OpenWrt, MediaTek MT7620);
  the new ones run on routers and on Ingenic cameras alike. `ci/build-musl.sh` starts
  each build from a clean directory.
- Acceptance on an OpenWrt router (MT7620): TAP backend with udhcpc, inbound access to
  the router's web UI, take-over, `vtesse.init` under procd (respawn), lwIP backend
  with forwards, SOCKS5, UDP relay and link loss; results in docs/testing.md.

## 1.09 — 2026-10-14

- Build-time module selection (#29): `make SOCKS=0`, `UDP_RELAY=0`, `TAP=0` (and
  `LWIP=0`, the TAP-only build); a disabled feature is refused with a clear message,
  `--version` lists the modules. CI builds and tests the minimal variant.
- Bench `fault-test` (#9): link loss, server restart, a DHCP server without option
  121, lease renewal with 2-minute leases; unit test for the refusal of encrypted
  sessions. The bench entrypoint survives `docker restart`.
- Real-device tests against the bench over the LAN (#19, #30): two OpenIPC cameras
  (TAP and lwIP backends, `S99vtesse`) and the Android TV box; bench sessions
  `02BE0C000007/8` for more devices; results in docs/testing.md.

## 1.08 — 2026-10-14

- lwIP memory profiles (#12): `make LWIP_PROFILE=large|medium|small`. Release builds
  for cameras and routers (arm32, mipsel, mips) use **medium** (64 KiB TCP window,
  ~250 KiB static pools), arm64 and x86_64 keep **large** (~125 KiB window with
  scaling, ~1.3 MiB). Measured on OpenIPC cameras (SigmaStar SSC337, 27 MiB RAM,
  11–13 MiB available next to Majestic) against the bench over a 48 ms path: medium
  peaks at ~400 KiB RSS and downloads at ~8.9 Mbit/s through a forward, large at
  ~1 MiB and ~12–13 Mbit/s, small is limited to ~0.9 Mbit/s by its window.
- `bench.sh up` can listen on a LAN address (`BENCH_BIND=…`) to test real devices
  against the bench instead of a production server.

## 1.07 — 2026-10-14

- UDP relay (#28): datagrams sent to the client's tunnel address on a port are relayed
  unchanged to `127.0.0.1` on the device — for RTP pushed by cameras (OpenIPC
  Majestic `outgoing`) and players listening on `udp://@:PORT` (OpenIPC Decoder).
  `vtesse-cli --udp-relay PORT[:LOCAL_PORT]` / `udp_relay=` in the config (lwIP
  backend, up to 3); Android: per-server option on port S559 (1559, 2559, 3559), the
  status shows the camera and player settings and counters. Bench `udp-test`
  (500 numbered datagrams in order, a fragmented 4000-byte one) in CI.

## 1.06 — 2026-10-14

- Android: servers with cameras (#27). Each of the three servers has up to 8
  cameras (name, IP); RTSP, HTTP and optionally SSH are forwarded for every camera on
  fixed local ports, local port = server·1000 + service·10 + camera: RTSP S55N
  (1551–1558, 2551–…), HTTP S80N, SSH S22N; SOCKS5 on S080 (1080, 2080, 3080). Ports
  never overlap between servers, so player slots stay valid. Free-form forwards
  remain as "Other forwards" (the forwards of 1.05 move there). Camera fields have
  accessibility labels. RTP/UDP ("camera 9", port S559) is planned in #28.

## 1.05 — 2026-10-14

- Android: up to three server profiles, each with its own name, server, port, device
  ID, password, SOCKS5 port and port forwards (#26). The settings of 1.04 become
  profile 1; a new profile starts with profile 1's device ID. A profile row on the
  main screen (also with a TV remote); selecting another profile while connected
  reconnects with it (~1.5 s on the test box), an empty profile just disconnects.
  The notification shows the active profile.
- Android: a new tunnel waits until the previous tunnel process has ended instead of
  starting into it; stopping never blocks the service's main thread for long.

## 1.04 — 2026-10-14

- lwIP TCP window ~125 KiB with window scaling (was 16 x MSS ≈ 23 KiB), send
  buffer 64 x MSS (#24). One connection is limited to window / RTT: with ~140 ms to
  the VTun server the old window allowed ~1.3 Mbit/s, so a 1080p camera stream lost
  most frames (only key frames got through). Now the stream through a forward keeps
  up in real time, as with the kernel's TCP; the remaining limit was the path to
  the camera. Small profile (`VTESSE_LWIP_SMALL`) unchanged.

## 1.03 — 2026-10-14

- Android app (#13): `vtesse-android.apk` in the release, Android 5.0+ including
  Android TV boxes. Foreground service without `VpnService` (no VPN icon) in its own
  process, SOCKS5 and port forwards on `127.0.0.1`, generated device ID, status and
  log screen, reconnect on network changes, release signing (key in repository
  secrets). Verified on an Android 10 TV box against a production server: DHCP and
  classless routes, HTTP and RTSP to a camera through the forwards, VLC opening the
  camera stream from `127.0.0.1` (RTSP session, ~1.1 Mbit/s), disconnect and
  reconnect.
- `vt_network_changed()`: drop a connection bound to the old network and reconnect
  at once (`TUNNEL_ERR_NETWORK`).

## 1.02 — 2026-10-14

- Device ID from the device's own MAC: `id_from=eth0` in the config or `--id-from eth0`
  (reads `/sys/class/net/eth0/address`; multicast and all-zero MACs refused) (#6).
- `type tun` sessions: manual IPv4 with `ip=`, `gw=`, `route=` (repeatable) in the
  config or `--ip`/`--gw` (the old `--debug-ip`/`--debug-gw` still work); with `--tap`
  a TUN interface is created and the hook sets the address and routes (#8).
- Bench: `tun-test` (lwIP and TAP over a `type tun` session), `tap-test` takes the
  device ID from an interface MAC; both run in CI.
- DHCP client (#5) closed after verification on a production VTun 3.0.2 server
  (lwIP and TAP: ping and HTTP to a camera behind the server).

## 1.01 — 2026-10-14

- VTun server builds: `server/build-vtund.sh` is the only way vtund is built in the
  repository (bench image and CI); it applies our patches from `server/patches` and
  fails if one is missing or does not apply (#23).
- `vtun-3.0.2-denied-log.patch`: a refused login is logged with the session name
  (device ID) and the reason — wrong password, unknown session, `multi no`, no session
  name (#22).
- Releases carry `vtund-3.0.2-x86_64`, the patched VTun 3.0.2 server for PCs and
  servers, static musl; for cameras and routers vtund is built in the OpenIPC
  environment when needed.
- Releases: only `latest`, titled with the version; no versioned releases.

## 1.00 — 2026-10-13

First release.

- VTun 3.0.2 client: handshake, XOR challenge response, session flags, framing,
  keepalive (#2); `tunnel_ops_t` and event loop helpers (#3).
- lwIP stack over the tunnel: Ethernet netif, ARP, ICMP (#4), DHCP with option 121/249
  routes.
- `vtesse-cli` for Linux (#7); local SOCKS5 proxy (#10) and static port forwards (#11).
- TAP/TUN backend: a kernel interface instead of lwIP, hook scripts for udhcpc (#19).
- Static musl builds for camera and router targets, CI and releases (#20); service
  scripts `S99vtesse` (BusyBox/OpenIPC) and `vtesse.init` (OpenWrt procd).
- Docker test bench with a VTun 3.0.2 server and acceptance tests (#1).
