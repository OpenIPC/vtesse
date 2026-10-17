# Testing

| Check              | Command                                                                                                          | What it covers                                                                                                                                                                                                                                                                            |
|--------------------|------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Unit tests         | `make test`                                                                                                      | `test_proto` (wire format, stock-client reference vector), `test_route` (option 121/249 parser, lookup, device ID), `test_client` (client scenarios against a scripted server: happy path, `ERR`, compression, bad greeting, handshake and keepalive timeouts, reconnect after take-over) |
| Sanitizers         | `make BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined" test` | memory errors, undefined behaviour, leaks                                                                                                                                                                                                                                                 |
| Big-endian         | `ci/test-mips.sh`                                                                                                | static MIPS BE build in Docker, all unit tests under `qemu-mips`                                                                                                                                                                                                                          |
| Real devices       | `BENCH_BIND=<LAN address> bench/bench.sh up`                                                                     | the bench listens on a LAN address so cameras/routers can connect to it instead of a production server                                                                                                                                                                                    |
| Cross targets      | `QEMU=1 ci/build-musl.sh <target>`                                                                               | static musl build for a camera/router target (VTesse; the patched vtund for x86_64 or with `VTUND=1`), unit tests under qemu-user                                                                                                                                                         |
| Bench self-test    | `bench/bench.sh selftest`                                                                                        | server behaviour VTesse relies on (greeting, flags, keepalive, DHCP through the tunnel, `killold`, `ERR` cases, `multi no`)                                                                                                                                                               |
| Stage 1 acceptance | `bench/bench.sh cli-test`                                                                                        | `vtesse-cli`: DHCP address and option 121 routes, option 3 ignored, HTTP through lwIP, ping of the client from the device, reconnect after a take-over with the same address                                                                                                              |
| Stage 2 acceptance | `bench/bench.sh proxy-test`                                                                                      | SOCKS5 reply codes, `curl --socks5`, 20 MiB download and 8 MiB echo with integrity checks, RTSP through a forward, 8 parallel downloads, SOCKS5 authentication                                                                                                                            |
| type tun           | `bench/bench.sh tun-test`                                                                                        | `type tun` session with manual IPv4 (`ip`/`gw`/`route`): lwIP — HTTP to the device and inbound ping of the client; `--tap` — TUN interface configured by the hook, ping/HTTP to the device, inbound ping, no default route                                                                |
| UDP relay          | `bench/bench.sh udp-test`                                                                                        | the device behind the server sends 500 numbered 1200-byte datagrams and one 4000-byte (fragmented) datagram to the tunnel address; all arrive at `127.0.0.1:1559` in order and intact; relay statistics                                                                                   |
| Failures           | `bench/bench.sh fault-test`                                                                                      | link loss (server frozen past the keepalive timeout): DOWN, reconnect, same address; server restart: reconnect, same address; no option 121: option 3 becomes the default route; 2-minute leases: renewed at T1, address kept after expiry; dnsmasq restored (~4 min)                     |
| TAP backend        | `bench/bench.sh tap-test`                                                                                        | `vtesse-cli --tap` (`LWIP=0` build) in a container: DHCP via udhcpc, option 121 route, no default route, HTTP and a 20 MiB download from any process, **inbound** ping and TCP from the device, reconnect after a take-over keeps interface and address, clean exit                       |
| Reference client   | `bench/bench.sh stock-test`                                                                                      | the stock `vtund` client + `dhclient` reach the device through the bench                                                                                                                                                                                                                  |

## CI

[`.github/workflows/build.yml`](../.github/workflows/build.yml) runs on every push
and pull request:

| Job                                          | What it does                                                                                                                                                                |
|----------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `tests`                                      | `make test`, the same under ASan/UBSan, the `LWIP=0` build                                                                                                                  |
| `build` (arm32, mipsel, arm64, mips, x86_64) | static musl binaries with `ci/build-musl.sh` (`vtesse`, `vtesse-tap`; patched `vtund` in the x86_64 job), unit tests and a `vtund -h` smoke test under qemu-user, artifacts |
| `bench`                                      | the Docker test bench (vtund built with our patches): `selftest`, `cli-test`, `proxy-test`, `tap-test`, `tun-test`, `udp-test`, `fault-test`                                |
| `android`                                    | the APKs in Docker (`android/build.sh`, `ci/Dockerfile.android`); the release APK is signed with the key from the repository secrets                                        |
| `release`                                    | push to `main`: replaces the `latest` release; binaries, `vtund-<VTun version>-x86_64`, hook scripts, service scripts, `SHA256SUMS`                                         |

### Versions and releases

The version lives in one place, `include/vtesse/version.h`:
`#define VTESSE_VERSION "MAJOR.MINOR.PATCH"` (shown by `vtesse-cli --version`). Up to 1.12
it was `MAJOR.MM` (`1.00`, `1.01`, …); 1.12 is followed by 1.12.1 or 1.13.0.

- There is one release, `latest`: every push to `main` replaces it (tag moved to that
  commit). No versioned releases are kept.
- Its title is `VTesse <version>`; the notes are one short line. The change history is
  [CHANGELOG.md](../CHANGELOG.md), the patch list [server/README.md](../server/README.md).
- File names are stable: `vtesse-<arch>`, `vtesse-tap-<arch>`, and
  `vtund-<VTun version>-x86_64` (e.g. `vtund-3.0.2-x86_64`) for the VTun server, PCs only.
