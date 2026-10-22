# Usage

- [Building](#building)
- [vtesse-cli](#vtesse-cli)
- [Inbound forwards](#inbound-forwards)
- [Installation](#installation)
- [U-Boot environment (helpdesk access)](#u-boot-environment-helpdesk-access)
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
make LWIP_PROFILE=medium   # lwIP memory profile: large (default), medium, small
make SOCKS=0 UDP_RELAY=0   # leave out optional modules (default: all in)
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

| Switch        | Leaves out                         | arm32/x86_64 code saved |
|---------------|------------------------------------|-------------------------|
| `SOCKS=0`     | SOCKS5 server (port forwards stay) | ~1.7 KB                 |
| `UDP_RELAY=0` | UDP relay                          | ~2 KB                   |

A disabled feature is refused with a clear message (`--socks: built without SOCKS5`).
Release binaries contain everything, one per platform. The TAP backend (`--tap`, a kernel
interface) and the `LWIP=0` and `TAP=0` switches were removed in 2.0.0 (#72).

Each target gives `vtesse-<target>`; x86_64 also `vtund-3.0.2-x86_64`, the VTun server with our patches
([server/README.md](../server/README.md)). Use musl builds on devices: a static glibc binary cannot resolve host names
without the NSS libraries of the same glibc. Prebuilt binaries are on the
[releases page](https://github.com/OpenIPC/vtesse/releases) (`latest` is rebuilt
from every push to `main`).

## vtesse-cli

```
vtesse-cli [--config FILE] [--seconds N] [-v]
           [--socks PORT] [--forward LPORT:IP:RPORT]... [--listen ADDR]
           [--accept PORT[:IP:PORT]]...
           [--fetch http://IP[:PORT]/PATH] [--udp-relay PORT[:LOCAL_PORT]]...
           [--ip A.B.C.D/P|learn [--gw A.B.C.D]]
vtesse --install [--host SERVER] [--port N] [--password PASSWORD|-]
                 [--session ID | --id-from IFACE] [--ip A.B.C.D/P] [--gw A.B.C.D]
                 [--no-start] [--system SYSTEM] [--root DIR]
vtesse --uninstall [--system SYSTEM] [--root DIR]
```

| Option                           | Meaning                                                                                                                                                                                                                       |
|----------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `--config FILE`                  | configuration file, default `~/.config/vtesse/probe.conf`                                                                                                                                                                     |
| `--seconds N`                    | stop after N seconds (default: run until SIGINT/SIGTERM)                                                                                                                                                                      |
| `--id-from IFACE`                | [device ID](#device-id) = MAC of the local interface `IFACE` (overrides `session`/`id_from` of the file)                                                                                                                      |
| `--socks PORT`                   | SOCKS5 server on `LISTEN:PORT`                                                                                                                                                                                                |
| `--forward LPORT:IP:RPORT`       | TCP forward `LISTEN:LPORT → IP:RPORT` through the tunnel (repeatable, up to 16)                                                                                                                                               |
| `--listen ADDR`                  | address of the local listeners, default `127.0.0.1`                                                                                                                                                                           |
| `--accept PORT[:IP:PORT]`        | [inbound forward](#inbound-forwards): connections from the tunnel to `PORT` of the tunnel address go to `IP:PORT` on this machine (default: the same port on the address of the default-route interface); repeatable, up to 4 |
| `--udp-relay PORT[:LOCAL_PORT]`  | [UDP relay](#udp-relay-rtp-from-cameras): datagrams to the tunnel address on `PORT` go to `127.0.0.1:LOCAL_PORT` (default the same port); repeatable, up to 3                                                                 |
| `--fetch URL`                    | one HTTP GET through the stack once an address is bound (diagnostics)                                                                                                                                                         |
| `--ip A.B.C.D/P`, `--gw A.B.C.D` | [manual IPv4](#manual-ipv4-type-tun) instead of DHCP; override `ip`/`gw` of the file (`--debug-ip`/`--debug-gw` are accepted as old names)                                                                                    |
| `--ip learn`                     | `type tun`: the address is learnt from the first packet the server sends ([manual IPv4](#manual-ipv4-type-tun))                                                                                                               |
| `--show-config`                  | print the settings in effect (file, [U-Boot environment](#u-boot-environment-helpdesk-access), defaults) and exit                                                                                                             |
| `--no-env`                       | ignore the U-Boot environment                                                                                                                                                                                                 |
| `--install`, `--uninstall`       | [install](#installation) the binary as a service, or remove it                                                                                                                                                                |

The CLI brings the tunnel and the userspace stack up, prints link events, the DHCP
address and routes, runs the local proxy and the inbound forwards and prints proxy
statistics on exit. There is no kernel interface: the TAP backend (`--tap`, `--ifname`,
`--hook`) was removed in 2.0.0 (#72), and those options are refused with that reason. Exit
code 0 means the tunnel was up at the end (and `--fetch`, if given, succeeded).

Before it starts, the client waits for a default route (any interface): without one
there is nothing to connect to. The wait has no time limit and cannot be turned off;
it prints `waiting for a default route`, then a line every minute, and
`default route present after N s` when one appears. SIGINT/SIGTERM end the wait
(exit code 0). `--show-config` and `--install` do not wait. At boot this covers an
uplink that gets its route after the service starts (Wi-Fi with a background DHCP
client).

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

## Inbound forwards

Without a kernel interface the device's own services (web interface, ssh, RTSP) are
not on the tunnel by themselves. `accept=PORT[:IP:PORT]` (or `--accept`, #67) makes the
stack listen on `PORT` of the tunnel address and relays every connection from the
tunnel to a host socket:

```
accept=80                    # tunnel address:80 -> the device's LAN address:80
accept=22
accept=8554:127.0.0.1:554    # explicit target
```

- Without `IP:PORT` the target is the same port on the address of the default-route
  interface, looked up at each connection: the device's own LAN address. Its services
  see a LAN client, so a web interface that trusts `127.0.0.1` still asks for the
  password. `127.0.0.1` has to be named explicitly.
- Up to 4 lines; command-line `--accept` come before the file's. Inbound connections
  share the proxy's connection limit (24); over it, and when the local port refuses or
  does not answer within 15 s, the connection from the tunnel is reset.
- Each connection is logged (`accept 80 -> 192.168.1.10:80: connected`, or the error)
  and counted in the proxy statistics.
- The service: `vtesse --install` writes `accept=80`, `accept=22` and `accept=554` to a
  configuration without `accept` lines, and a client started from the
  [U-Boot environment](#u-boot-environment-helpdesk-access) without `accept` lines opens
  the same ports: the server reaches the camera's web interface, ssh and RTSP, as the
  TAP backend of 1.x gave it. `accept=none` (or `--accept none`) turns that off: no
  inbound forwards, and `--install` adds nothing; it cannot be combined with other
  `accept` lines.
- On a `type tun` session the server needs the client's address before it can connect:
  set it with `ip=`, or let the client learn it from the first connection
  (`ip=learn`, [manual IPv4](#manual-ipv4-type-tun)).

## Installation

`vtesse --install` installs the running binary as a service (#50, #51, #52):

```sh
curl -fsSL -o /tmp/vtesse https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-arm32 &&
  chmod 755 /tmp/vtesse && /tmp/vtesse --install --host vtun.example.org --password secret
vtesse --uninstall
```

1. It detects the system, or takes `--system`:

   | System                 | Detected by                                              | Binary                  | Service                                                                           |
   |------------------------|----------------------------------------------------------|-------------------------|-----------------------------------------------------------------------------------|
   | `openwrt`              | `/etc/openwrt_release`                                   | `/usr/bin/vtesse`       | `/etc/init.d/vtesse` (procd), `/etc/rc.d/S99vtesse`, `/lib/upgrade/keep.d/vtesse` |
   | `openipc`              | `OPENIPC_VERSION` in `/etc/os-release`                   | `/usr/bin/vtesse`       | `/etc/init.d/S99vtesse`                                                           |
   | `systemd` (a Linux PC) | `/run/systemd/system`                                    | `/usr/local/bin/vtesse` | `/etc/systemd/system/vtesse.service`                                              |
   | `busybox`              | BusyBox init: `/etc/init.d/rcS` runs `S??*` (not OpenRC) | `/usr/bin/vtesse`       | `/etc/init.d/S99vtesse`                                                           |

2. It writes `/etc/vtesse.conf` (mode 600) from the keys `--host`, `--port`, `--password`,
   `--session` or `--id-from`, `--ip`, `--gw` ([configuration file](#configuration-file)).
   An existing file keeps its other lines and gets the given keys replaced; a new one
   needs at least `--host` and `--password` and gets `port=5000` and, without `--session`,
   `id_from=` the interface of the default route (it must have a MAC; with `--root` the
   identity must be given, as this machine's interfaces say nothing about the image). `--password -` reads the password from
   standard input instead of the command line (which other users can see in `ps`).
   Without a configuration and without keys nothing is installed. A configuration
   without `accept` lines gets `accept=80`, `accept=22` and `accept=554`
   ([inbound forwards](#inbound-forwards)).
3. It copies itself, writes the service file from its built-in copy and (re)starts the
   service; `--no-start` only installs. Only the binary, the service file and the
   configuration are written.
4. On OpenWrt `sysupgrade` keeps only the changed files under `/etc`: the keep list in
   `/lib/upgrade/keep.d/vtesse` carries the binary and the init script over.
5. Updating a 1.x installation removes the hook scripts of the TAP backend (in
   `/usr/libexec/vtesse`, `/usr/local/libexec/vtesse`, and `/usr/share/vtesse` up to
   1.13). An `/etc/default/vtesse` that still passes `--tap`, `--ifname` or `--hook` is
   reported and left alone: the client refuses those options, so the service does not
   start until they are removed. `--scripts` is refused.
6. Values are checked against the limits of the service: no spaces around a value, a
   password of up to 127 characters, a session name of up to 42 characters without
   spaces or colons. A kept `/etc/vtesse.conf` is set to mode 600 as well.

Running `--install` again updates the binary and the service file (the upgrade path)
and the given keys. `--uninstall` stops the service and removes everything `--install`
may have written, in any of the layouts, `/etc/vtesse.conf` and `/etc/default/vtesse`
included; if the service does not stop, nothing is removed. On a clean system it is not
an error. `--root DIR` does the same below `DIR` without touching the service
(tests, preparing a firmware image); an empty `DIR` or `/` is refused. A system with
`/etc/init.d` but another init (OpenRC, sysvinit) is not guessed: name it with `--system`.

The service files are also in [`scripts/`](../scripts): `S99vtesse`
(`start|stop|restart|status`, overrides in `/etc/default/vtesse`), `vtesse.init` (procd,
respawn) and `vtesse.service` (systemd, restart).

## U-Boot environment (helpdesk access)

For quick remote access by support staff, the settings can live in the U-Boot
environment instead of a file, the way OpenIPC keeps its VTun server there (#64):

```sh
fw_setenv vtesse vtun.example.org     # on: the service starts at boot with this server
fw_setenv vtesse                      # off
```

With only `vtesse` set, the client uses the defaults of OpenIPC's VTun client
([`tunnel`](https://github.com/OpenIPC/firmware/blob/master/general/package/vtund-openipc/files/tunnel)),
so it reaches the same server with the same credentials:

- port `5000`;
- the interface of the default route (it must have a MAC), its MAC in upper case;
- session = that MAC without colons (the [device ID](#device-id));
- password = `echo "$MAC" | sha1sum`: SHA-1 of the MAC with colons **and a newline**.

The client first [waits for a default route](#vtesse-cli); a route through an interface
without a MAC (PPPoE, a tunnel) means `eth0`. `vtesse_session` and `vtesse_id_from`
exclude each other; with both set the client refuses to start.

Optional variables override these and the file, one key at a time:

| Variable                            | Key                   |
|-------------------------------------|-----------------------|
| `vtesse`                            | `host`                |
| `vtesse_port`                       | `port`                |
| `vtesse_session` / `vtesse_id_from` | `session` / `id_from` |
| `vtesse_password`                   | `password`            |
| `vtesse_ip`, `vtesse_gw`            | `ip`, `gw`            |

Everything else (`route`, `udp_relay`, `accept`, SOCKS5 users, `auth`) is set only in
`/etc/vtesse.conf`. Without `accept` lines there, a client started with `vtesse` set
opens ports 80, 22 and 554 to the server ([inbound forwards](#inbound-forwards)): the
camera's web interface, ssh and RTSP on its own LAN address. `vtesse_ip=learn` takes
the address from the first packet of a `type tun` session. The order for each key: command line, U-Boot environment,
`/etc/vtesse.conf`, the VTun defaults (only with `vtesse` set). The variables are read
with `fw_printenv`; without it (a PC, most OpenWrt routers) this step is skipped.
`--no-env` ignores them; `--show-config` prints the result.

- The service scripts start without `/etc/vtesse.conf` when `vtesse` is set.
- `vtesse --install` without a configuration installs the binary and the service when
  `vtesse` is set; keys given to `--install` are also stored with `fw_setenv` when it is
  available (`vtesse_session` and `vtesse_id_from` replace each other), and a failed
  `fw_setenv` stops the install. Without `fw_setenv` the keys go to `/etc/vtesse.conf`
  and the server stays in the environment; a key whose variable is already set is
  refused, as the variable would override it. While `vtesse` is set, the file needs no
  server or password and gets no defaults. With `--root` this machine's environment is
  not used.
- `vtesse --uninstall` leaves the variables alone, so a reinstall picks them up again.

Anyone with root or a flash dump can read `vtesse_password`, and the default password
is derived from the MAC: this mode is for helpdesk access and is no stronger than VTun
on OpenIPC ([security](security.md)).

## Configuration file

`key=value` lines, `#` starts a comment. Credentials are read only from this file,
never from the command line; keep it outside the repository with mode 600.

| Key                            | Meaning                                                                                                                                                  |
|--------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------|
| `host`                         | VTun server name or IPv4 address                                                                                                                         |
| `port`                         | VTun server TCP port                                                                                                                                     |
| `session`                      | session name = [device ID](#device-id)                                                                                                                   |
| `id_from`                      | instead of `session`: take the device ID from the MAC of this interface, e.g. `eth0`                                                                     |
| `udp_relay`                    | `PORT[:LOCAL_PORT]`, repeatable: [UDP relay](#udp-relay-rtp-from-cameras)                                                                                |
| `accept`                       | `PORT[:IP:PORT]`, repeatable, up to 4: [inbound forwards](#inbound-forwards); `none`: none at all, not even the service defaults                         |
| `ip`, `gw`, `route`            | [manual IPv4](#manual-ipv4-type-tun): `ip=A.B.C.D/P` or `ip=learn`, optional `gw=A.B.C.D`, `route=NET/P` (repeatable, up to 16); required for `type tun` |
| `password`                     | session password                                                                                                                                         |
| `auth`                         | challenge response, `xor` (default; servers built without OpenSSL)                                                                                       |
| `socks_user`, `socks_password` | optional SOCKS5 username/password (RFC 1929)                                                                                                             |

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
  exclusive in the file.
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
- Applied when the tunnel comes up; the CLI prints `address … (manual)`. The same
  keys work on a `type ether` session instead of DHCP.

`ip=learn` (or `--ip learn`, #68): VTun does not hand out addresses, so with a manual
address every device needs its own configuration. A stock server keeps the address in
the session's `up {}` and routes only that address into the session, so the
destination of the first unicast IPv4 packet from the server is ours, and its source is
the server's end:

- the address (`/32`) and the peer are set before that packet enters the stack, so a
  first SYN (an [inbound forward](#inbound-forwards)) is accepted;
- broadcast, multicast and `0.0.0.0` destinations are ignored;
- after a reconnect the next packet is checked again: a changed address replaces the
  old one, the same address changes nothing;
- `route` lines apply as with a manual address; without them the default route points
  into the link (the peer). `gw` is refused (the peer is learnt);
- until the address is known, outbound use (SOCKS5, forwards) fails as without DHCP;
  the CLI prints `waiting for the first packet from the server for the address`, then
  `address … (learnt), peer …`;
- `type tun` only: on a `type ether` session the client stops (exit code 2) and DHCP is
  the way. To make the address known right after connect, the server can send a packet
  from `up {}` (e.g. `program "ping -c1 <address>"`).
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
