# Embedding

How to use `libvtesse` and `libnetcore` inside another application.

- [Model](#model)
- [Linking](#linking)
- [Setup](#setup)
- [Driving the loop](#driving-the-loop)
- [Rules](#rules)
- [Example](#example)

## Model

- Single-threaded and non-blocking. Nothing in the libraries creates threads or runs
  its own loop; the host calls readiness handlers and timers.
- Three loop sources, each an `nc_source_t` ([`loop.h`](../include/netcore/loop.h)):
  1. the tunnel — `nc_source_from_tunnel()`: one fd (it changes on reconnect),
     read/write readiness, timers (handshake, keepalive, reconnect);
  2. the stack timers — `nc_stack_timer_source()`: timers only (lwIP);
  3. the proxy — `nc_proxy_source()`: many fds (`poll_fill` / `poll_dispatch`) and
     connect timers.
- The stack takes over the tunnel's rx and event callbacks and reports tunnel events
  plus `NC_STACK_EV_ADDR_BOUND` / `NC_STACK_EV_ADDR_LOST` through its own callback.
- One stack per process (lwIP keeps global state).

## Linking

```sh
make
cc -std=c99 -D_POSIX_C_SOURCE=200809L \
   -Iinclude -Iport/lwip -Ithird_party/lwip/src/include \
   app.c build/libvtesse.a build/libnetcore.a
```

Code that uses lwIP directly (its own TCP connections instead of the proxy) needs
the same include paths; `port/lwip/lwipopts.h` must be the configuration lwIP was
built with. For small devices build everything with `-DVTESSE_LWIP_SMALL`.

## Setup

1. `vt_create()` with host, port, session (device ID), password, `VT_AUTH_XOR`; the
   strings are copied. Optional tuning fields in `vt_cfg_t` (timeouts, keepalive,
   data delay, backoff, buffer size); 0 selects the default.
2. `nc_stack_init()` with the tunnel, the MAC (`nc_devid_to_mac()`) and an event
   callback. DHCP is used unless `static_ip` is set.
3. Optionally `nc_proxy_create()` for SOCKS5 / port forwards, or open lwIP TCP
   connections yourself; check `nc_stack_route_ok()` first.
4. `vt_start()`.

Shutdown: `t.ops->close(vt)` (sends `CONN_CLOSE`), `nc_proxy_destroy()`,
`vt_destroy()`.

## Driving the loop

Either add the sources to `nc_loop_t` and call `nc_loop_run()` /
`nc_loop_run_once()` ([`loop.c`](../netcore/loop.c)), or integrate them into an
existing loop:

- before waiting: collect fds (`get_fd`, `wants_write` → `POLLOUT`, or `poll_fill`)
  and the smallest `next_timeout_ms` (0 = due now, -1 = none);
- after waiting: call `on_writable` / `on_readable` (re-check `get_fd` — it may have
  changed inside a handler) or `poll_dispatch`; then `on_timer` for every source whose
  `next_timeout_ms` returns 0.

## Rules

- Call everything from one thread. On Android run the loop in a dedicated thread
  and pass commands to it (e.g. through a pipe added as another source).
- Do not destroy objects from inside callbacks; stop the loop first.
- `send()` never blocks and never splits a packet; `-1` means dropped.
- `getaddrinfo()` runs at connect time and may block briefly; pass an IPv4 literal
  as `host` to avoid it.

## Example

A host with its own `poll()` loop: tunnel + DHCP + SOCKS5 on port 1080 + a forward
`127.0.0.1:8554 → 10.78.0.10:554`.

```c
#include <poll.h>
#include <stdio.h>
#include <string.h>

#include "netcore/loop.h"
#include "netcore/proxy.h"
#include "netcore/route.h"
#include "netcore/stack.h"
#include "vtesse/vtun_client.h"

static void on_stack_event(void *user, int ev, int tunnel_ev, int err)
{
    (void)user;
    if (ev == NC_STACK_EV_ADDR_BOUND)
        printf("address bound\n");
    else if (ev == NC_STACK_EV_TUNNEL && err)
        printf("tunnel %s: %s\n", tunnel_event_name(tunnel_ev), tunnel_strerror(err));
}

int main(void)
{
    vt_cfg_t vc;
    nc_stack_cfg_t sc;
    nc_proxy_cfg_t pc;
    vt_t *vt;
    nc_proxy_t *px;
    tunnel_t t;
    nc_source_t src[3];
    int err, i;

    memset(&vc, 0, sizeof(vc));
    vc.host = "vtun.example.org";
    vc.port = 5555;
    vc.session = "02AB34CD56EF";            /* device ID */
    vc.password = "secret";
    vc.auth = VT_AUTH_XOR;
    vt = vt_create(&vc, &err);              /* copies the strings */
    if (!vt)
        return 1;
    t = vt_tunnel(vt);

    memset(&sc, 0, sizeof(sc));
    nc_devid_to_mac(vc.session, sc.mac);    /* DHCP by default */
    nc_stack_init(&t, &sc, on_stack_event, NULL);

    memset(&pc, 0, sizeof(pc));
    pc.socks_port = 1080;
    pc.forwards[0].local_port = 8554;
    pc.forwards[0].ip = 0x0a4e000a;         /* 10.78.0.10, host byte order */
    pc.forwards[0].port = 554;
    pc.forward_count = 1;
    px = nc_proxy_create(&pc, NULL, NULL);
    if (!px)
        return 1;

    src[0] = nc_source_from_tunnel(&t);
    src[1] = nc_stack_timer_source();
    src[2] = nc_proxy_source(px);
    vt_start(vt);

    for (;;) {                              /* the host's own loop */
        struct pollfd pfd[64];
        int first[3], count[3], n = 0, timeout = -1;

        for (i = 0; i < 3; i++) {
            int tmo = src[i].next_timeout_ms ? src[i].next_timeout_ms(src[i].ctx) : -1;
            if (tmo >= 0 && (timeout < 0 || tmo < timeout))
                timeout = tmo;
            first[i] = n;
            count[i] = 0;
            if (src[i].poll_fill) {
                count[i] = src[i].poll_fill(src[i].ctx, pfd + n, 64 - n);
            } else if (src[i].get_fd && src[i].get_fd(src[i].ctx) >= 0) {
                pfd[n].fd = src[i].get_fd(src[i].ctx);
                pfd[n].events = POLLIN;
                if (src[i].wants_write && src[i].wants_write(src[i].ctx))
                    pfd[n].events |= POLLOUT;
                count[i] = 1;
            }
            n += count[i];
        }
        if (poll(pfd, (nfds_t)n, timeout) < 0)
            continue;
        for (i = 0; i < 3; i++) {
            struct pollfd *p = pfd + first[i];
            if (!count[i])
                continue;
            if (src[i].poll_dispatch) {
                src[i].poll_dispatch(src[i].ctx, p, count[i]);
                continue;
            }
            if (src[i].get_fd(src[i].ctx) != p->fd)
                continue;                   /* fd changed (reconnect) */
            if ((p->revents & (POLLOUT | POLLERR | POLLHUP)) && src[i].wants_write(src[i].ctx))
                src[i].on_writable(src[i].ctx);
            if (src[i].get_fd(src[i].ctx) == p->fd && (p->revents & (POLLIN | POLLERR | POLLHUP)))
                src[i].on_readable(src[i].ctx);
        }
        for (i = 0; i < 3; i++)
            if (src[i].next_timeout_ms && src[i].next_timeout_ms(src[i].ctx) == 0)
                src[i].on_timer(src[i].ctx);
    }
}
```
