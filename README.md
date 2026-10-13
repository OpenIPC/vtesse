# VTesse — resource access microsystem

VTesse is a client for existing **VTun** servers (`vtund -s`) that works **without a
TUN/TAP device and without a system VPN**. The tunnel is terminated inside the
application by a userspace TCP/IP stack (lwIP); applications reach remote devices
through a local SOCKS5 proxy or port forwards on `127.0.0.1`.

- No root, no `/dev/net/tun`, no Android `VpnService` (no VPN icon).
- Targets: embedded Linux (IP cameras, routers; ARM and MIPS, including big-endian,
  uClibc/musl), Android, and later an embeddable communication module.
- Client only, outbound connections only, IPv4 only.

> **Status: prototype.** Traffic is not encrypted (see [Security](#security)).

## Table of contents

- [How it works](#how-it-works)
- [VTun protocol](#vtun-protocol)
- [Addressing (DHCP)](#addressing-dhcp)
- [Device ID](#device-id)
- [Architecture](#architecture)
- [Security](#security)
- [Tools](#tools)
- [Roadmap](#roadmap)
- [License](#license)

## How it works

On Android a regular app cannot open `/dev/net/tun` and cannot load kernel
modules; the only system tunnel API is `VpnService`, which always shows a VPN icon.
A VTun server does not care who produces the packets: in `type tun` sessions frames
carry raw IPv4 packets, in `type ether` sessions raw Ethernet frames. VTesse builds
and consumes those frames in userspace:

```
 app / video player
        │  TCP to 127.0.0.1:<port>   (SOCKS5 or port forward)
        ▼
 ┌──────────────────────────┐
 │ local proxy              │  SOCKS5 (CONNECT) / port forwarding
 ├──────────────────────────┤
 │ userspace TCP/IP (lwIP)  │  IPv4, TCP, UDP, ICMP, ARP, DHCP client
 ├──────────────────────────┤
 │ tunnel_ops_t             │  protocol-agnostic tunnel interface
 ├──────────────────────────┤
 │ vtun_client              │  handshake, flags, framing, keepalive
 ├──────────────────────────┤
 │ transport                │  plain TCP socket
 └──────────────────────────┘
```

Limitation: only traffic of the application itself (or of programs configured to
use its local proxy) goes through the tunnel, not the whole device.

## VTun protocol

Verified against the VTun **3.0.2** sources (`auth.c`, `lib.c`, `lock.c`,
`linkfd.c`, `netlib.c`, `generic/tcp_proto.c`, `linux/tun_dev.c`, `cfg_file.y`)
and against a live 3.0.2 server. Other versions (2.x, 3.0.3+, distribution forks)
may differ — e.g. `nat_hack` does not exist in 3.0.2.

### Handshake

Every control message is **exactly 50 bytes** (`VTUN_MESG_SIZE`): text padded with
NUL bytes (`print_p` formats into 49 bytes and always writes 50; `readn_t` reads
exactly 50). A short message without padding stalls the handshake.

```
S → C: "VTUN server ver <ver> <build date>\n"      50 bytes, NUL padded
C → S: "HOST: <session>\n"                         50 bytes
S → C: "OK CHAL: <32 chars>\n"                     50 bytes
C → S: "CHAL: <32 chars>\n"                        50 bytes
S → C: "OK FLAGS: <...>\n"   or   "ERR\n"          50 bytes
```

- The server answers **any** `HOST:` with `OK CHAL`, even for an unknown session.
  After `CHAL:` it replies `ERR` for: malformed response, unknown session, wrong
  response (password / auth mode) or a refused session lock. `ERR` therefore cannot
  distinguish "unknown session" from "wrong password".
- The server tokenizes with `strtok(buf, " :")`: the session name must not contain
  spaces or `:`; with the `"HOST: "` prefix and `"\n"` it must fit 49 bytes
  (≤ 42 characters), otherwise it is silently truncated.
- Each handshake step is bounded by the server `timeout` option (default 30 s).

**Challenge encoding** (`cl2cs` / `cs2cl`): 16 bytes (`VTUN_CHAL_SIZE`), each byte
as two characters from `"abcdefghijklmnop"` (high nibble, then low nibble), wrapped
in `<` `>`. Exactly 32 characters.

**Challenge response** (`encrypt_chal`) depends on how the server was built:

| Server build | Response |
|---|---|
| without OpenSSL (`--disable-ssl`) | `resp[i] = chal[i] ^ pwd[i % strlen(pwd)]`, i = 0..15 — only the first 16 password characters matter |
| with OpenSSL | Blowfish-ECB over two 8-byte blocks, key = `MD5(password)` (16 bytes) |

The client cannot detect the variant in advance; the auth mode is a setting.

**Session lock** (`lock_host`, option `multi`): `yes` (default) allows parallel
sessions, `no` rejects a second one with `ERR`, `killold` sends SIGTERM to the old
session process and waits up to ~5 s (then SIGKILL) before replying `OK FLAGS`.
With `killold`, two clients using the same session name keep kicking each other
off — every device must have its own session name.

### Session flags

`OK FLAGS: <...>` (`bf2cf` / `cf2bf`), server order: transport, type, `S<n>`,
`C<n>`, `L<n>`, `K`, `E<n>`, `>`. Example: `<TeK>`.

| Flag | Meaning | VTesse |
|---|---|---|
| `T` / `U` | TCP / UDP transport | TCP; `U` → error "UDP not supported" (deferred) |
| `u` / `e` | `type tun` / `type ether` | supported |
| `t` / `p` | `type tty` / `type pipe` | error |
| `K` | keepalive | must answer `ECHO_REQ` |
| `C<n>` / `L<n>` | zlib / LZO compression | error "compression enabled on server" |
| `E` / `E<n>` | encryption (`E0`/`E` legacy, else cipher id) | error "encryption enabled on server" |
| `S<n>` | shaper | ignored |
| `F` | reserved | ignored |

An unknown flag character is an error (as in the reference `cf2bf`).

### Framing

Right after `OK FLAGS` the same TCP connection carries frames:

```
+----------------+----------------------+
| hdr (2 bytes)  | data (0..2048 bytes) |
+----------------+----------------------+
hdr, big-endian: bits 0..11 = length (VTUN_FSIZE_MASK 0x0fff), bits 12..15 = flags
```

| Flag | Name | Meaning |
|---|---|---|
| `0x1000` | `VTUN_CONN_CLOSE` | peer closes the session (also sent when kicked by `killold`) |
| `0x2000` | `VTUN_ECHO_REQ` | keepalive request |
| `0x4000` | `VTUN_ECHO_REP` | keepalive reply |
| `0x8000` | `VTUN_BAD_FRAME` | internal return code of `tcp_read`, never on the wire |

- A flag frame is a bare header (length 0).
- Max payload `VTUN_FRAME_SIZE` = 2048; the receiver accepts up to 2048 + 100
  (`VTUN_FRAME_OVERHEAD`) and drops larger frames.
- `type ether`: the server opens a tap with `IFF_NO_PI` → payload is a raw Ethernet
  frame (14-byte header + payload), no packet-info prefix. MTU 1500 → 1514-byte frames.
- `type tun`: raw IPv4 packet (`IFF_TUN | IFF_NO_PI`).

### Keepalive (`lfd_linker`)

- The server sends `ECHO_REQ` **immediately** when the session starts (first frame
  after `OK FLAGS`).
- It then waits `ka_interval` seconds (`keepalive <interval>:<count>`, default 30:4);
  on a fully idle interval it sends another `ECHO_REQ` and increments a counter;
  when the counter exceeds `count` the session is dropped.
- **Any** received frame resets the counter. The server answers `ECHO_REQ` with
  `ECHO_REP` and ignores `ECHO_REP`.
- VTesse answers every `ECHO_REQ` and sends its own `ECHO_REQ` to detect dead links.

**tap-up race.** The server runs the session `up` commands (bringing the tap up,
adding it to a bridge) asynchronously and starts forwarding at once. A data frame
that arrives before the tap is up fails with `EIO` on the server and the session is
dropped. VTesse waits ~0.5 s after `OK FLAGS` before its first data frame
(reproduced on the [bench](bench/)).

### UDP transport (deferred)

With `U` in the flags (`udp_session` in `netlib.c`) both sides create a UDP socket,
exchange 2-byte ports (network order) over TCP, close TCP and continue over UDP
(one datagram = `[hdr][data]`). In 3.0.2 the server binds UDP to the same local
address and port as the TCP socket and `connect()`s it to the client's TCP peer
address plus the reported port; there is no `nat_hack`. Behind a NAT that does not
preserve the source port (typical carrier-grade NAT) the client's datagrams are
silently dropped. VTesse currently supports TCP only.

### Not provided by VTun

VTun does not hand out addresses or routes — the stock client configures them in
`up { ... }` scripts. VTesse obtains them via DHCP (below).

## Addressing (DHCP)

For `type ether` sessions bridged to a segment with a DHCP server, VTesse runs the
lwIP DHCP client through the tunnel:

- options used: 1 (netmask), 3 (router), 51/58/59 (lease), **121** (classless static
  routes, RFC 3442), 249 (Microsoft variant of 121); 6 (DNS) and 28 are informational;
- per **RFC 3442**, when option 121 is present option 3 is ignored → there is no
  default route through the tunnel; only the connected subnet and the 121 routes are
  reachable, other destinations are rejected by the proxy ("no route via tunnel");
- `chaddr` and client-id are derived from the [device ID](#device-id), so a
  reconnect gets the same lease;
- the server may probe the offered address with ARP before answering, so the first
  DHCP timeout must be several seconds;
- leases are not released on tunnel loss; on reconnect the previous address is
  requested again;
- until an address is bound the proxy rejects connections ("tunnel not ready").

Manual IPv4 settings exist only for debugging and for `type tun` (no DHCP over L3).

## Device ID

The session name is the device ID: a MAC address written as **12 characters, upper
case, `0-9A-F`, no separators** (e.g. `02AB34CD56EF`). The same value is used as the
client's Ethernet MAC in `type ether` sessions.

- Validation: exactly 12 characters from `0-9A-F`; the least significant bit of the
  first byte must be 0 (a multicast address cannot be a source MAC).
- Embedded Linux: the device's own MAC (or configured).
- Android: the hardware MAC is not available to apps — a random locally administered
  unicast ID is generated once and stored (second character `2`, `6`, `A` or `E`),
  or the ID is entered manually. A session with that name must exist on the server.

## Architecture

- **Library first.** `libvtesse` (VTun client) + `libnetcore` (lwIP glue, proxy, event
  loop helpers). No global state, no hidden threads, no own main loop: the host
  application drives everything through file descriptors, readiness callbacks and
  timers. CLI and JNI are thin wrappers.
- **C99, POSIX only** (sockets, `poll`, `clock_gettime`); the only third-party code is
  lwIP (`NO_SYS=1`, raw API, IPv4 only).
- **Explicit byte order** everywhere (frame headers, DHCP options); tests also run on
  big-endian MIPS (qemu).
- **Small footprint:** `-Os`, optional modules (SOCKS5, port forwarding, `type tun`,
  Blowfish auth, UDP) selectable at build time.
- **Protocol-agnostic upper layers.** Everything above the transport talks to a
  tunnel interface only, so the stack/proxy/Android layers can later serve another
  tunnel protocol (e.g. [camex](https://github.com/openipc/camex)):

```c
typedef struct tunnel_ops {
    int  (*get_fd)(void *ctx);              /* -1 when none; may change, re-query every loop */
    int  (*wants_write)(void *ctx);         /* POLLOUT needed (connect, pending output) */
    void (*on_readable)(void *ctx);
    void (*on_writable)(void *ctx);
    int  (*next_timeout_ms)(void *ctx);     /* 0 = due, -1 = none */
    void (*on_timer)(void *ctx);
    void (*set_rx_cb)(void *ctx, tunnel_rx_cb cb, void *user);
    void (*set_event_cb)(void *ctx, tunnel_event_cb cb, void *user);  /* CONNECTING/UP/DOWN/FAILED */
    int  (*send)(void *ctx, const uint8_t *pkt, size_t len);  /* whole packet or -1 (dropped) */
    int  (*link_type)(void *ctx);           /* L2 (Ethernet) or L3 (IPv4) */
    int  (*mtu)(void *ctx);
    void (*close)(void *ctx);
} tunnel_ops_t;
```

See [`include/netcore/tunnel.h`](include/netcore/tunnel.h). `UP` is reported once data
may flow (the VTun client holds it back ~0.5 s after `OK FLAGS`, see the tap-up race).
`DOWN` means the tunnel retries by itself (exponential backoff 1–30 s, reset after a
stable session); `FAILED` is final (wrong credentials, compression/encryption/UDP/
unsupported type on the server, not a VTun server).

### Userspace stack

[`include/netcore/stack.h`](include/netcore/stack.h) runs lwIP 2.2.1 over a tunnel
(one stack per process — lwIP keeps global state):

- L2 links: Ethernet netif with ARP; frames not addressed to our MAC or broadcast and
  EtherTypes other than IPv4/ARP are dropped before lwIP (the bridge is shared).
  L3 links: raw IPv4.
- DHCP client (lwIP) requesting options 121 and 249; parsed routes are applied when
  the lease is bound. RFC 3442: with classless routes option 3 is ignored.
- No default netif: `LWIP_HOOK_IP4_ROUTE_SRC` admits only the connected subnet and
  routed destinations; `LWIP_HOOK_ETHARP_GET_GW` picks the per-route gateway.
- On every tunnel `UP` the ARP cache is flushed: a Linux bridge without a fixed MAC
  takes a port's MAC, so the gateway MAC changes when taps come and go (found on the
  bench — replies went to the old MAC until the gateway re-ARPed).
- After a reconnect the link comes up again and lwIP requests the previous address
  (DHCP INIT-REBOOT); the lease is never released.
- `port/lwip/`: `lwipopts.h` (default and `VTESSE_LWIP_SMALL` profiles), `arch/cc.h`,
  hook prototypes. `third_party/lwip/`: unmodified upstream subset.

Partial TCP writes are buffered so frames are never split; when the buffer is full
whole packets are dropped.

### Local proxy

- SOCKS5 `CONNECT` (no auth by default, optional RFC 1929 user/password), IPv4
  destinations only; domain names (ATYP 3) and IPv6 (ATYP 4) are rejected — there is
  no DNS in the stack.
- Static port forwards `127.0.0.1:L → remote_ip:R` for players without proxy support.
- RTSP: use RTSP-over-TCP (interleaved); RTP over UDP cannot pass a TCP forward.
- Any local process can use an unauthenticated proxy on `127.0.0.1`.
- TCP-over-TCP: inner TCP connections degrade on lossy links; lwIP TCP timers are
  tuned conservatively.

### Android

Foreground service with a regular notification (not a VPN), no `VpnService`, no
`BIND_VPN_SERVICE`. Android 14+ requires a `foregroundServiceType`; Doze and
network changes are handled by reconnecting (`NetworkCallback`).

## Security

- **Authentication.** XOR mode: the challenge and the response are sent in clear and
  `response = challenge XOR password` — one captured handshake reveals the first 16
  password characters; the 3.0.2 challenge comes from `srand(time(NULL))` and is
  predictable. Blowfish mode: unsalted `MD5(password)` key — offline brute force is
  possible from a captured handshake.
- **Data** is sent in clear (VTesse rejects VTun encryption/compression; VTun's own
  encryption has no integrity protection anyway).
- Use only over trusted networks or inside an external secure channel (TLS /
  stunnel, an operator VPN). A TLS wrapper for the TCP transport is on the roadmap.

## Building

C99 + POSIX, plain `make`; cross compile with `make CC=<toolchain>-gcc`.

```sh
make            # build/libnetcore.a, build/libvtesse.a, build/vtesse-cli
make test       # unit tests and client scenarios against a scripted server
```

Layout:

| Path | Contents |
|---|---|
| `include/netcore/`, `netcore/` | `tunnel_ops_t`, poll loop helper, route table, lwIP stack glue (protocol-agnostic) |
| `include/vtesse/`, `vtun/` | VTun wire format helpers and the VTun client |
| `cli/` | `vtesse-cli` |
| `tests/` | unit tests (`test_proto`, `test_route`), client scenarios (`test_client`) |
| `port/lwip/`, `third_party/lwip/` | lwIP configuration and vendored lwIP 2.2.1 |
| `ci/` | big-endian MIPS build + tests under qemu (`ci/test-mips.sh`, Docker) |
| `bench/` | Docker test bench |
| `tools/` | probe script |

`vtesse-cli --config FILE [--seconds N] [--fetch http://IP[:PORT]/PATH]` reads `host`,
`port`, `session` (= device ID, also the MAC), `password` from a key=value file (never
from the command line), brings the tunnel and the stack up, prints the DHCP address
and routes and optionally performs one HTTP GET through lwIP. `--debug-ip A.B.C.D/P
--debug-gw G` replaces DHCP (debugging, `type tun`).

```
[   0.504] up: VTUN server ver 3.X 10/13/2026, flags <TeK>
[   3.512] address 10.77.0.169 mask 255.255.255.0, option 3 10.77.0.1 (ignored: classless routes present)
[   3.512]   route 10.78.0.0/24 via 10.77.0.1
[   3.512]   route 10.79.0.0/24 via 10.77.0.1
[   3.522] fetch 10.78.0.10:80/: ok, 205 bytes, "HTTP/1.0 200 OK"
```

Checks: `make test` (also under ASan/UBSan), `ci/test-mips.sh` (big-endian),
`bench/bench.sh selftest` and `bench/bench.sh cli-test` (stage 1 acceptance on the
bench: DHCP, routes, HTTP through lwIP, ping from the device, reconnect after a
`killold` take-over with the same address).

## Tools

[`bench/`](bench/) — Docker test bench: VTun 3.0.2 server, bridged segment with a
DHCP server (option 121) and a remote device; protocol self-test and reference
vectors.

`tools/vtun_probe.py` — Python 3, no dependencies. Performs the handshake (XOR
auth), prints the greeting and flags, answers keepalives, optionally sends one
DHCPDISCOVER through a `type ether` session and decodes the offer (including
option 121), then closes with `VTUN_CONN_CLOSE`.

```sh
tools/vtun_probe.py [--config ~/.config/vtesse/probe.conf] [--dhcp] [--delay S] [--seconds N]
```

The config file (`key=value`: `host`, `port`, `session`, `password`) lives outside
the repository. With `multi killold`, probing with a production session name kicks
the real device off — use a dedicated test session.

## Roadmap

Stages and tasks are tracked as [GitHub issues](https://github.com/compot-dev/vtesse/issues):

| Stage | Issues |
|---|---|
| 0. Test bench (VTun 3.0.2 in Docker, bridge, DHCP with option 121) | [#1](https://github.com/compot-dev/vtesse/issues/1) |
| 1. CLI prototype on Linux: TCP + `type ether` + DHCP over lwIP | [#2](https://github.com/compot-dev/vtesse/issues/2) [#3](https://github.com/compot-dev/vtesse/issues/3) [#4](https://github.com/compot-dev/vtesse/issues/4) [#5](https://github.com/compot-dev/vtesse/issues/5) [#6](https://github.com/compot-dev/vtesse/issues/6) [#7](https://github.com/compot-dev/vtesse/issues/7) [#8](https://github.com/compot-dev/vtesse/issues/8) |
| 2. Local proxy: SOCKS5 and port forwarding | [#10](https://github.com/compot-dev/vtesse/issues/10) [#11](https://github.com/compot-dev/vtesse/issues/11) |
| 3. Embedded Linux: ARM/MIPS cross builds, footprint | [#12](https://github.com/compot-dev/vtesse/issues/12) |
| 4. Android app (no VpnService) | [#13](https://github.com/compot-dev/vtesse/issues/13) |
| 5. Optional: UDP transport, TLS wrapper, Blowfish auth | [#14](https://github.com/compot-dev/vtesse/issues/14) [#15](https://github.com/compot-dev/vtesse/issues/15) [#16](https://github.com/compot-dev/vtesse/issues/16) |
| 6. Reuse of the upper layers for camex | [#17](https://github.com/compot-dev/vtesse/issues/17) |
| Cross-cutting: tests and CI, license | [#9](https://github.com/compot-dev/vtesse/issues/9) [#18](https://github.com/compot-dev/vtesse/issues/18) |

## License

[The Prosperity Public License 3.0.0](LICENSE.md) — free for noncommercial use, a
thirty-day trial for commercial use. Third-party code keeps its own license
(lwIP: BSD-3-Clause).
