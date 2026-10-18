# Usage

- [Building](#building)
- [vtesse-cli](#vtesse-cli)
- [TAP backend](#tap-backend)
- [Installation](#installation)
- [Configuration file](#configuration-file)
- [Device ID](#device-id)
- [Addressing (DHCP)](#addressing-dhcp)
- [Local proxy](#local-proxy)
- [UDP relay (RTP from cameras)](#udp-relay-rtp-from-cameras)
- [Errors](#errors)
- [Probe tool](#probe-tool)

## Building

C99 + POSIX, plain `make`.

```sh
make            # build/libnetcore.a, build/libvtesse.a, build/vtesse-cli
make test       # unit tests and client scenarios against a scripted server
make LWIP=0     # TAP backend only: no userspace stack and proxy
make LWIP_PROFILE=medium   # lwIP memory profile: large (default), medium, small
make SOCKS=0 UDP_RELAY=0 TAP=0   # leave out optional modules (default: all in)
make CROSS_COMPILE=mipsel-linux-musl- STATIC=1      # cross compile, static binary
```

Static musl binaries for cameras and routers are built by
[`ci/build-musl.sh`](../ci/build-musl.sh) (also used by CI):

```sh
QEMU=1 ci/build-musl.sh arm32 dist     # downloads the toolchain, runs the tests under qemu
```

| Target   | Toolchain                                        | Devices                                                                 |
|----------|--------------------------------------------------|-------------------------------------------------------------------------|
| `arm32`  | OpenIPC `hisilicon-hi3516cv100` (arm musleabi)   | 32-bit ARM cameras                                                      |
| `mipsel` | OpenWrt 23.05 `ramips/mt7621` (musl, soft-float) | Ingenic cameras, little-endian MIPS routers (MediaTek MT7620/7621/7628) |
| `arm64`  | Bootlin aarch64 musl                             | 64-bit ARM                                                              |
| `mips`   | OpenWrt 23.05 `ath79/generic` (musl, soft-float) | big-endian MIPS routers (Atheros ath79 and others)                      |

MIPS builds are soft-float: router SoCs have no FPU and their kernels often lack FPU
emulation, so hard-float binaries end with `Illegal instruction` there (found on an
OpenWrt MT7620 router, #30).
| `x86_64` | Bootlin x86-64 musl | PCs, servers |

Optional modules (#29), `vtesse-cli --version` lists what a binary contains:

| Switch        | Leaves out                                                          | arm32/x86_64 code saved |
|---------------|---------------------------------------------------------------------|-------------------------|
| `LWIP=0`      | lwIP stack, port forwards, SOCKS5, UDP relay (TAP/TUN backend only) | ~75 %                   |
| `SOCKS=0`     | SOCKS5 server (port forwards stay)                                  | ~1.7 KB                 |
| `UDP_RELAY=0` | UDP relay                                                           | ~2 KB                   |
| `TAP=0`       | TAP/TUN backend (kernel interface)                                  | ~5 KB                   |

A disabled feature is refused with a clear message (`--socks: built without SOCKS5`).
Release binaries contain everything, one per platform; the `LWIP=0` build is not published
(build it with `make LWIP=0` when the flash is tight).

Each target gives `vtesse-<target>` (lwIP and TAP backends, `--tap`); x86_64 also `vtund-3.0.2-x86_64`, the VTun server with our patches
([server/README.md](../server/README.md)). Use musl builds on devices: a static glibc binary cannot resolve host names
without the NSS libraries of the same glibc. Prebuilt binaries are on the
[releases page](https://github.com/OpenIPC/vtesse/releases) (`latest` is rebuilt
from every push to `main`).

## vtesse-cli

```
vtesse-cli [--config FILE] [--seconds N] [-v]
           [--socks PORT] [--forward LPORT:IP:RPORT]... [--listen ADDR]
           [--fetch http://IP[:PORT]/PATH] [--udp-relay PORT[:LOCAL_PORT]]...
           [--ip A.B.C.D/P [--gw A.B.C.D]]
vtesse-cli [--config FILE] [--seconds N] --tap [--ifname NAME] [--hook SCRIPT|none]
vtesse --install [--host SERVER] [--port N] [--password PASSWORD|-]
                 [--session ID | --id-from IFACE] [--ip A.B.C.D/P] [--gw A.B.C.D]
                 [--scripts] [--no-start] [--system SYSTEM] [--root DIR]
vtesse --uninstall [--system SYSTEM] [--root DIR]
```

| Option                           | Meaning                                                                                                                                                       |
|----------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `--config FILE`                  | configuration file, default `~/.config/vtesse/probe.conf`                                                                                                     |
| `--seconds N`                    | stop after N seconds (default: run until SIGINT/SIGTERM)                                                                                                      |
| `--id-from IFACE`                | [device ID](#device-id) = MAC of the local interface `IFACE` (overrides `session`/`id_from` of the file)                                                      |
| `--socks PORT`                   | SOCKS5 server on `LISTEN:PORT`                                                                                                                                |
| `--forward LPORT:IP:RPORT`       | TCP forward `LISTEN:LPORT → IP:RPORT` through the tunnel (repeatable, up to 16)                                                                               |
| `--listen ADDR`                  | address of the local listeners, default `127.0.0.1`                                                                                                           |
| `--udp-relay PORT[:LOCAL_PORT]`  | [UDP relay](#udp-relay-rtp-from-cameras): datagrams to the tunnel address on `PORT` go to `127.0.0.1:LOCAL_PORT` (default the same port); repeatable, up to 3 |
| `--fetch URL`                    | one HTTP GET through the stack once an address is bound (diagnostics)                                                                                         |
| `--ip A.B.C.D/P`, `--gw A.B.C.D` | [manual IPv4](#manual-ipv4-type-tun) instead of DHCP; override `ip`/`gw` of the file (`--debug-ip`/`--debug-gw` are accepted as old names)                    |
| `--tap`                          | [TAP backend](#tap-backend): kernel interface instead of lwIP (Linux, root)                                                                                   |
| `--ifname NAME`                  | interface name for `--tap`, default `vtesse%d`                                                                                                                |
| `--hook SCRIPT`                  | script run on interface events for `--tap`; default: the [built-in hook](#tap-backend), `none`: no hook                                                       |
| `--install`, `--uninstall`       | [install](#installation) the binary as a service, or remove it                                                                                                |

The CLI brings the tunnel and the userspace stack up, prints link events, the DHCP
address and routes, runs the local proxy and prints proxy statistics on exit. Exit
code 0 means the tunnel was up at the end (and `--fetch`, if given, succeeded).

```sh
vtesse-cli --socks 1080 --forward 8554:10.78.0.10:554
curl --socks5 127.0.0.1:1080 http://10.78.0.10/
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/stream
```

```
[   0.504] up: VTUN server ver 3.X 10/13/2026, flags <TeK>
[   3.512] address 10.77.0.169 mask 255.255.255.0, option 3 10.77.0.1 (ignored: classless routes present)
[   3.512]   route 10.78.0.0/24 via 10.77.0.1
[   3.512]   route 10.79.0.0/24 via 10.77.0.1
[   3.522] fetch 10.78.0.10:80/: ok, 205 bytes, "HTTP/1.0 200 OK"
```

## TAP backend

`--tap` passes tunnel frames to a kernel interface (`/dev/net/tun`) instead of the
userspace stack. Every program on the device then uses the tunnel, including
**inbound** connections to the device's own services. Requires root or
`CAP_NET_ADMIN` and the `tun` driver; not available on Android.

- The interface is created on the first tunnel `UP`: TAP for `type ether`, TUN for
  `type tun`; MAC = device ID, MTU 1500, IPv6 disabled on it.
- It is kept across reconnects; while the tunnel is down the carrier is off (which
  also clears the kernel's ARP neighbours, see
  [protocol](protocol.md#server-behaviour-that-affects-clients)), so addresses and
  routes survive a reconnect.
- Addressing is done by the system: a hook runs `SCRIPT <event>` with
  `VTESSE_EVENT`, `VTESSE_IFNAME`, `VTESSE_LINK` (`ether`/`tun`) and
  `VTESSE_SESSION` in the environment; events: `up` (interface created), `reconnect`,
  `down`, `stop`.

The hook scripts are built into the binary (#52). Without `--hook` it runs
`<prefix>/libexec/vtesse/vtesse-hook.sh` next to its `<prefix>/bin` if that file exists
(`vtesse --install --scripts` writes it, to be edited), otherwise its built-in copy of
[`scripts/vtesse-hook.sh`](../scripts/vtesse-hook.sh); the binary itself then serves as
the `udhcpc` script (called with `VTESSE_RUN=udhcpc-script`, it runs its built-in copy of
[`scripts/vtesse-udhcpc.script`](../scripts/vtesse-udhcpc.script)). `--hook FILE` runs
that file, `--hook none` runs no hook. The hook starts BusyBox `udhcpc` with
`vtesse-udhcpc.script`, which

- sets the address and installs the option 121 (or 249) routes via the interface;
- **never touches `/etc/resolv.conf`** — stock DHCP scripts replace the device's
  resolver with the tunnel DNS, which may be unreachable and breaks resolving the
  VTun server itself (found on the bench);
- installs a default route through the tunnel only with `VTESSE_DEFAULT_ROUTE=1` — it
  would also capture the connection to the VTun server unless a host route to the
  server exists.

`dhclient` is a fallback of a hook file only; its stock script has the two problems above. The built-in hook needs BusyBox `udhcpc` (on a PC: the `udhcpc` package) and refuses to fall back; `--install` warns on a PC without it.

```sh
vtesse-cli --config /etc/vtesse.conf --tap --ifname vtesse0
```

## Installation

`vtesse --install` installs the running binary as a service (#50, #51, #52):

```sh
curl -fsSL -o /tmp/vtesse https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-arm32 &&
  chmod 755 /tmp/vtesse && /tmp/vtesse --install --host vtun.example.org --password secret
vtesse --uninstall
```

1. It detects the system, or takes `--system`:

   | System                 | Detected by                            | Binary                  | Service                                                                           | Hook copies (`--scripts`)   |
   |------------------------|----------------------------------------|-------------------------|-----------------------------------------------------------------------------------|-----------------------------|
   | `openwrt`              | `/etc/openwrt_release`                 | `/usr/bin/vtesse`       | `/etc/init.d/vtesse` (procd), `/etc/rc.d/S99vtesse`, `/lib/upgrade/keep.d/vtesse` | `/usr/libexec/vtesse`       |
   | `openipc`              | `OPENIPC_VERSION` in `/etc/os-release` | `/usr/bin/vtesse`       | `/etc/init.d/S99vtesse`                                                           | `/usr/libexec/vtesse`       |
   | `systemd` (a Linux PC) | `/run/systemd/system`                  | `/usr/local/bin/vtesse` | `/etc/systemd/system/vtesse.service`                                              | `/usr/local/libexec/vtesse` |
   | `busybox`              | `/etc/init.d`                          | `/usr/bin/vtesse`       | `/etc/init.d/S99vtesse`                                                           | `/usr/libexec/vtesse`       |

2. It writes `/etc/vtesse.conf` (mode 600) from the keys `--host`, `--port`, `--password`,
   `--session` or `--id-from`, `--ip`, `--gw` ([configuration file](#configuration-file)).
   An existing file keeps its other lines and gets the given keys replaced; a new one
   needs at least `--host` and `--password` and gets `port=5000` and, without `--session`,
   `id_from=` the interface of the default route (it must have a MAC; with `--root` the
   identity must be given, as this machine's interfaces say nothing about the image). `--password -` reads the password from
   standard input instead of the command line (which other users can see in `ps`).
   Without a configuration and without keys nothing is installed.
3. It copies itself, writes the service file from its built-in copy and (re)starts the
   service; `--no-start` only installs. Only the binary, the service file and the
   configuration are written: the hook scripts run from the binary. `--scripts` also
   writes them to the libexec directory, where the binary finds them.
4. On OpenWrt `sysupgrade` keeps only the changed files under `/etc`: the keep list in
   `/lib/upgrade/keep.d/vtesse` carries the binary and the scripts over.
5. Hook scripts of 1.13 and older in `/usr/share/vtesse` are moved to the libexec
   directory, so a hand-edited copy keeps working (`--scripts` does not overwrite a moved
   file); delete them to use the built-in ones. A `--hook /usr/share/vtesse/...` in
   `/etc/default/vtesse` is changed to the new path.
6. Values are checked against the limits of the service: no spaces around a value, a
   password of up to 127 characters, a session name of up to 42 characters without
   spaces or colons. A kept `/etc/vtesse.conf` is set to mode 600 as well.

Running `--install` again updates the binary and the service file (the upgrade path)
and the given keys. `--uninstall` stops the service and removes everything `--install`
may have written, in any of the layouts, `/etc/vtesse.conf` and `/etc/default/vtesse`
included; if the service does not stop, nothing is removed. On a clean system it is not
an error. `--root DIR` does the same below `DIR` without touching the service
(tests, preparing a firmware image).

The service files are also in [`scripts/`](../scripts): `S99vtesse`
(`start|stop|restart|status`, overrides in `/etc/default/vtesse`, loads the `tun`
module), `vtesse.init` (procd, respawn) and `vtesse.service` (systemd, restart).

## Configuration file

`key=value` lines, `#` starts a comment. Credentials are read only from this file,
never from the command line; keep it outside the repository with mode 600.

| Key                            | Meaning                                                                                                                                    |
|--------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
| `host`                         | VTun server name or IPv4 address                                                                                                           |
| `port`                         | VTun server TCP port                                                                                                                       |
| `session`                      | session name = [device ID](#device-id)                                                                                                     |
| `id_from`                      | instead of `session`: take the device ID from the MAC of this interface, e.g. `eth0`                                                       |
| `udp_relay`                    | `PORT[:LOCAL_PORT]`, repeatable: [UDP relay](#udp-relay-rtp-from-cameras) (lwIP backend)                                                   |
| `ip`, `gw`, `route`            | [manual IPv4](#manual-ipv4-type-tun): `ip=A.B.C.D/P`, optional `gw=A.B.C.D`, `route=NET/P` (repeatable, up to 16); required for `type tun` |
| `password`                     | session password                                                                                                                           |
| `auth`                         | challenge response, `xor` (default; servers built without OpenSSL)                                                                         |
| `socks_user`, `socks_password` | optional SOCKS5 username/password (RFC 1929)                                                                                               |

## Device ID

The session name is the device ID: a MAC address written as **12 characters, upper
case, `0-9A-F`, no separators** (e.g. `02AB34CD56EF`). The same value is used as the
client's Ethernet MAC in `type ether` sessions.

- Validation: exactly 12 characters from `0-9A-F`; the least significant bit of the
  first byte must be 0 (a multicast address cannot be a source MAC). The CLI accepts
  other session names too and then derives a locally administered MAC (with a
  warning).
- Embedded Linux: the device's own MAC, so the ID needs no configuration per
  device: `id_from=eth0` in the file (or `--id-from eth0`) reads
  `/sys/class/net/eth0/address` and uses it as the session name, e.g.
  `a4:5e:60:12:34:5c` → `A45E6012345C`. The CLI prints `device ID … (MAC of eth0)`
  and refuses a multicast or all-zero MAC. `session` and `id_from` are mutually
  exclusive in the file. With `--tap` the tunnel interface gets the same MAC as
  `eth0`; they are different L2 segments, so this is harmless.
- Android: the hardware MAC is not available to apps — a random locally administered
  unicast ID is generated once and stored (second character `2`, `6`, `A` or `E`),
  or the ID is entered manually. A session with that name must exist on the server.

## Addressing (DHCP)

For `type ether` sessions bridged to a segment with a DHCP server, VTesse runs the
lwIP DHCP client through the tunnel:

- options used: 1 (netmask), 3 (router), 51/58/59 (lease), **121** (classless static
  routes, RFC 3442), 249 (Microsoft variant of 121); 6 (DNS) and 28 are informational;
- per **RFC 3442**, when option 121 is present option 3 is ignored → there is no
  default route through the tunnel; only the connected subnet and the 121 routes are
  reachable, other destinations are refused by the proxy (SOCKS5 code 3);
- `chaddr` is the device ID, so a reconnect gets the same lease (client-id, option 61,
  is not sent);
- the server may probe the offered address with ARP before answering (~3 s);
- leases are not released on tunnel loss; on reconnect the previous address is
  requested again;
- until an address is bound the proxy refuses connections (SOCKS5 code 3).

## Manual IPv4 (type tun)

A `type tun` session carries bare IPv4 packets (no Ethernet, no ARP), so there is no
DHCP: the address and routes are configured. The server side typically has
`ip "addr add 10.76.0.1 peer 10.76.0.2 dev %%"`; the client then uses:

```
ip=10.76.0.2/32
gw=10.76.0.1
route=10.78.0.0/24       # networks behind the server, repeatable
```

- `route` lines are the reachable networks, like option 121; without them and with
  `gw` there is a default route through the tunnel.
- lwIP backend: applied when the tunnel comes up; the CLI prints
  `address … (manual)`. The same keys work on a `type ether` session instead of DHCP.
- TAP backend (`--tap`): a TUN interface is created; `vtesse-hook.sh` gets
  `VTESSE_IP`, `VTESSE_GW`, `VTESSE_ROUTES` and sets the address and routes
  (`ip addr`/`ip route`) instead of starting DHCP. A default route via `gw` is added
  only with `VTESSE_DEFAULT_ROUTE=1`.
- Acceptance: `bench/bench.sh tun-test` (both backends).

## Local proxy

Host sockets on `127.0.0.1` (configurable) relayed to TCP connections through the
tunnel. Outbound TCP and IPv4 only.

- **SOCKS5** (RFC 1928) `CONNECT`; no authentication by default, optional
  username/password (RFC 1929). Reply codes:

  | Code                           | When                                            |
  |--------------------------------|-------------------------------------------------|
  | `3` network unreachable        | no route via the tunnel, or no DHCP address yet |
  | `4` host unreachable           | connect timeout (15 s)                          |
  | `5` connection refused         | the destination port is closed                  |
  | `7` command not supported      | anything but `CONNECT`                          |
  | `8` address type not supported | host names (no DNS in the stack) and IPv6       |

  A domain field holding an IPv4 literal is accepted (`socks5h` clients).
- **Port forwards** `LPORT:IP:RPORT` for players without proxy support (RTSP 554,
  HTTP 80). RTSP must use RTP over TCP (interleaved); RTP over UDP cannot pass a TCP
  forward. A forward to an unreachable destination closes the local connection.
- At most 24 connections at a time (bounded by lwIP TCP PCBs); further connections
  are accepted and closed.
- Any local process can use an unauthenticated proxy on `127.0.0.1` — set
  `socks_user` / `socks_password` where that matters.

## UDP relay (RTP from cameras)

A camera can push its video as RTP over UDP instead of being asked over RTSP (OpenIPC
Majestic `outgoing`). The port forwards are TCP only, so VTesse has a separate UDP
relay (#28): datagrams sent to the client's **tunnel address** on a port are passed,
unchanged, to `127.0.0.1` on the device, where a player listens.

```sh
vtesse-cli --config /etc/vtesse.conf --udp-relay 1559
#   address 10.77.0.169 ...
#   udp relay 10.77.0.169:1559 -> 127.0.0.1:1559
```

On the camera (`/etc/majestic.yaml`, then restart Majestic), with the tunnel address the
client printed. The key depends on the Majestic version — the camera's
`/api/v1/config.schema.json` shows which one it has:

```yaml
outgoing:                       # one destination (e.g. Majestic of 2025)
  enabled: true
  server: udp://10.77.0.169:1559
```

```yaml
outgoing:                       # a list (newer Majestic)
  servers:
    - udp://10.77.0.169:1559
```

The Majestic log then shows `Outgoing RTP stream udp://…`. Restarting Majestic over SSH
on OpenIPC firmware: keep the environment of the boot scripts, otherwise the sensor
may not be found and video stops (`Cannot start SDK`):
`SENSOR=$(fw_printenv -n sensor) /etc/init.d/S95majestic restart` (or restart from
the web UI, or reboot).

Player: `udp://@:1559` (OpenIPC Decoder: a `udp://` slot or its U cell with port 5600
via `--udp-relay 1559:5600`).

- One direction only; nothing is sent back to the camera. Up to 3 relays.
- Inside the TCP tunnel datagrams arrive reliably and in order (TCP retransmits);
  one is dropped only if the local socket's buffer is full. Large datagrams arrive
  as IP fragments and are reassembled.
- The tunnel address is stable: the DHCP lease is kept and requested again after a
  reconnect, so the camera's setting stays valid.
- lwIP backend only: with `--tap` the kernel receives UDP on the interface itself.
- Statistics at exit: `udp relay :1559: datagrams N, B bytes, dropped D`.
- Android: per-server option on port S559 (1559, 2559, 3559), see
  [android/README.md](../android/README.md).
- Acceptance: `bench/bench.sh udp-test`.

## Errors

Link events printed by the CLI (`down` = retried automatically with backoff 1–30 s,
`failed` = final):

| Message                                                      | Kind   | Meaning                                                                                                    |
|--------------------------------------------------------------|--------|------------------------------------------------------------------------------------------------------------|
| wrong session name or password                               | failed | server replied `ERR` (unknown session, wrong password, wrong auth mode, or session locked with `multi no`) |
| compression is enabled on the server                         | failed | session flags contain `C`/`L`                                                                              |
| encryption is enabled on the server                          | failed | session flags contain `E`                                                                                  |
| unsupported transport (UDP)                                  | failed | session flags contain `U`                                                                                  |
| unsupported session type                                     | failed | `type tty` / `type pipe`                                                                                   |
| protocol error (not a VTun server?)                          | failed | unexpected greeting or flags                                                                               |
| closed by server (possibly taken over by another connection) | down   | `CONN_CLOSE` or EOF, e.g. another client logged in with the same session and `multi killold`               |
| no response from server (keepalive)                          | down   | 5 keepalive intervals without any frame                                                                    |
| timeout                                                      | down   | a handshake step timed out                                                                                 |
| cannot resolve / cannot connect to server                    | down   | DNS or TCP connect failure                                                                                 |

## Probe tool

`tools/vtun_probe.py` — Python 3, no dependencies. Performs the handshake (XOR
auth), prints the greeting and flags, answers keepalives, optionally sends one
DHCPDISCOVER through a `type ether` session and decodes the offer (including
option 121), then closes with `VTUN_CONN_CLOSE`.

```sh
tools/vtun_probe.py [--config ~/.config/vtesse/probe.conf] [--dhcp] [--delay S] [--seconds N]
```

It reads the same configuration file (`host`, `port`, `session`, `password`). With
`multi killold`, probing with a production session name kicks the real device off —
use a dedicated test session.