- PATCH for fixes and small changes, MINOR for a new feature, MAJOR for an incompatible
  change or a large update, only when the maintainer decides. A pull request that changes
  the code or the build (the sources, `android/` except its unit tests, the `Makefile`,
  `ci/`, `scripts/`) bumps `VTESSE_VERSION` as its last commit before the merge, together
  with its `CHANGELOG.md` section (`## <version> — YYYY-MM-DD`). Docs, `tests/`, `bench/`
  and `.github/` changes do not. CI checks this on every pull request
  (`.github/check-version.sh`): a new number must be canonical (no leading zeros), greater
  than the base one, and come with its own section.

## Test bench

[`bench/`](../bench/README.md): Docker image with a VTun 3.0.2 server built without
OpenSSL and with our patches ([`server/build-vtund.sh`](../server/README.md)), a bridged segment with a DHCP server (option 121, ping check), and a
"remote device" behind a router (HTTP with a 20 MiB file, RTSP stub, TCP echo).

```sh
bench/bench.sh build
bench/bench.sh up
make
bench/bench.sh selftest
bench/bench.sh cli-test
bench/bench.sh proxy-test
bench/bench.sh tap-test
bench/bench.sh down
```

Requires Docker with `--privileged` (tap devices, bridge, network namespace).

## Reference vectors

[`bench/vectors/`](../bench/vectors/README.md): handshake captured from the stock
3.0.2 client; `test_proto` checks the XOR response against it.

## Against a real server

Use a dedicated test session (a production session name gets kicked off with
`multi killold`) and keep its credentials outside the repository:

```sh
tools/vtun_probe.py --config ~/.config/vtesse/probe.conf --dhcp
build/vtesse-cli --config ~/.config/vtesse/probe.conf --seconds 60
```

## Real devices

Devices on the LAN connect to the bench instead of a production server:
`BENCH_BIND=<LAN address> bench/bench.sh up` (sessions `02BE0C000001/2/7/8` are ether
clients; `02BE0C000006` is `type tun`). Faults are injected on the bench side:
`docker pause` (link loss past the keepalive timeout), `docker restart` (server
restart), a second login with the same session (take-over).

Results of 2026-10-14 (v1.09–1.10, #19, #30), all against the bench over a ~47 ms path.
Device configuration was not changed: binaries, scripts and configs ran from `/tmp`
(service scripts as copies with their paths pointed there) and were removed afterwards.

| Device                                                                                                                     | Backend                      | Checked                                                                                                                                                                                                                                                                                                                                                                 |
|----------------------------------------------------------------------------------------------------------------------------|------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 2 × OpenIPC camera, SigmaStar SSC337 (armv7, 27 MiB RAM), `vtesse-arm32` / `vtesse-tap-arm32`                              | TAP (`--tap`, hook + udhcpc) | interface with MAC = device ID, DHCP address, option 121 routes, no default route; ping/HTTP to the device; 20 MiB at ~10 Mbit/s on both at once; **inbound**: the device opens the cameras' own web UI over the tunnel; take-over (reconnect ~2 s, same address); `type tun` with `ip=`/`route=`; clean stop; `S99vtesse` start/status/restart/stop; peak RSS ~148 KiB |
| same cameras                                                                                                               | lwIP (medium profile)        | DHCP + 121; HTTP and RTSP forwards; SOCKS5 used from the LAN (`--listen 0.0.0.0`); UDP relay (all datagrams arrive); take-over; link loss (DOWN by keepalive, reconnect); peak RSS ~412 KiB                                                                                                                                                                             |
| OpenWrt 15.05 router, MediaTek MT7620 (MIPS 24KEc, no FPU, 61 MiB RAM), `vtesse-mipsel` / `vtesse-tap-mipsel` (soft-float) | TAP                          | MAC = device ID, DHCP, option 121 routes, no default route, `resolv.conf` untouched; ping/HTTP to the device; 20 MiB at ~16 Mbit/s; **inbound**: the device opens the router's web UI over the tunnel; take-over (~2 s, same address); clean stop; `vtesse.init` under procd: start, respawn after `kill -9`, stop, output in the system log; peak RSS ~188 KiB         |
| same router                                                                                                                | lwIP (medium profile)        | DHCP + 121; HTTP/RTSP forwards; 20 MiB at ~9 Mbit/s; SOCKS5 from the LAN; UDP relay; link loss 65 s (DOWN by keepalive, reconnect); peak RSS ~468 KiB                                                                                                                                                                                                                   |
| Android 10 TV box (Amlogic, armv7)                                                                                         | app (large profile)          | server profile with a camera: RTSP/HTTP forwards on the scheme ports, SOCKS5, 20 MiB with sha256 check, UDP relay counters, link loss and server restart recovered by themselves, switching back to another server                                                                                                                                                      |

The router run found that hard-float MIPS binaries die with `Illegal instruction` on
router SoCs without an FPU (no FPU emulation in their kernels): since 1.10 the `mipsel`
and `mips` releases are soft-float (OpenWrt toolchains) and run on routers and Ingenic cameras alike
(checked on the MT7620 router and an Ingenic T23 camera).

Clean-up after device tests: remove the uploaded files (`/tmp/vtesse`), check that no
`vtesse*`/`vtun*` interface is left, and `bench/bench.sh up` again (localhost only).
