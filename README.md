![OpenIPC logo][logo]

# VTesse

**English** | [Русский](README.ru.md) | [简体中文](README.zh-CN.md)

[![Build](https://github.com/OpenIPC/vtesse/actions/workflows/build.yml/badge.svg)](https://github.com/OpenIPC/vtesse/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/OpenIPC/vtesse/releases/latest)
[![License](https://img.shields.io/badge/license-Prosperity%203.0-blue)](LICENSE.md)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Android-brightgreen)](docs/architecture.md)
[![Status](https://img.shields.io/badge/status-prototype-orange)](docs/security.md)

A resource access microsystem: a client for existing **VTun** servers (`vtund -s`) that works **without a TUN/TAP device and without a system VPN**. The tunnel is terminated inside the application by a userspace TCP/IP stack (lwIP); applications reach remote devices through a local SOCKS5 proxy or port forwards on `127.0.0.1`.

> [!WARNING]
> **An experimental tool, not a security product.** The tunnel has no encryption and no privacy protection: all traffic, including video, passwords and the web interfaces of the devices behind it, travels in clear text, and the VTun handshake can reveal the session password to anyone who captures it. Anyone on the path (public Wi-Fi, mobile and provider networks, the Internet) can read, alter or take over the session. Use it for experiments and only over networks you trust, or inside a secure channel of your own (TLS/stunnel, an operator VPN); do not expose cameras or other devices through it on open networks. Details: [docs/security.md](docs/security.md).

## Features

- **No VPN:** no root, no `/dev/net/tun`, no Android `VpnService` (no VPN icon)
- **VTun:** 3.0.x servers, TCP transport, `type ether` and `type tun` sessions, XOR challenge response, keepalive, automatic reconnection with exponential back-off
- **Network:** address and routes via DHCP through the tunnel (option 121, RFC 3442), IPv4, outbound connections only
- **UDP relay:** RTP pushed by cameras to the tunnel address reaches a local player (`--udp-relay`)
- **Local proxy:** SOCKS5 `CONNECT` with optional username/password, static TCP port forwards (e.g. RTSP over TCP for video players)
- **Targets:** embedded Linux (IP cameras, routers; ARM and MIPS, including big-endian, uClibc/musl), Android, and later an embeddable communication module
- **Library first:** C99 + POSIX, single-threaded, no own main loop — easy to embed

## Download

Static binaries (no dependencies) are on the [releases page](https://github.com/OpenIPC/vtesse/releases): [`latest`](https://github.com/OpenIPC/vtesse/releases/latest) is rebuilt from every push to `main` (stable file names, used below). The release title shows the VTesse version; the change history is in [CHANGELOG.md](CHANGELOG.md).

| Platform                                                                                        | `ARCH`   |
|-------------------------------------------------------------------------------------------------|----------|
| OpenIPC cameras: HiSilicon, Goke, SigmaStar, other 32-bit ARM; Raspberry Pi with a 32-bit OS    | `arm32`  |
| OpenIPC cameras: Ingenic (T31 and others); MIPS little-endian routers (MediaTek MT7621, MT7628) | `mipsel` |
| MIPS big-endian routers (Qualcomm Atheros ath79 and others)                                     | `mips`   |
| 64-bit ARM: Raspberry Pi with a 64-bit OS, Rockchip, other ARM64 boards                         | `arm64`  |
| PCs and servers                                                                                 | `x86_64` |

One binary per platform, `vtesse-ARCH`, with both backends: lwIP (SOCKS5 / port forwards without root) and TAP (a kernel interface with `--tap`). The TAP backend needs root and the `tun` driver (`/dev/net/tun`); the hook scripts `vtesse-hook.sh` and `vtesse-udhcpc.script` are built into the binary. Until 1.12 a TAP-only `vtesse-tap-ARCH` was published too; its links now return 404, use `vtesse-ARCH --tap` instead.

The releases also carry the VTun server for PCs and servers, `vtund-3.0.2-x86_64`: the original VTun 3.0.2 with our patches (a refused login is logged with the device ID and the reason), see [server/README.md](server/README.md). For cameras and routers vtund is built in the OpenIPC environment when needed.

### Permanent links

There is one release, [`latest`](https://github.com/OpenIPC/vtesse/releases/latest), titled `VTesse <version>`; every push to `main` rebuilds it, and no versioned releases are kept. File names carry no version (only the VTun server's follows the VTun version), so these links never change:

| File                   | What it is                                                   | Permanent link                                                                    |
|------------------------|--------------------------------------------------------------|-----------------------------------------------------------------------------------|
| `vtesse-arm32`         | client, 32-bit ARM                                           | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-arm32>         |
| `vtesse-mipsel`        | client, MIPS little-endian                                   | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-mipsel>        |
| `vtesse-mips`          | client, MIPS big-endian                                      | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-mips>          |
| `vtesse-arm64`         | client, 64-bit ARM                                           | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-arm64>         |
| `vtesse-x86_64`        | client, PCs and servers                                      | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-x86_64>        |
| `vtesse-android.apk`   | Android app                                                  | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-android.apk>   |
| `vtund-3.0.2-x86_64`   | VTun server with our patches, PCs (follows the VTun version) | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtund-3.0.2-x86_64>   |
| `vtesse-hook.sh`       | TAP hook script (built into the binary; for `--hook FILE`)   | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-hook.sh>       |
| `vtesse-udhcpc.script` | udhcpc script for the TAP interface (built in)               | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-udhcpc.script> |
| `S99vtesse`            | init script, OpenIPC (BusyBox; `--install` writes it)        | <https://github.com/OpenIPC/vtesse/releases/latest/download/S99vtesse>            |
| `vtesse.init`          | init script, OpenWrt (procd; `--install` writes it)          | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse.init>          |

```sh
curl -fsSLO https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-arm32 &&
  chmod 755 vtesse-arm32
```

### Install as a service: OpenIPC camera, OpenWrt router, Linux PC

As root: download the binary for your `ARCH` (see the table) and let it install itself. It detects the system, copies itself to `/usr/bin/vtesse` (`/usr/local/bin` on a PC), writes `/etc/vtesse.conf` (mode 600) from the keys and the init script (OpenIPC `S99vtesse`, OpenWrt procd `vtesse.init`, systemd `vtesse.service`), and starts the service at once and at every boot. The hook scripts are built in. On OpenWrt the installation survives `sysupgrade`.

```sh
ARCH=arm32   # arm32 or mipsel on cameras, mipsel / mips / arm64 on routers, x86_64 on a PC
curl -fsSL -o /tmp/vtesse https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-$ARCH &&
  chmod 755 /tmp/vtesse &&
  /tmp/vtesse --install --host vtun.example.org --port 5555 --password secret
```

- The session name is the device ID: 12 characters `0-9A-F`, also used as the MAC. Without `--session 02AB34CD56EF` it is taken from the MAC of the interface with the default route (`id_from=`, see [Device ID](docs/usage.md#device-id)); `--id-from eth0` names the interface.
- `--password -` reads the password from standard input instead of the command line. Without `--port` the port is 5000.
- An existing `/etc/vtesse.conf` is kept: `vtesse --install` without keys only updates the binary and the init script, keys given again replace their lines. The file is plain `key=value` ([configuration file](docs/usage.md#configuration-file)).
- `vtesse --uninstall` stops the service and removes everything, `/etc/vtesse.conf` included.
- `--scripts` also writes the hook scripts to `/usr/libexec/vtesse` to be edited; `--no-start` installs without starting. All options: [Installation](docs/usage.md#installation).
- OpenWrt needs the `tun` driver for the TAP backend: `opkg update && opkg install kmod-tun` (`apk add kmod-tun` on OpenWrt 25+).

Service control: `/etc/init.d/S99vtesse start|stop|restart|status` on OpenIPC (overrides in `/etc/default/vtesse`), `/etc/init.d/vtesse start|stop` on OpenWrt, `systemctl status vtesse` on a PC.

### Keenetic router (research, [#32](https://github.com/OpenIPC/vtesse/issues/32))

Not supported yet. KeeneticOS (NDMS) is closed firmware: SSH gives only the NDMS command line, no Linux shell. Own software runs only through the **OPKG** component, which installs Entware into `/opt` on a storage volume (`opkg disk <volume> <installer URL>`), so the router needs a USB drive or an internal `storage:` partition.

Tried on a Netcraze Explorer NC-1621 (Keenetic rebrand, NDMS 5.2 beta, MIPS, 128 MB RAM, `small_flash`): the `opkg` component is available and installs (`components install opkg`, `components commit`, firmware rebuilt and the router restarts), but the model has no USB and no `storage:` volume, and Entware on the RAM disk `temp:` is refused (`unsuitable disk type`). Next: a Keenetic with a USB port (MT7621 or arm64) and an `.ipk` package with `vtesse-mipsel`/`vtesse-arm64`, an Entware init script and NDMS hooks for the TAP backend.

### Linux PC, Raspberry Pi

```sh
ARCH=x86_64   # x86_64; arm64 / arm32 on a Raspberry Pi with a 64-bit / 32-bit OS
mkdir -p ~/.local/bin
curl -fsSL -o ~/.local/bin/vtesse https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-$ARCH && chmod 755 ~/.local/bin/vtesse
# no root needed: SOCKS5 on 127.0.0.1:1080 and a forward to a camera's RTSP
~/.local/bin/vtesse --config ~/.config/vtesse/vtesse.conf --socks 1080 --forward 8554:10.78.0.10:554
```

The file has the same `key=value` lines as `/etc/vtesse.conf` (see [Build from source](#build-from-source)). For the TAP backend as a systemd service, install it as above.

### Android

Android 5.0 and later, including Android TV boxes: [`vtesse-android.apk`](https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-android.apk). No VPN permission and no VPN icon — a service with a regular notification runs the tunnel and offers SOCKS5 and port forwards on `127.0.0.1`; a player such as OpenIPC Decoder or VLC opens e.g. `rtsp://127.0.0.1:1551/...` (camera 1 of server 1). Up to three servers with up to 8 cameras each on fixed ports. The device ID is generated on first start (long press copies it) and must be added as a session on the server. Up to three server profiles, switched with one press. See [android/README.md](android/README.md).

![VTesse for Android: server profiles and cameras (example data)](docs/img/android-settings.png)

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

| Document                             | Contents                                                                                                                |
|--------------------------------------|-------------------------------------------------------------------------------------------------------------------------|
| [Usage](docs/usage.md)               | building, `vtesse-cli` options, configuration file, device ID, DHCP and routes, local proxy, error messages, probe tool |
| [VTun protocol](docs/protocol.md)    | handshake, challenge response, session flags, framing, keepalive, server behaviour that affects clients, UDP            |
| [Architecture](docs/architecture.md) | layers, `tunnel_ops_t`, VTun client, lwIP stack, proxy and flow control, repository layout                              |
| [Embedding](docs/embedding.md)       | using the libraries in another application, loop integration, example                                                   |
| [Security](docs/security.md)         | VTun authentication weaknesses, clear-text data, local proxy exposure                                                   |
| [VTun server](server/README.md)      | patched vtund builds, release binaries, patch list                                                                      |
| [Testing](docs/testing.md)           | unit tests, sanitizers, big-endian MIPS, Docker test bench and acceptance tests                                         |

## Roadmap

Stages and tasks are tracked as [GitHub issues](https://github.com/OpenIPC/vtesse/issues):

| Stage                                                                | Status     | Issues                                                                                                                                                                                                                                                                                                                                                                      |
|----------------------------------------------------------------------|------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 0. Test bench (VTun 3.0.2 in Docker, bridge, DHCP with option 121)   | ✅ Done    | [#1](https://github.com/OpenIPC/vtesse/issues/1) ✅                                                                                                                                                                                                                                                                                                                         |
| 1. CLI prototype on Linux: TCP + `type ether` + DHCP over lwIP       | ✅ Done    | [#2](https://github.com/OpenIPC/vtesse/issues/2) ✅ [#3](https://github.com/OpenIPC/vtesse/issues/3) ✅ [#4](https://github.com/OpenIPC/vtesse/issues/4) ✅ [#5](https://github.com/OpenIPC/vtesse/issues/5) ✅ [#6](https://github.com/OpenIPC/vtesse/issues/6) ✅ [#7](https://github.com/OpenIPC/vtesse/issues/7) ✅ [#8](https://github.com/OpenIPC/vtesse/issues/8) ✅ |
| 2. Local proxy: SOCKS5 and port forwarding                           | ✅ Done    | [#10](https://github.com/OpenIPC/vtesse/issues/10) ✅ [#11](https://github.com/OpenIPC/vtesse/issues/11) ✅                                                                                                                                                                                                                                                                 |
| 3. Embedded Linux: ARM/MIPS cross builds, footprint, CI              | ✅ Done    | [#12](https://github.com/OpenIPC/vtesse/issues/12) ✅ [#20](https://github.com/OpenIPC/vtesse/issues/20) ✅                                                                                                                                                                                                                                                                 |
| 3. TAP/TUN backend (kernel interface instead of lwIP, Linux targets) | ✅ Done    | [#19](https://github.com/OpenIPC/vtesse/issues/19) ✅                                                                                                                                                                                                                                                                                                                       |
| 4. Android app (no VpnService)                                       | ✅ Done    | [#13](https://github.com/OpenIPC/vtesse/issues/13) ✅                                                                                                                                                                                                                                                                                                                       |
| 5. Optional: UDP transport, TLS wrapper, Blowfish auth               | ⏸ Deferred | [#14](https://github.com/OpenIPC/vtesse/issues/14) [#15](https://github.com/OpenIPC/vtesse/issues/15) [#16](https://github.com/OpenIPC/vtesse/issues/16)                                                                                                                                                                                                                    |
| 6. Reuse of the upper layers for camex                               | ⏸ Deferred | [#17](https://github.com/OpenIPC/vtesse/issues/17)                                                                                                                                                                                                                                                                                                                          |
| Cross-cutting: tests, license                                        | ✅ Done    | [#9](https://github.com/OpenIPC/vtesse/issues/9) ✅ [#18](https://github.com/OpenIPC/vtesse/issues/18) ✅                                                                                                                                                                                                                                                                   |

✅ — closed issue. ⏸ — deferred.

## License

[The Prosperity Public License 3.0.0](LICENSE.md): free for noncommercial use, with a 30-day trial for commercial use. See [LICENSE.md](LICENSE.md) for the full terms. Third-party code keeps its own license (lwIP: BSD-3-Clause).

## Support

- Questions and bug reports: [GitHub Issues](https://github.com/OpenIPC/vtesse/issues)
- Documentation: [docs/](docs/)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
