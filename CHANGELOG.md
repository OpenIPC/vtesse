# Changelog

The version lives in `include/vtesse/version.h`. Binaries are published only as the
[`latest`](https://github.com/compot-dev/vtesse/releases/tag/latest) release, rebuilt
from every push to `main`; this file is the change history.

## 1.01 — 2026-10-14

- VTun server builds: `server/build-vtund.sh` is the only way vtund is built in the
  repository (bench image and CI); it applies our patches from `server/patches` and
  fails if one is missing or does not apply (#23).
- `vtun-3.0.2-denied-log.patch`: a refused login is logged with the session name
  (device ID) and the reason — wrong password, unknown session, `multi no`, no session
  name (#22).
- Releases carry `vtund-3.0.2-<arch>`, the patched VTun 3.0.2 server, static musl,
  for arm32, mipsel, mips, arm64 and x86_64.
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
