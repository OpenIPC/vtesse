# VTun protocol

Specification of the VTun client/server protocol as used by VTesse. Verified
against the VTun **3.0.2** sources (`auth.c`, `lib.c`, `lock.c`, `linkfd.c`,
`netlib.c`, `generic/tcp_proto.c`, `linux/tun_dev.c`, `cfg_file.y`) and against a
live 3.0.2 server. Other versions (2.x, 3.0.3+, distribution forks) may differ —
e.g. `nat_hack` does not exist in 3.0.2.

- [Handshake](#handshake)
- [Session flags](#session-flags)
- [Framing](#framing)
- [Keepalive](#keepalive)
- [Server behaviour that affects clients](#server-behaviour-that-affects-clients)
- [UDP transport](#udp-transport)
- [Not provided by VTun](#not-provided-by-vtun)

## Handshake

TCP, default port `VTUN_PORT`. Every control message is **exactly 50 bytes**
(`VTUN_MESG_SIZE`): text padded with NUL bytes (`print_p` formats into 49 bytes and
always writes 50; `readn_t` reads exactly 50). A short message without padding stalls
the handshake.

```
S → C: "VTUN server ver <ver> <build date>\n"      50 bytes, NUL padded
C → S: "HOST: <session>\n"                         50 bytes
S → C: "OK CHAL: <32 chars>\n"                     50 bytes
C → S: "CHAL: <32 chars>\n"                        50 bytes
S → C: "OK FLAGS: <...>\n"   or   "ERR\n"          50 bytes
```

The stock client (`auth_client`) checks only prefixes: `"VTUN"`, then `"OK"` plus a
challenge, then `"OK"` plus flags. 3.0.2 builds report version `3.X` followed by the
build date.

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

| Server build                      | Response                                                                                             |
|-----------------------------------|------------------------------------------------------------------------------------------------------|
| without OpenSSL (`--disable-ssl`) | `resp[i] = chal[i] ^ pwd[i % strlen(pwd)]`, i = 0..15 — only the first 16 password characters matter |
| with OpenSSL                      | Blowfish-ECB over two 8-byte blocks, key = `MD5(password)` (16 bytes)                                |

The client cannot detect the variant in advance; the auth mode is a setting.
VTesse implements XOR; Blowfish is planned
([#16](https://github.com/OpenIPC/vtesse/issues/16)).

Reference vectors from the stock client are in
[`bench/vectors/`](../bench/vectors/README.md).

**Session lock** (`lock_host`, option `multi`): `yes` (default) allows parallel
sessions, `no` rejects a second one with `ERR`, `killold` sends SIGTERM to the old
session process and waits up to ~5 s (then SIGKILL) before replying `OK FLAGS`.
With `killold`, two clients using the same session name keep kicking each other
off — every device must have its own session name. The lock lives in
`VTUN_LOCK_DIR` (`<localstatedir>/lock/vtund`); if that directory is missing, every
login with `multi` other than `yes` gets `ERR`.

## Session flags

`OK FLAGS: <...>` (`bf2cf` / `cf2bf`), server order: transport, type, `S<n>`,
`C<n>`, `L<n>`, `K`, `E<n>`, `>`. Example: `<TeK>`.

| Flag            | Meaning                                      | VTesse                                          |
|-----------------|----------------------------------------------|-------------------------------------------------|
| `T` / `U`       | TCP / UDP transport                          | TCP; `U` → error "UDP not supported" (deferred) |
| `u` / `e`       | `type tun` / `type ether`                    | supported                                       |
| `t` / `p`       | `type tty` / `type pipe`                     | error                                           |
| `K`             | keepalive                                    | must answer `ECHO_REQ`                          |
| `C<n>` / `L<n>` | zlib / LZO compression                       | error "compression enabled on server"           |
| `E` / `E<n>`    | encryption (`E0`/`E` legacy, else cipher id) | error "encryption enabled on server"            |
| `S<n>`          | shaper                                       | ignored                                         |
| `F`             | reserved                                     | ignored                                         |

An unknown flag character, a missing number after `C`/`L`/`S` or a missing `>` is an
error (as in the reference `cf2bf`). A session `speed 0:0` removes a shaper inherited
from the `default` section.

## Framing

Right after `OK FLAGS` the same TCP connection carries frames:

```
+----------------+----------------------+
| hdr (2 bytes)  | data (0..2048 bytes) |
+----------------+----------------------+
hdr, big-endian: bits 0..11 = length (VTUN_FSIZE_MASK 0x0fff), bits 12..15 = flags
```

| Flag     | Name              | Meaning                                                      |
|----------|-------------------|--------------------------------------------------------------|
| `0x1000` | `VTUN_CONN_CLOSE` | peer closes the session (also sent when kicked by `killold`) |
| `0x2000` | `VTUN_ECHO_REQ`   | keepalive request                                            |
| `0x4000` | `VTUN_ECHO_REP`   | keepalive reply                                              |
| `0x8000` | `VTUN_BAD_FRAME`  | internal return code of `tcp_read`, never on the wire        |

- A flag frame is a bare header (length 0).
- Max payload `VTUN_FRAME_SIZE` = 2048; the receiver accepts up to 2048 + 100
  (`VTUN_FRAME_OVERHEAD`) and drops larger frames.
- `type ether`: the server opens a tap with `IFF_NO_PI` → payload is a raw Ethernet
  frame (14-byte header + payload), no packet-info prefix. MTU 1500 → 1514-byte frames.
- `type tun`: raw IPv4 packet (`IFF_TUN | IFF_NO_PI`).

## Keepalive

Server side (`lfd_linker` in `linkfd.c`):

- The server sends `ECHO_REQ` **immediately** when the session starts (first frame
  after `OK FLAGS`). The stock client does the same.
- It then waits `ka_interval` seconds (`keepalive <interval>:<count>`, default 30:4);
  on a fully idle interval it sends another `ECHO_REQ` and increments a counter;
  when the counter exceeds `count` the session is dropped.
- **Any** received frame resets the counter. The server answers `ECHO_REQ` with
  `ECHO_REP` and ignores `ECHO_REP`.

VTesse answers every `ECHO_REQ`, sends its own `ECHO_REQ` every 10 s and declares
the link dead after 5 silent intervals.

## Server behaviour that affects clients

**tap-up race.** The server runs the session `up` commands (bringing the tap up,
adding it to a bridge) asynchronously and starts forwarding at once. A data frame
that arrives before the tap is up fails with `EIO` on the server and the session is
dropped. Reproduced on the bench with frames sent < 50 ms after `OK FLAGS`; VTesse
waits ~0.5 s before its first data frame. Keepalive frames are not affected.

**Bridge MAC changes.** With `type ether` sessions added to a Linux bridge that has
no fixed MAC, the bridge takes a port's MAC, so the gateway MAC on the segment
changes when taps join and leave. A client that cached the gateway MAC before a
reconnect sends to a dead MAC until the gateway re-ARPs; VTesse flushes its ARP
cache on every reconnect.

## UDP transport

With `U` in the flags (`udp_session` in `netlib.c`) both sides create a UDP socket,
exchange 2-byte ports (network order) over TCP, close TCP and continue over UDP
(one datagram = `[hdr][data]`). In 3.0.2 the server binds UDP to the same local
address and port as the TCP socket and `connect()`s it to the client's TCP peer
address plus the reported port; there is no `nat_hack`. Behind a NAT that does not
preserve the source port (typical carrier-grade NAT) the client's datagrams are
silently dropped. VTesse currently supports TCP only
([#14](https://github.com/OpenIPC/vtesse/issues/14)).

## Not provided by VTun

VTun does not hand out addresses or routes — the stock client configures them in
`up { ... }` scripts. For `type ether` sessions bridged to a segment with a DHCP
server, VTesse obtains them via DHCP (see [usage](usage.md#addressing-dhcp)).
