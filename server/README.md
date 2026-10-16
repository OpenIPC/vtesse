# VTun server (vtund) with VTesse patches

VTesse talks to an unmodified VTun server, but every vtund **we** build — the
Docker test bench and the release binaries — is VTun with the patches from
[`patches/`](patches/). There is one build script for all of them:

```sh
server/build-vtund.sh build/vtund                                  # native
server/build-vtund.sh --cross /path/to/bin/aarch64-linux- --static build/vtund-arm64
server/build-vtund.sh --version                                    # VTun version it builds: 3.0.2
```

It downloads the original `vtun-3.0.2.tar.gz` (sha256 checked), applies every
`patches/vtun-3.0.2-*.patch` in name order — the build fails if there is none or one
does not apply — and configures VTun like the target deployment: no OpenSSL (XOR
challenge response), no LZO, no zlib. Requirements: a C compiler, `bison`, `flex`,
`patch`, `curl`; for `--cross` also `autotools-dev` or `automake` (current
`config.sub`/`config.guess`; the 2001 ones in the tarball do not know aarch64 or musl).

## Releases

CI builds a static musl vtund for PCs and servers only (x86_64, `ci/build-musl.sh`)
and attaches it to the [`latest`](https://github.com/OpenIPC/vtesse/releases/tag/latest)
release as `vtund-<VTun version>-x86_64`, e.g. `vtund-3.0.2-x86_64`. The name carries the
original VTun version, not the VTesse version; the applied patches are listed below.
The binary reports itself as upstream does (`VTun ver 3.X <build date>`),
and the protocol is unchanged, so stock and patched servers and clients mix freely.

Cameras and routers: vtund is not released for them; when needed it is built in the
OpenIPC environment with the same patches, e.g. with the OpenIPC toolchain:

```sh
server/build-vtund.sh --cross /path/to/toolchain/bin/arm-openipc-linux-musleabi- --static vtund
VTUND=1 ci/build-musl.sh arm32      # or together with VTesse, OpenIPC toolchain downloaded by the script
```

## Patches

| Patch                                                                | What it does                                                                                                                           |
|----------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------|
| [`vtun-3.0.2-denied-log.patch`](patches/vtun-3.0.2-denied-log.patch) | A refused login is logged with the session name the client sent and the reason instead of only `Denied connection from IP:PORT` (#22). |

Log lines with `vtun-3.0.2-denied-log.patch`:

```
Denied connection from 172.28.0.1:33016: session 02BE0C000001: wrong password
Denied connection from 172.28.0.1:53086: session 02BE0C0000FF: unknown session
Denied connection from 172.28.0.1:42658: session 02BE0C000005: already connected (multi no) or lock error
Denied connection from 172.28.0.1:40112: no session name received
```

A new patch: `patches/vtun-<version>-<topic>.patch`, `-p1` against the unpacked
tarball, with a first line that summarises it, a row in the table above and an entry
in [CHANGELOG.md](../CHANGELOG.md).
