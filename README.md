![OpenIPC logo][logo]

# VTesse

**English** | [Русский](README.ru.md) | [简体中文](README.zh-CN.md)

[![Build](https://github.com/compot-dev/vtesse/actions/workflows/build.yml/badge.svg)](https://github.com/compot-dev/vtesse/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/compot-dev/vtesse/releases/tag/latest)
[![License](https://img.shields.io/badge/license-Prosperity%203.0-blue)](LICENSE.md)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Android-brightgreen)](docs/architecture.md)
[![Status](https://img.shields.io/badge/status-prototype-orange)](docs/security.md)

A resource access microsystem: a client for existing **VTun** servers (`vtund -s`) that works **without a TUN/TAP device and without a system VPN**. The tunnel is terminated inside the application by a userspace TCP/IP stack (lwIP); applications reach remote devices through a local SOCKS5 proxy or port forwards on `127.0.0.1`.

## Features

- **No VPN:** no root, no `/dev/net/tun`, no Android `VpnService` (no VPN icon)
- **VTun:** 3.0.x servers, TCP transport, `type ether` and `type tun` sessions, XOR challenge response, keepalive, automatic reconnection with exponential back-off
- **Network:** address and routes via DHCP through the tunnel (option 121, RFC 3442), IPv4, outbound connections only
- **Local proxy:** SOCKS5 `CONNECT` with optional username/password, static TCP port forwards (e.g. RTSP over TCP for video players)
- **Targets:** embedded Linux (IP cameras, routers; ARM and MIPS, including big-endian, uClibc/musl), Android, and later an embeddable communication module
- **Library first:** C99 + POSIX, single-threaded, no own main loop — easy to embed

## Download

Static binaries (no dependencies) are on the [releases page](https://github.com/compot-dev/vtesse/releases): [`latest`](https://github.com/compot-dev/vtesse/releases/tag/latest) is rebuilt from every push to `main` (stable file names, used below); versioned releases such as [`v1.00`](https://github.com/compot-dev/vtesse/releases/tag/v1.00) carry the version in the file names (`vtesse-1.00-arm32`).

| Platform | `ARCH` |
|---|---|
| OpenIPC cameras: HiSilicon, Goke, SigmaStar, other 32-bit ARM; Raspberry Pi with a 32-bit OS | `arm32` |
| OpenIPC cameras: Ingenic (T31 and others); MIPS little-endian routers (MediaTek MT7621, MT7628) | `mipsel` |
| MIPS big-endian routers (Qualcomm Atheros ath79 and others) | `mips` |
| 64-bit ARM: Raspberry Pi with a 64-bit OS, Rockchip, other ARM64 boards | `arm64` |
| PCs and servers | `x86_64` |

Two binaries per platform: `vtesse-ARCH` (lwIP and TAP backends: SOCKS5 / port forwards without root, or a kernel interface with `--tap`) and `vtesse-tap-ARCH` (TAP only, about half the size). The TAP backend needs root and the `tun` driver (`/dev/net/tun`), and the hook scripts `vtesse-hook.sh` and `vtesse-udhcpc.script`.

### Configuration

All installations below read `/etc/vtesse.conf` (the session name is the device ID: 12 characters `0-9A-F`, also used as the MAC):

```sh
cat > /etc/vtesse.conf <<EOF
host=vtun.example.org
port=5555
session=02AB34CD56EF
password=secret
EOF
chmod 600 /etc/vtesse.conf
```

### OpenIPC camera

```sh
# as root; ARCH = arm32 or mipsel, see the table
ARCH=arm32
URL=https://github.com/compot-dev/vtesse/releases/download/latest
cd /tmp
curl -fsSLO $URL/vtesse-tap-$ARCH
curl -fsSLO $URL/vtesse-hook.sh
curl -fsSLO $URL/vtesse-udhcpc.script
curl -fsSL $URL/SHA256SUMS | grep -E " (vtesse-tap-$ARCH|vtesse-hook.sh|vtesse-udhcpc.script)$" | sha256sum -c -   # check the download
# install
cp vtesse-tap-$ARCH /usr/bin/vtesse
mkdir -p /usr/share/vtesse
cp vtesse-hook.sh vtesse-udhcpc.script /usr/share/vtesse/
chmod 755 /usr/bin/vtesse /usr/share/vtesse/*
vtesse --version
# run in the foreground (Ctrl+C to stop)
vtesse --config /etc/vtesse.conf --tap --ifname vtesse0 --hook /usr/share/vtesse/vtesse-hook.sh
```

Start at boot:

```sh
cat > /etc/init.d/S99vtesse <<'EOF'
#!/bin/sh
case "$1" in
    start) start-stop-daemon -S -b -m -p /var/run/vtesse.pid -x /usr/bin/vtesse -- \
               --config /etc/vtesse.conf --tap --ifname vtesse0 --hook /usr/share/vtesse/vtesse-hook.sh ;;
    stop)  start-stop-daemon -K -p /var/run/vtesse.pid ;;
esac
EOF
chmod +x /etc/init.d/S99vtesse
/etc/init.d/S99vtesse start
```

### OpenWrt router

```sh
opkg update && opkg install curl kmod-tun   # kmod-tun is needed for --tap only
# apk add curl kmod-tun on OpenWrt 25+
# install the binary and the scripts as on the camera above, then:
cat > /etc/init.d/vtesse <<'EOF'
#!/bin/sh /etc/rc.common
START=99
USE_PROCD=1
start_service() {
    procd_open_instance
    procd_set_param command /usr/bin/vtesse --config /etc/vtesse.conf \
        --tap --ifname vtesse0 --hook /usr/share/vtesse/vtesse-hook.sh
    procd_set_param respawn
    procd_close_instance
}
EOF
chmod +x /etc/init.d/vtesse
/etc/init.d/vtesse enable && /etc/init.d/vtesse start
```

### Linux PC, Raspberry Pi

```sh
ARCH=x86_64   # x86_64; arm64 / arm32 on a Raspberry Pi with a 64-bit / 32-bit OS
curl -fsSLO https://github.com/compot-dev/vtesse/releases/download/latest/vtesse-$ARCH
chmod +x vtesse-$ARCH
# no root needed: SOCKS5 on 127.0.0.1:1080 and a forward to a camera's RTSP
./vtesse-$ARCH --config /etc/vtesse.conf --socks 1080 --forward 8554:10.78.0.10:554
```

## Build from source

```sh
make
mkdir -p ~/.config/vtesse
cat > ~/.config/vtesse/vtesse.conf <<EOF
host=vtun.example.org
port=5555
session=02AB34CD56EF
password=secret
EOF
chmod 600 ~/.config/vtesse/vtesse.conf

build/vtesse-cli --config ~/.config/vtesse/vtesse.conf --socks 1080 --forward 8554:10.78.0.10:554
curl --socks5 127.0.0.1:1080 http://10.78.0.10/
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/stream
```

The session name is the device ID (12 characters `0-9A-F`, also used as the MAC); credentials are read only from the configuration file.

## Documentation

| Document | Contents |
|---|---|
| [Usage](docs/usage.md) | building, `vtesse-cli` options, configuration file, device ID, DHCP and routes, local proxy, error messages, probe tool |
| [VTun protocol](docs/protocol.md) | handshake, challenge response, session flags, framing, keepalive, server behaviour that affects clients, UDP |
| [Architecture](docs/architecture.md) | layers, `tunnel_ops_t`, VTun client, lwIP stack, proxy and flow control, repository layout |
| [Embedding](docs/embedding.md) | using the libraries in another application, loop integration, example |
| [Security](docs/security.md) | VTun authentication weaknesses, clear-text data, local proxy exposure |
| [Testing](docs/testing.md) | unit tests, sanitizers, big-endian MIPS, Docker test bench and acceptance tests |

## Roadmap

Stages and tasks are tracked as [GitHub issues](https://github.com/compot-dev/vtesse/issues):

| Stage | Status | Issues |
|---|---|---|
| 0. Test bench (VTun 3.0.2 in Docker, bridge, DHCP with option 121) | ✅ Done | [#1](https://github.com/compot-dev/vtesse/issues/1) ✅ |
| 1. CLI prototype on Linux: TCP + `type ether` + DHCP over lwIP | ✅ Done | [#2](https://github.com/compot-dev/vtesse/issues/2) ✅ [#3](https://github.com/compot-dev/vtesse/issues/3) ✅ [#4](https://github.com/compot-dev/vtesse/issues/4) ✅ [#5](https://github.com/compot-dev/vtesse/issues/5) [#6](https://github.com/compot-dev/vtesse/issues/6) [#7](https://github.com/compot-dev/vtesse/issues/7) ✅ [#8](https://github.com/compot-dev/vtesse/issues/8) |
| 2. Local proxy: SOCKS5 and port forwarding | ✅ Done | [#10](https://github.com/compot-dev/vtesse/issues/10) ✅ [#11](https://github.com/compot-dev/vtesse/issues/11) ✅ |
| 3. Embedded Linux: ARM/MIPS cross builds, footprint, CI | 🔄 In progress | [#12](https://github.com/compot-dev/vtesse/issues/12) [#20](https://github.com/compot-dev/vtesse/issues/20) ✅ |
| 3. TAP/TUN backend (kernel interface instead of lwIP, Linux targets) | 🔄 In progress | [#19](https://github.com/compot-dev/vtesse/issues/19) |
| 4. Android app (no VpnService) | ⏳ Planned | [#13](https://github.com/compot-dev/vtesse/issues/13) |
| 5. Optional: UDP transport, TLS wrapper, Blowfish auth | ⏳ Planned | [#14](https://github.com/compot-dev/vtesse/issues/14) [#15](https://github.com/compot-dev/vtesse/issues/15) [#16](https://github.com/compot-dev/vtesse/issues/16) |
| 6. Reuse of the upper layers for camex | ⏳ Planned | [#17](https://github.com/compot-dev/vtesse/issues/17) |
| Cross-cutting: tests, license | 🔄 In progress | [#9](https://github.com/compot-dev/vtesse/issues/9) [#18](https://github.com/compot-dev/vtesse/issues/18) ✅ |

✅ — closed issue.

## License

[The Prosperity Public License 3.0.0](LICENSE.md): free for noncommercial use, with a 30-day trial for commercial use. See [LICENSE.md](LICENSE.md) for the full terms. Third-party code keeps its own license (lwIP: BSD-3-Clause).

## Support

- Questions and bug reports: [GitHub Issues](https://github.com/compot-dev/vtesse/issues)
- Documentation: [docs/](docs/)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
