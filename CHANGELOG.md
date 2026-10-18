# Changelog

The version lives in `include/vtesse/version.h`. Binaries are published only as the
[`latest`](https://github.com/OpenIPC/vtesse/releases/tag/latest) release, rebuilt
from every push to `main`; this file is the change history. Versions after 1.12 are
`MAJOR.MINOR.PATCH` (1.12 is followed by 1.12.1 or 1.13.0); the headings below keep the
numbers as released.

## 1.15.0 — 2026-10-18

Settings in the U-Boot environment for helpdesk access (#64).

- `fw_setenv vtesse <server>` is enough to bring a camera with vtesse online, as
  `fw_setenv vtun` is for VTun on OpenIPC: with only the server, the client uses the
  defaults of OpenIPC's VTun client: port 5000, the session is the MAC of the
  default-route interface without colons, the password is `echo "$MAC" | sha1sum`.
- `vtesse_port`, `vtesse_session` / `vtesse_id_from`, `vtesse_password`, `vtesse_ip`,
  `vtesse_gw` override those and `/etc/vtesse.conf` key by key; the rest stays in the
  file. Order: command line, U-Boot environment, file, defaults.
- `S99vtesse` and `vtesse.init` start without `/etc/vtesse.conf` when `vtesse` is set;
  `--install` works without keys then, and stores given keys with `fw_setenv`
  (without `fw_setenv`, in `/etc/vtesse.conf` next to the server in the environment;
  a key whose variable is set is refused, and the file needs no server or password);
  `--uninstall` leaves the variables.
- New `--show-config` (the settings in effect) and `--no-env`.
- The default interface for the device ID must have a MAC (`--install` and the
  environment defaults).

## 1.14.0 — 2026-10-18

Self-install (#52), environment detection (#51), install paths (#50).

- `vtesse --install` installs the running binary as a service: it detects OpenIPC,
  OpenWrt, a Linux PC with systemd or another BusyBox system, copies itself to
  `/usr/bin/vtesse` (`/usr/local/bin` on a PC), writes `/etc/vtesse.conf` from
  `--host`, `--port`, `--password` (`-` reads standard input), `--session` or
  `--id-from`, `--ip`, `--gw`, writes the init script (`S99vtesse`, procd `vtesse.init`
  with its `rc.d` link, or `vtesse.service`) and starts it. A new configuration gets
  `port=5000` and the device ID from the interface of the default route unless they are
  given; an existing one keeps its other lines. On OpenWrt
  `/lib/upgrade/keep.d/vtesse` keeps the installation across `sysupgrade` (before, the
  binary and the scripts were lost on an upgrade). Running it again updates the binary
  and the scripts.
- `vtesse --uninstall` stops the service and removes everything, `/etc/vtesse.conf`
  included.
- The hook scripts are built into the binary. `--tap` without `--hook` now runs the
  built-in hook (DHCP through udhcpc with the built-in `vtesse-udhcpc.script`; before, no
  hook ran), or `<prefix>/libexec/vtesse/vtesse-hook.sh` when it exists
  (`--install --scripts` writes it there to be edited); `--hook none` runs no hook, as
  before without `--hook`. The built-in hook needs BusyBox `udhcpc` and does not fall
  back to `dhclient`, whose stock script may replace `/etc/resolv.conf` and add a default
  route through the tunnel. Hook scripts in `/usr/share/vtesse` (1.13 and older) are
  moved to `/usr/libexec/vtesse` by `--install` and stay in use when
  `/etc/default/vtesse` names them; otherwise they are kept there as inactive `*.1.13`
  copies and the built-in scripts run.
- `S99vtesse` and `vtesse.init` no longer pass `--hook /usr/share/vtesse/...`; the
  service files load the `tun` module when `/dev/net/tun` is missing. `S99vtesse`
  stop/status find the daemon by name, so they still work after the binary is
  replaced. New `scripts/vtesse.service` for systemd.
- The binaries grow by ~33 KiB (arm32 177 → 210 KiB), of which 8 KiB are the scripts.
- READMEs: one install command for OpenIPC, OpenWrt and a PC instead of downloading
  each file. The scripts stay on the release page for `--hook FILE` and manual setups.

## 1.13.0 — 2026-10-17

One binary per platform (#43) and the `MAJOR.MINOR.PATCH` versions (#44).

- The release has one binary per platform, `vtesse-<arch>`, with both backends (lwIP, and
  TAP with `--tap`). The TAP-only `vtesse-tap-<arch>` is no longer published: its links
  return 404, use `vtesse-<arch> --tap`; the binary name on the device and the command line
  stay the same. It costs 72–83 KiB per binary (31–44 KiB compressed on flash); the release
  goes from 17 files to 12. `make LWIP=0` still builds a TAP-only binary (CI and the bench
  test it).
- Versions are `MAJOR.MINOR.PATCH` from this one on; CI checks that a code or build change
  comes with a new version and its section here. The Android `versionCode` is
  `MAJOR*10000 + MINOR*100 + PATCH` (11300; 1.12 was 112), so the update installs.
- READMEs: the OpenIPC and OpenWrt install commands fetch `vtesse-$ARCH`.

## 1.12 — 2026-10-15

Fixes from the first Qodo review of the whole codebase (#34, PR #35):

- lwIP backend: a DHCP renewal replaces the classless routes (or removes them) even
  though the address stays the same; before, only the first lease set them.
- The keepalive timeout is refreshed only by complete frames: a peer that trickles
  bytes of a frame it never finishes no longer keeps a dead session alive.
- `vt_create()` rejects negative timer settings; the reconnect backoff cannot overflow.
- SOCKS5: an unfinished handshake is dropped after the connect timeout (15 s), so idle
  clients cannot hold every connection slot.
- `vtesse-cli`: `--seconds` and the `--fetch` port are checked (`--seconds 1m` used to
  mean "no limit"); the `stop` hook runs only after earlier hooks finished, so a DHCP
  client started by a late `up` hook is not left running.
- `vtesse-udhcpc.script` installs on-link classless routes (gateway 0.0.0.0) without `via`.
- `S99vtesse` stop/status check the process (`-x`), so a stale PID file cannot make
  them signal or report another process.
- CI: unit-test failures under QEMU fail the build (`| tail -1` hid them); toolchain
  archives are checked against SHA256 before their compiler runs; the bench self-test
  checks that ECHO_REQ really is the first frame.

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
