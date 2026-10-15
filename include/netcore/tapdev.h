/*
 * TAP/TUN backend (Linux): passes tunnel frames to a kernel interface
 * created through /dev/net/tun instead of the userspace stack.
 *
 * The kernel owns addressing and routing; every program on the device uses
 * the tunnel, inbound connections included. The interface is created on the
 * first tunnel UP (TAP for L2 sessions, TUN for L3) and kept across reconnects;
 * while the tunnel is down the carrier is off, which also makes the kernel
 * forget its ARP neighbours (the remote bridge MAC may change).
 *
 * Requires root or CAP_NET_ADMIN and the tun driver. Not for Android.
 */
#ifndef NETCORE_TAPDEV_H
#define NETCORE_TAPDEV_H

#include <stdint.h>

#include "netcore/loop.h"
#include "netcore/tunnel.h"

enum nc_tap_event {
    NC_TAP_EV_TUNNEL = 1,   /* tunnel event; see tunnel_event / err */
    NC_TAP_EV_CREATED,      /* interface created and up (first tunnel UP) */
    NC_TAP_EV_RECONNECTED,  /* tunnel up again, same interface */
    NC_TAP_EV_ERROR         /* interface could not be created; err = errno */
};

typedef void (*nc_tap_event_cb)(void *user, int event, int tunnel_event, int err);

typedef struct nc_tap_cfg {
    uint8_t mac[6];         /* TAP MAC (from the device ID) */
    const char *ifname;     /* requested name, default "vtesse%d" */
    int mtu;                /* default 1500 */
} nc_tap_cfg_t;

typedef struct nc_tap_stats {
    unsigned long frames_in, frames_out, dropped;
} nc_tap_stats_t;

typedef struct nc_tap nc_tap_t;

/* Attaches to the tunnel (takes over its rx and event callbacks). */
nc_tap_t   *nc_tap_create(const tunnel_t *t, const nc_tap_cfg_t *cfg, nc_tap_event_cb cb, void *user);
void        nc_tap_destroy(nc_tap_t *tap);
nc_source_t nc_tap_source(nc_tap_t *tap);
const char *nc_tap_ifname(const nc_tap_t *tap);    /* "" before creation */
int         nc_tap_link(const nc_tap_t *tap);      /* enum tunnel_link of the interface */
void        nc_tap_get_stats(const nc_tap_t *tap, nc_tap_stats_t *st);

#endif
