# Architecture

- [Layers](#layers)
- [Principles](#principles)
- [Repository layout](#repository-layout)
- [Tunnel interface](#tunnel-interface)
- [VTun client](#vtun-client)
- [Userspace stack](#userspace-stack)
- [Local proxy](#local-proxy)
- [Backends](#backends)
- [Android](#android)

## Layers

```
 app / video player
        │  TCP to 127.0.0.1:<port> (SOCKS5 or port forward), UDP from the relay
        ▼
 ┌──────────────────────────┐
 │ local proxy, UDP relay   │  netcore/proxy.c   SOCKS5 (CONNECT) / port forwarding
 │                          │  netcore/udprelay.c   UDP to the tunnel address -> 127.0.0.1
 ├──────────────────────────┤
 │ userspace TCP/IP (lwIP)  │  netcore/stack.c   IPv4, TCP, UDP, ICMP, ARP, DHCP client
 ├──────────────────────────┤
 │ tunnel_ops_t             │  include/netcore/tunnel.h   protocol-agnostic
 ├──────────────────────────┤
 │ vtun_client              │  vtun/   handshake, flags, framing, keepalive, reconnect
 ├──────────────────────────┤
 │ transport                │  plain TCP socket
 └──────────────────────────┘
```

On Android a regular app cannot open `/dev/net/tun`; the only system tunnel API is
`VpnService`, which always shows a VPN icon. A VTun server does not care who produces
the packets, so VTesse builds and consumes them in userspace. Only traffic of the
application itself (or of programs configured to use its local proxy) goes through
the tunnel; there is no system network interface.

## Principles

- **Library first.** `libvtesse` (VTun client) + `libnetcore` (tunnel interface,
  stack glue, proxy, loop helper). No hidden threads and no own main loop: the host
  drives everything through file descriptors, readiness callbacks and timers (see
  [embedding](embedding.md)). The VTun client keeps all state in its context; the
  stack is one per process because lwIP keeps global state. CLI and JNI are thin
  wrappers.
- **C99, POSIX only** (sockets, `poll`, `clock_gettime`); the only third-party code is
  lwIP (`NO_SYS=1`, raw API, IPv4 only).
- **Explicit byte order** everywhere (frame headers, DHCP options); tests also run on
  big-endian MIPS.
- **Small footprint:** `-Os`; a small lwIP profile (`VTESSE_LWIP_SMALL`) for cameras
  and routers; build-time module selection is planned
  ([#12](https://github.com/compot-dev/vtesse/issues/12)).
- **Protocol-agnostic upper layers.** Everything above the transport talks to
  `tunnel_ops_t` only, so the stack, proxy and Android layers can later serve another
  tunnel protocol (e.g. [camex](https://github.com/openipc/camex)).

## Repository layout

| Path | Contents |
|---|---|
| `include/netcore/`, `netcore/` | `tunnel_ops_t`, poll loop helper, route table, lwIP stack glue, proxy, TAP backend (protocol-agnostic) |
| `include/vtesse/`, `vtun/` | VTun wire format helpers and the VTun client |
| `cli/` | `vtesse-cli` |
| `port/lwip/` | lwIP configuration (`lwipopts.h`), `arch/cc.h`, hook prototypes |
| `third_party/lwip/` | vendored lwIP 2.2.1, unmodified subset |
| `tests/` | unit tests and client scenarios |
| `bench/` | Docker test bench |
| `android/` | Android app: Gradle project, JNI bridge, foreground service ([android/README.md](../android/README.md)) |
| `server/` | vtund build script and our VTun server patches (bench and release builds) |
| `ci/` | cross builds (`build-musl.sh`), build containers, big-endian MIPS tests under qemu |
| `.github/workflows/` | CI: tests, cross builds, test bench, releases |
| `tools/` | probe script |
| `scripts/` | hook and udhcpc scripts for the TAP backend, service scripts (`S99vtesse`, `vtesse.init`) |
| `docs/` | documentation |

## Tunnel interface

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

See [`include/netcore/tunnel.h`](../include/netcore/tunnel.h).

- `UP` is reported once data may flow (the VTun client holds it back ~0.5 s after
  `OK FLAGS`, see the tap-up race in [protocol](protocol.md#server-behaviour-that-affects-clients)).
- `DOWN` means the tunnel retries by itself (exponential backoff 1–30 s, reset after a
  stable session of 30 s).
- `FAILED` is final: wrong credentials, compression/encryption/UDP/unsupported type
  on the server, not a VTun server.
- `send` never splits a packet; when the output buffer is full the packet is dropped
  (like a lost frame).

## VTun client

[`include/vtesse/vtun_client.h`](../include/vtesse/vtun_client.h),
[`include/vtesse/vtun_proto.h`](../include/vtesse/vtun_proto.h).

- Non-blocking state machine: connect → greeting → `HOST` → `CHAL` → flags →
  established; per-step timeouts (15 s, the `CHAL` step at least 10 s because the
  server may wait ~5 s for `multi killold`).
- 64 KiB output buffer; partial TCP writes never split a frame.
- Keepalive: answers `ECHO_REQ`, sends its own every 10 s, link dead after 5 silent
  intervals.
- Oversized incoming frames (> 2148 bytes) are skipped without losing framing.
- Name resolution uses `getaddrinfo()` (IPv4) and may block briefly at connect time.
- `vtun_proto.c` holds pure wire-format functions (messages, challenge codec, XOR,
  flags parser and policy check, frame headers).

## Userspace stack

[`include/netcore/stack.h`](../include/netcore/stack.h) runs lwIP 2.2.1 over a tunnel
(one stack per process — lwIP keeps global state):

- L2 links: Ethernet netif with ARP; frames not addressed to our MAC or broadcast and
  EtherTypes other than IPv4/ARP are dropped before lwIP (the bridge is shared).
  L3 links: raw IPv4.
- DHCP client (lwIP) requesting options 121 and 249 (`DHCP_ADD_EXTRA_REQUEST_OPTIONS`);
  the routes are parsed in `LWIP_HOOK_DHCP_PARSE_OPTION` and applied when the lease is
  bound. RFC 3442: with classless routes option 3 is ignored.
- No default netif: `LWIP_HOOK_IP4_ROUTE_SRC` admits only the connected subnet and
  routed destinations; `LWIP_HOOK_ETHARP_GET_GW` picks the per-route gateway.
- On every tunnel `UP` the ARP cache is flushed (the bridge MAC may have changed).
- After a reconnect the link comes up again and lwIP requests the previous address
  (DHCP INIT-REBOOT); the lease is never released.
- `port/lwip/lwipopts.h`: default and `VTESSE_LWIP_SMALL` profiles, `MEM_ALIGNMENT`
  follows the pointer size, system byte-order functions.

## Local proxy

[`include/netcore/proxy.h`](../include/netcore/proxy.h): host sockets relayed to lwIP
TCP connections.

- **Flow control end to end:** a local socket is read only while lwIP has send buffer
  space; data received from lwIP is acknowledged (window reopened) only after it was
  written to the local socket. No unbounded buffers; half-close is propagated both
  ways.
- Connections are never freed inside lwIP callbacks; they are marked and cleaned up
  from the loop.
- The proxy is a multi-fd loop source (`poll_fill` / `poll_dispatch`).
- TCP-over-TCP: inner connections share one VTun TCP stream; on the bench a single
  download reaches ~110–140 MiB/s and 8 parallel downloads ~55 MiB/s in total.

User-facing behaviour (reply codes, limits): [usage](usage.md#local-proxy).

## UDP relay

[`include/netcore/udprelay.h`](../include/netcore/udprelay.h) (#28): an lwIP UDP PCB
bound to a port of the client's tunnel address; its receive callback (running in the
loop thread, inside lwIP input) copies each datagram to a connected, non-blocking host
UDP socket (`127.0.0.1:LOCAL_PORT`). No loop source of its own; a full local buffer
drops the datagram instead of stalling the tunnel. One direction only (camera →
device), for RTP pushed by cameras. User-facing: [usage](usage.md#udp-relay-rtp-from-cameras).

## Backends

Two consumers of `tunnel_ops_t`:

| | lwIP (default) | TAP ([`tapdev.h`](../include/netcore/tapdev.h)) |
|---|---|---|
| Interface in the system | none | `vtesse0` (TAP for `type ether`, TUN for `type tun`) |
| Who can use the tunnel | the application, programs using its proxy | every program |
| Inbound connections | no | yes |
| Addressing | built-in DHCP client (lwIP) | system DHCP client via the hook script |
| Privileges | none | root or `CAP_NET_ADMIN`, `tun` driver |
| Platforms | Linux, Android | Linux |
| Code size (x86_64, `-Os`) | ~107 KiB code + lwIP pools | ~23 KiB (`make LWIP=0`) |

The TAP backend creates the interface on the first tunnel `UP`, keeps it across
reconnects and switches the carrier off while the tunnel is down
(`TUNSETCARRIER`; on kernels without it the interface just stays up and the hook
flushes neighbours). Frames are read in batches and dropped while the tunnel is down
or its output buffer is full; frames from the tunnel are written non-blocking.

## Android

Planned ([#13](https://github.com/compot-dev/vtesse/issues/13)): foreground service
with a regular notification (not a VPN), no `VpnService`, no `BIND_VPN_SERVICE`.
Android 14+ requires a `foregroundServiceType`; Doze and network changes are handled
by reconnecting (`NetworkCallback`).
