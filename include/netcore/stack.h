/*
 * Userspace IPv4 stack (lwIP) on top of a tunnel.
 *
 * lwIP keeps global state, so there is exactly one stack per process.
 * The stack takes over the tunnel's rx and event callbacks and forwards
 * tunnel events to its own callback.
 */
#ifndef NETCORE_STACK_H
#define NETCORE_STACK_H

#include <stdint.h>

#include "netcore/loop.h"
#include "netcore/route.h"
#include "netcore/tunnel.h"

enum nc_stack_event {
    NC_STACK_EV_TUNNEL = 1,     /* tunnel event; see tunnel_event / err */
    NC_STACK_EV_ADDR_BOUND,     /* address (and routes) configured */
    NC_STACK_EV_ADDR_LOST
};

typedef void (*nc_stack_event_cb)(void *user, int event, int tunnel_event, int err);

typedef struct nc_stack_cfg {
    uint8_t mac[6];             /* own MAC for L2 links (from the device ID) */
    int static_ip;              /* 1: use addr/mask/gw/routes below instead of DHCP */
    int learn_ip;               /* type tun (#68): the address is the destination of the first
                                 * unicast packet, gw its source; routes below, none: default */
    uint32_t addr, mask, gw;    /* host byte order */
    nc_routes_t routes;         /* static routes (static_ip, learn_ip); none: default route via gw */
    int opt3_default_route;     /* debug: keep option 3 as default route even with 121 */
} nc_stack_cfg_t;

typedef struct nc_stack_info {
    int bound;
    uint32_t addr, mask, gw;    /* gw: option 3 (informational when routes come from 121) */
    int default_route;          /* a default route via the tunnel is in effect */
    nc_routes_t routes;         /* classless routes in effect */
} nc_stack_info_t;

/* Initialise lwIP and attach it to the tunnel. Call once. */
int  nc_stack_init(const tunnel_t *t, const nc_stack_cfg_t *cfg, nc_stack_event_cb cb, void *user);

/* lwIP timers as a loop source. */
nc_source_t nc_stack_timer_source(void);

/* Is dst (host order) reachable through the tunnel? */
int  nc_stack_route_ok(uint32_t dst);

void nc_stack_get_info(nc_stack_info_t *info);

/* Opaque access for protocol code built on lwIP (proxy, tests). */
struct netif *nc_stack_netif(void);

#endif
