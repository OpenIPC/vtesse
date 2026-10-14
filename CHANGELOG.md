# Changelog

The version lives in `include/vtesse/version.h`. Binaries are published only as the
[`latest`](https://github.com/compot-dev/vtesse/releases/tag/latest) release, rebuilt
from every push to `main`; this file is the change history.

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
