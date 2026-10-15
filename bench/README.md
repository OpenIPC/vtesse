# VTesse test bench

A Docker image with a **VTun 3.0.2 server built like the target deployment**
(`--disable-ssl --disable-lzo --disable-zlib --prefix=''`, so the challenge
response is XOR) and a bridged L2 segment that looks like the real one:

```
 client (vtesse / stock vtund / probe)
        │ TCP
        ▼
 ┌──────────────────────────── bench container ────────────────────────────┐
 │ vtund -s ── tapN ──┐                                                    │
 │                    br-vtesse 10.77.0.1/24 ── dnsmasq (DHCP, option 121) │
 │                    │ (routing)                                          │
 │                    rt0 10.78.0.1/24 ── veth ── netns "device"           │
 │                                               10.78.0.10: HTTP :80,     │
 │                                                           RTSP :554     │
 └─────────────────────────────────────────────────────────────────────────┘
```

All names, addresses and passwords are bench-only test values.

The server is built with our patches by [`server/build-vtund.sh`](../server/README.md)
(the same script as the release binaries): a refused login is
logged with the session name and the reason, e.g.
`Denied connection from 172.28.0.1:33016: session 02BE0C000001: wrong password`
(stock 3.0.2 logs only `Denied connection from IP:PORT`).

- DHCP: leases `10.77.0.100–199`, infinite; option 3 `10.77.0.1`; option 121
  `10.78.0.0/24` and `10.79.0.0/24` (unreachable) via `10.77.0.1`. dnsmasq pings an
  address before offering it, so the OFFER comes after ~3 s, like on the target server.
- The RTSP "device" is a stub that answers `OPTIONS`/`DESCRIBE` (no media); the
  device also has `/big.bin` (20 MiB) on HTTP and a TCP echo server on port 7.
- vtund syslog output is forwarded to `docker logs`.

## Sessions (`vtund.conf`)

| Session | Purpose | Flags |
|---|---|---|
| `02BE0C000001` | main: `type ether`, bridge + DHCP, `keepalive 10:5`, `multi killold` | `<TeK>` |
| `02BE0C000002` | second ether client | `<TeK>` |
| `02BE0C000003` | inherits `speed 512:512` from `default` | `<TeS512K>` |
| `02BE0C000004` | `compress zlib:9` (client must refuse) | `<TeC9K>` |
| `02BE0C000005` | `multi no` (second login → `ERR`) | `<TeK>` |
| `02BE0C000006` | `type tun`, manual addressing (`10.76.0.1` ↔ `10.76.0.2`) | `<TuK>` |
| `02BE0C000007`, `02BE0C000008` | more ether clients for real devices on the LAN (`BENCH_BIND`) | `<TeK>` |

Passwords are in `vtund.conf`.

## Usage

Requires Docker with `--privileged` (tap devices, bridge, network namespace).

```sh
bench/bench.sh build        # build the image (downloads and verifies vtun-3.0.2.tar.gz)
bench/bench.sh up           # server on 127.0.0.1:15000 (BENCH_PORT)
bench/bench.sh selftest     # protocol self-test (see below)
bench/bench.sh stock-test   # reference: stock vtund client + dhclient reach the device
bench/bench.sh probe --dhcp # tools/vtun_probe.py with the main session
bench/bench.sh logs
bench/bench.sh down
```

`bench.sh cli-test` (after `make`) is the stage 1 acceptance test for
`build/vtesse-cli`: DHCP address and option 121 routes, option 3 ignored, HTTP GET to
the device through lwIP, ping of the client from the device, reconnect after a
`killold` take-over keeping the same address.

`bench.sh proxy-test` is the stage 2 acceptance test (SOCKS5 and port forwards):
reply codes for host names/IPv6/no route/refused, `curl --socks5`, 20 MiB download
and 8 MiB TCP echo through forwards with integrity checks, RTSP `OPTIONS`, 8 parallel
downloads, SOCKS5 username/password. The device also serves `/big.bin` (20 MiB,
sha256 in `/big.sha256`) and a TCP echo on port 7.

`bench.sh tap-test` is the TAP backend acceptance test (the device ID comes from the MAC
of a dummy interface, `id_from`): it builds `vtesse-cli` with
`LWIP=0` in a Debian bookworm container (`ci/Dockerfile.native`) and runs it with
`--tap` in a second privileged container; addressing via `udhcpc` and
`scripts/vtesse-udhcpc.script`; inbound ping and HTTP from the device to the client.

`bench.sh tun-test` is the `type tun` acceptance test (#8): session `02BE0C000006` with
manual IPv4 (`ip=10.76.0.2/32`, `gw=10.76.0.1`, `route=10.78.0.0/24`); lwIP: HTTP to the
device and inbound ping; `--tap`: a TUN interface addressed by the hook, ping/HTTP to the
device, inbound ping, no default route.

`bench.sh udp-test` is the UDP relay acceptance test (#28): `vtesse-cli` with
`udp_relay=1559`; the device sends 500 numbered datagrams and a fragmented 4000-byte
one to the client's tunnel address; all must arrive at `127.0.0.1:1559` in order.

`bench.sh fault-test` covers failures (#9): link loss (`docker pause` past the keepalive
timeout), server restart, a DHCP server without option 121 and 2-minute leases
(renewal); it restores dnsmasq at the end. The entrypoint is restart-safe.

`selftest.py` checks: 50-byte greeting; `<TeK>`; `ECHO_REQ` as the first frame;
DHCP OFFER through the tunnel, delayed ≥ 2 s, with option 121; `killold`
take-over (new login OK, old connection closed); shaper/compression/tun flags;
`ERR` for a wrong password and for an unknown session (after `OK CHAL`);
`multi no` refusal.

Reference handshake vectors from the stock client: [`vectors/`](vectors/).

## Findings

- **tap-up race.** The server runs the session `up` commands (`ip link set up`,
  `brctl addif`) asynchronously and starts forwarding immediately. A data frame
  that arrives before the tap is up makes the tap write fail with `EIO` and the
  server drops the session. Reproduced with frames sent < 50 ms after `OK FLAGS`;
  a client must wait (~0.5 s) before its first data frame. Keepalive frames are
  not affected.
- **Bridge MAC changes.** `br-vtesse` has no fixed MAC, so it takes a port's MAC and
  changes when taps join/leave. A client that cached the gateway MAC before a
  reconnect sends to a dead MAC until the gateway re-ARPs; VTesse flushes its ARP
  cache on every tunnel `UP`.
- The stock client applies option 121 and therefore ignores option 3 (RFC 3442):
  no default route via the tunnel.
