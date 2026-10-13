/*
 * Protocol-agnostic tunnel interface.
 *
 * Everything above the transport (userspace stack, proxy, JNI) talks to a
 * tunnel only through tunnel_ops_t, so the same layers can serve VTun or
 * another tunnel protocol. A tunnel never blocks and never owns a loop: the
 * host drives it through its file descriptor, readiness handlers and timers.
 */
#ifndef NETCORE_TUNNEL_H
#define NETCORE_TUNNEL_H

#include <stddef.h>
#include <stdint.h>

enum tunnel_link {
    TUNNEL_LINK_NONE = 0,   /* not established yet */
    TUNNEL_LINK_L2 = 1,     /* Ethernet frames */
    TUNNEL_LINK_L3 = 2      /* raw IPv4 packets */
};

enum tunnel_event {
    TUNNEL_EV_CONNECTING = 1,   /* a connection attempt has started */
    TUNNEL_EV_UP,               /* link established, packets may flow */
    TUNNEL_EV_DOWN,             /* link lost; the tunnel will retry by itself */
    TUNNEL_EV_FAILED            /* stopped for good (configuration or server policy) */
};

enum tunnel_err {
    TUNNEL_OK = 0,
    TUNNEL_ERR_CONFIG,
    TUNNEL_ERR_NOMEM,
    TUNNEL_ERR_RESOLVE,
    TUNNEL_ERR_CONNECT,
    TUNNEL_ERR_TIMEOUT,
    TUNNEL_ERR_IO,
    TUNNEL_ERR_PEER_CLOSED,
    TUNNEL_ERR_KEEPALIVE,
    TUNNEL_ERR_PROTOCOL,
    TUNNEL_ERR_AUTH,
    TUNNEL_ERR_UNSUPPORTED_TYPE,
    TUNNEL_ERR_UNSUPPORTED_TRANSPORT,
    TUNNEL_ERR_COMPRESSION,
    TUNNEL_ERR_ENCRYPTION,
    TUNNEL_ERR_CLOSED            /* closed locally */
};

const char *tunnel_strerror(int err);
const char *tunnel_event_name(int event);

typedef void (*tunnel_rx_cb)(void *user, const uint8_t *pkt, size_t len);
typedef void (*tunnel_event_cb)(void *user, int event, int err);

typedef struct tunnel_ops {
    /* Socket to poll, or -1 when there is none (e.g. waiting to reconnect).
     * May change between calls: re-query on every loop iteration. */
    int  (*get_fd)(void *ctx);
    /* Non-zero when POLLOUT is needed (connect in progress, pending output). */
    int  (*wants_write)(void *ctx);
    void (*on_readable)(void *ctx);
    void (*on_writable)(void *ctx);
    /* Milliseconds until on_timer must run: 0 = due now, -1 = no timer. */
    int  (*next_timeout_ms)(void *ctx);
    void (*on_timer)(void *ctx);
    void (*set_rx_cb)(void *ctx, tunnel_rx_cb cb, void *user);
    void (*set_event_cb)(void *ctx, tunnel_event_cb cb, void *user);
    /* Queue one whole packet. Returns 0 if accepted, -1 if dropped
     * (link not up, too large, or output buffer full). Never splits. */
    int  (*send)(void *ctx, const uint8_t *pkt, size_t len);
    int  (*link_type)(void *ctx);    /* enum tunnel_link */
    int  (*mtu)(void *ctx);          /* payload MTU of the link layer */
    void (*close)(void *ctx);        /* stop; no further events except none */
} tunnel_ops_t;

typedef struct tunnel {
    const tunnel_ops_t *ops;
    void *ctx;
} tunnel_t;

#endif
