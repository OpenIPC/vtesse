# Testing

| Check | Command | What it covers |
|---|---|---|
| Unit tests | `make test` | `test_proto` (wire format, stock-client reference vector), `test_route` (option 121/249 parser, lookup, device ID), `test_client` (client scenarios against a scripted server: happy path, `ERR`, compression, bad greeting, handshake and keepalive timeouts, reconnect after take-over) |
| Sanitizers | `make BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined" test` | memory errors, undefined behaviour, leaks |
| Big-endian | `ci/test-mips.sh` | static MIPS BE build in Docker, all unit tests under `qemu-mips` |
| Cross targets | `QEMU=1 ci/build-musl.sh <target>` | static musl build for a camera/router target (VTesse and the patched vtund), unit tests under qemu-user |
| Bench self-test | `bench/bench.sh selftest` | server behaviour VTesse relies on (greeting, flags, keepalive, DHCP through the tunnel, `killold`, `ERR` cases, `multi no`) |
| Stage 1 acceptance | `bench/bench.sh cli-test` | `vtesse-cli`: DHCP address and option 121 routes, option 3 ignored, HTTP through lwIP, ping of the client from the device, reconnect after a take-over with the same address |
| Stage 2 acceptance | `bench/bench.sh proxy-test` | SOCKS5 reply codes, `curl --socks5`, 20 MiB download and 8 MiB echo with integrity checks, RTSP through a forward, 8 parallel downloads, SOCKS5 authentication |
| TAP backend | `bench/bench.sh tap-test` | `vtesse-cli --tap` (`LWIP=0` build) in a container: DHCP via udhcpc, option 121 route, no default route, HTTP and a 20 MiB download from any process, **inbound** ping and TCP from the device, reconnect after a take-over keeps interface and address, clean exit |
| Reference client | `bench/bench.sh stock-test` | the stock `vtund` client + `dhclient` reach the device through the bench |

## CI

[`.github/workflows/build.yml`](../.github/workflows/build.yml) runs on every push
and pull request:

| Job | What it does |
|---|---|
| `tests` | `make test`, the same under ASan/UBSan, the `LWIP=0` build |
| `build` (arm32, mipsel, arm64, mips, x86_64) | static musl binaries with `ci/build-musl.sh` (`vtesse`, `vtesse-tap`, patched `vtund`), unit tests and a `vtund -h` smoke test under qemu-user, size report, artifacts |
| `bench` | the Docker test bench (vtund built with our patches): `selftest`, `cli-test`, `proxy-test`, `tap-test` |
| `release` | push to `main`: rolling `latest` pre-release and, for a new version, release `vMAJOR.MM`; binaries, `vtund-<VTun version>-<arch>`, hook scripts, `SHA256SUMS` |

### Versions and releases

The version lives in one place, `include/vtesse/version.h`:
`#define VTESSE_VERSION "MAJOR.MM"` (two-digit minor: `1.00`, `1.01`, …; shown by
`vtesse-cli --version`).

- Every push to `main` rebuilds the `latest` pre-release (moved to that commit).
- The first build of a version that has no release yet also publishes release
  `vMAJOR.MM` at the built commit, with release notes since the previous `v1.*`
  release. A push without a version bump (docs, CI) updates only `latest`.
- File names: `vtesse-<arch>` in `latest`, `vtesse-MAJOR.MM-<arch>` in `vMAJOR.MM`; the
  VTun server is `vtund-<VTun version>-<arch>` (e.g. `vtund-3.0.2-arm64`) in both, and
  the notes list the VTun version and the applied patches
  ([server/README.md](../server/README.md)).
- To release: bump `VTESSE_VERSION` in a commit to `main`; no manual tags.

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
