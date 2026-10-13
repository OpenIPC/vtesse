# Testing

| Check | Command | What it covers |
|---|---|---|
| Unit tests | `make test` | `test_proto` (wire format, stock-client reference vector), `test_route` (option 121/249 parser, lookup, device ID), `test_client` (client scenarios against a scripted server: happy path, `ERR`, compression, bad greeting, handshake and keepalive timeouts, reconnect after take-over) |
| Sanitizers | `make BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined" test` | memory errors, undefined behaviour, leaks |
| Big-endian | `ci/test-mips.sh` | static MIPS BE build in Docker, all unit tests under `qemu-mips` |
| Bench self-test | `bench/bench.sh selftest` | server behaviour VTesse relies on (greeting, flags, keepalive, DHCP through the tunnel, `killold`, `ERR` cases, `multi no`) |
| Stage 1 acceptance | `bench/bench.sh cli-test` | `vtesse-cli`: DHCP address and option 121 routes, option 3 ignored, HTTP through lwIP, ping of the client from the device, reconnect after a take-over with the same address |
| Stage 2 acceptance | `bench/bench.sh proxy-test` | SOCKS5 reply codes, `curl --socks5`, 20 MiB download and 8 MiB echo with integrity checks, RTSP through a forward, 8 parallel downloads, SOCKS5 authentication |
| Reference client | `bench/bench.sh stock-test` | the stock `vtund` client + `dhclient` reach the device through the bench |

## Test bench

[`bench/`](../bench/README.md): Docker image with a VTun 3.0.2 server built without
OpenSSL, a bridged segment with a DHCP server (option 121, ping check), and a
"remote device" behind a router (HTTP with a 20 MiB file, RTSP stub, TCP echo).

```sh
bench/bench.sh build
bench/bench.sh up
make
bench/bench.sh selftest
bench/bench.sh cli-test
bench/bench.sh proxy-test
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
