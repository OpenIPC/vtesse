# VTesse — resource access microsystem

VTesse is a client for existing **VTun** servers (`vtund -s`) that works **without a
TUN/TAP device and without a system VPN**. The tunnel is terminated inside the
application by a userspace TCP/IP stack (lwIP); applications reach remote devices
through a local SOCKS5 proxy or port forwards on `127.0.0.1`.

- No root, no `/dev/net/tun`, no Android `VpnService` (no VPN icon).
- Targets: embedded Linux (IP cameras, routers; ARM and MIPS, including big-endian,
  uClibc/musl), Android, and later an embeddable communication module.
- VTun 3.0.x, TCP transport, `type ether` and `type tun` sessions, XOR challenge
  response, no VTun compression/encryption.
- Address and routes via DHCP through the tunnel (option 121, RFC 3442).
- Client only, outbound connections only, IPv4 only.

> **Status: prototype.** Traffic is not encrypted (see [security](docs/security.md)).

## Quick start

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

The session name is the device ID (12 characters `0-9A-F`, also used as the MAC);
credentials are read only from the configuration file.

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

| Stage | Issues |
|---|---|
| 0. Test bench (VTun 3.0.2 in Docker, bridge, DHCP with option 121) | [#1](https://github.com/compot-dev/vtesse/issues/1) |
| 1. CLI prototype on Linux: TCP + `type ether` + DHCP over lwIP | [#2](https://github.com/compot-dev/vtesse/issues/2) [#3](https://github.com/compot-dev/vtesse/issues/3) [#4](https://github.com/compot-dev/vtesse/issues/4) [#5](https://github.com/compot-dev/vtesse/issues/5) [#6](https://github.com/compot-dev/vtesse/issues/6) [#7](https://github.com/compot-dev/vtesse/issues/7) [#8](https://github.com/compot-dev/vtesse/issues/8) |
| 2. Local proxy: SOCKS5 and port forwarding | [#10](https://github.com/compot-dev/vtesse/issues/10) [#11](https://github.com/compot-dev/vtesse/issues/11) |
| 3. Embedded Linux: ARM/MIPS cross builds, footprint | [#12](https://github.com/compot-dev/vtesse/issues/12) |
| 3. Optional TAP/TUN backend (kernel interface instead of lwIP, Linux targets) | [#19](https://github.com/compot-dev/vtesse/issues/19) |
| 4. Android app (no VpnService) | [#13](https://github.com/compot-dev/vtesse/issues/13) |
| 5. Optional: UDP transport, TLS wrapper, Blowfish auth | [#14](https://github.com/compot-dev/vtesse/issues/14) [#15](https://github.com/compot-dev/vtesse/issues/15) [#16](https://github.com/compot-dev/vtesse/issues/16) |
| 6. Reuse of the upper layers for camex | [#17](https://github.com/compot-dev/vtesse/issues/17) |
| Cross-cutting: tests and CI, license | [#9](https://github.com/compot-dev/vtesse/issues/9) [#18](https://github.com/compot-dev/vtesse/issues/18) |

## License

[The Prosperity Public License 3.0.0](LICENSE.md) — free for noncommercial use, a
thirty-day trial for commercial use. Third-party code keeps its own license
(lwIP: BSD-3-Clause).
