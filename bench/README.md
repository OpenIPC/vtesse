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

- DHCP: leases `10.77.0.100–199`, infinite; option 3 `10.77.0.1`; option 121
  `10.78.0.0/24` and `10.79.0.0/24` (unreachable) via `10.77.0.1`. dnsmasq pings an
  address before offering it, so the OFFER comes after ~3 s, like on the target server.
- The RTSP "device" is a stub that answers `OPTIONS`/`DESCRIBE` (no media).
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
- The stock client applies option 121 and therefore ignores option 3 (RFC 3442):
  no default route via the tunnel.
