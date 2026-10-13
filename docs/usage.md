# Usage

- [Building](#building)
- [vtesse-cli](#vtesse-cli)
- [TAP backend](#tap-backend)
- [Configuration file](#configuration-file)
- [Device ID](#device-id)
- [Addressing (DHCP)](#addressing-dhcp)
- [Local proxy](#local-proxy)
- [Errors](#errors)
- [Probe tool](#probe-tool)

## Building

C99 + POSIX, plain `make`; cross compile with `make CC=<toolchain>-gcc`.

```sh
make            # build/libnetcore.a, build/libvtesse.a, build/vtesse-cli
make test       # unit tests and client scenarios against a scripted server
make LWIP=0     # TAP backend only: no userspace stack and proxy (~23 KiB of code)
```

## vtesse-cli

```
vtesse-cli [--config FILE] [--seconds N] [-v]
           [--socks PORT] [--forward LPORT:IP:RPORT]... [--listen ADDR]
           [--fetch http://IP[:PORT]/PATH]
           [--debug-ip A.B.C.D/P --debug-gw A.B.C.D]
vtesse-cli [--config FILE] [--seconds N] --tap [--ifname NAME] [--hook SCRIPT]
```

| Option | Meaning |
|---|---|
| `--config FILE` | configuration file, default `~/.config/vtesse/probe.conf` |
| `--seconds N` | stop after N seconds (default: run until SIGINT/SIGTERM) |
| `--socks PORT` | SOCKS5 server on `LISTEN:PORT` |
| `--forward LPORT:IP:RPORT` | TCP forward `LISTEN:LPORT → IP:RPORT` through the tunnel (repeatable, up to 16) |
| `--listen ADDR` | address of the local listeners, default `127.0.0.1` |
| `--fetch URL` | one HTTP GET through the stack once an address is bound (diagnostics) |
| `--debug-ip`, `--debug-gw` | manual IPv4 instead of DHCP (debugging, `type tun`) |
| `--tap` | [TAP backend](#tap-backend): kernel interface instead of lwIP (Linux, root) |
| `--ifname NAME` | interface name for `--tap`, default `vtesse%d` |
| `--hook SCRIPT` | script run on interface events for `--tap` |

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
- Addressing is done by the system. `--hook` runs `SCRIPT <event>` with
  `VTESSE_EVENT`, `VTESSE_IFNAME`, `VTESSE_LINK` (`ether`/`tun`) and
  `VTESSE_SESSION` in the environment; events: `up` (interface created), `reconnect`,
  `down`, `stop`.

[`scripts/vtesse-hook.sh`](../scripts/vtesse-hook.sh) starts BusyBox `udhcpc` with
[`scripts/vtesse-udhcpc.script`](../scripts/vtesse-udhcpc.script), which

- sets the address and installs the option 121 (or 249) routes via the interface;
- **never touches `/etc/resolv.conf`** — stock DHCP scripts replace the device's
  resolver with the tunnel DNS, which may be unreachable and breaks resolving the
  VTun server itself (found on the bench);
- installs a default route through the tunnel only with `VTESSE_DEFAULT_ROUTE=1` — it
  would also capture the connection to the VTun server unless a host route to the
  server exists.

`dhclient` is a fallback in the hook; its stock script has the two problems above.

```sh
vtesse-cli --config /etc/vtesse.conf --tap --ifname vtesse0 --hook /usr/share/vtesse/vtesse-hook.sh
```

## Configuration file

`key=value` lines, `#` starts a comment. Credentials are read only from this file,
never from the command line; keep it outside the repository with mode 600.

| Key | Meaning |
|---|---|
| `host` | VTun server name or IPv4 address |
| `port` | VTun server TCP port |
| `session` | session name = [device ID](#device-id) |
| `password` | session password |
| `auth` | challenge response, `xor` (default; servers built without OpenSSL) |
| `socks_user`, `socks_password` | optional SOCKS5 username/password (RFC 1929) |

## Device ID

The session name is the device ID: a MAC address written as **12 characters, upper
case, `0-9A-F`, no separators** (e.g. `02AB34CD56EF`). The same value is used as the
client's Ethernet MAC in `type ether` sessions.

- Validation: exactly 12 characters from `0-9A-F`; the least significant bit of the
  first byte must be 0 (a multicast address cannot be a source MAC). The CLI accepts
  other session names too and then derives a locally administered MAC (with a
  warning).
- Embedded Linux: the device's own MAC (or configured).
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

Manual IPv4 settings (`--debug-ip`, `--debug-gw`) exist only for debugging and for
`type tun` (no DHCP over L3).

## Local proxy

Host sockets on `127.0.0.1` (configurable) relayed to TCP connections through the
tunnel. Outbound TCP and IPv4 only.

- **SOCKS5** (RFC 1928) `CONNECT`; no authentication by default, optional
  username/password (RFC 1929). Reply codes:

  | Code | When |
  |---|---|
  | `3` network unreachable | no route via the tunnel, or no DHCP address yet |
  | `4` host unreachable | connect timeout (15 s) |
  | `5` connection refused | the destination port is closed |
  | `7` command not supported | anything but `CONNECT` |
  | `8` address type not supported | host names (no DNS in the stack) and IPv6 |

  A domain field holding an IPv4 literal is accepted (`socks5h` clients).
- **Port forwards** `LPORT:IP:RPORT` for players without proxy support (RTSP 554,
  HTTP 80). RTSP must use RTP over TCP (interleaved); RTP over UDP cannot pass a TCP
  forward. A forward to an unreachable destination closes the local connection.
- At most 24 connections at a time (bounded by lwIP TCP PCBs); further connections
  are accepted and closed.
- Any local process can use an unauthenticated proxy on `127.0.0.1` — set
  `socks_user` / `socks_password` where that matters.

## Errors

Link events printed by the CLI (`down` = retried automatically with backoff 1–30 s,
`failed` = final):

| Message | Kind | Meaning |
|---|---|---|
| wrong session name or password | failed | server replied `ERR` (unknown session, wrong password, wrong auth mode, or session locked with `multi no`) |
| compression is enabled on the server | failed | session flags contain `C`/`L` |
| encryption is enabled on the server | failed | session flags contain `E` |
| unsupported transport (UDP) | failed | session flags contain `U` |
| unsupported session type | failed | `type tty` / `type pipe` |
| protocol error (not a VTun server?) | failed | unexpected greeting or flags |
| closed by server (possibly taken over by another connection) | down | `CONN_CLOSE` or EOF, e.g. another client logged in with the same session and `multi killold` |
| no response from server (keepalive) | down | 5 keepalive intervals without any frame |
| timeout | down | a handshake step timed out |
| cannot resolve / cannot connect to server | down | DNS or TCP connect failure |

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
