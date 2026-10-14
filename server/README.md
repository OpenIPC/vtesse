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

CI builds a static musl vtund for every target (`ci/build-musl.sh`) and attaches it
to the [releases](https://github.com/compot-dev/vtesse/releases) as
`vtund-<VTun version>-<arch>`, e.g. `vtund-3.0.2-x86_64`, in `latest` and in the
versioned releases from v1.01 on. The name carries the
original VTun version, not the VTesse version; the release notes list the applied
patches. The binary reports itself as upstream does (`VTun ver 3.X <build date>`),
and the protocol is unchanged, so stock and patched servers and clients mix freely.

## Patches

| Patch | What it does |
|---|---|
| [`vtun-3.0.2-denied-log.patch`](patches/vtun-3.0.2-denied-log.patch) | A refused login is logged with the session name the client sent and the reason instead of only `Denied connection from IP:PORT` (#22). |

Log lines with `vtun-3.0.2-denied-log.patch`:

```
Denied connection from 172.28.0.1:33016: session 02BE0C000001: wrong password
Denied connection from 172.28.0.1:53086: session 02BE0C0000FF: unknown session
Denied connection from 172.28.0.1:42658: session 02BE0C000005: already connected (multi no) or lock error
Denied connection from 172.28.0.1:40112: no session name received
```

A new patch: `patches/vtun-<version>-<topic>.patch`, `-p1` against the unpacked
tarball, with a first line that summarises it (used in the release notes), and a row
in the table above.
