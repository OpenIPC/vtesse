# VTun server patches

Applied in order to the vtun sources when the bench image is built
(`bench/Dockerfile`, `patch -p1`). Files are named `vtun-<version>-<topic>.patch`;
only the patches for the version being built are applied.

| Patch | What it does |
|---|---|
| `vtun-3.0.2-denied-log.patch` | A refused login is logged with the session name the client sent and the reason (`wrong password`, `unknown session`, `already connected (multi no) or lock error`, `no session name received`, …) instead of only `Denied connection from IP:PORT`. The protocol is unchanged. |

These patches apply to any vtund server, not only the bench, so they can also be
used on a production VTun server.
