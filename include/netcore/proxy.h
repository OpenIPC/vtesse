/*
 * Local proxy: SOCKS5 (CONNECT) and static TCP port forwards from host
 * sockets to destinations behind the tunnel, through the lwIP stack.
 *
 * Outbound TCP only, IPv4 only. Flow control is end to end: data is read from
 * a local socket only when lwIP has send buffer space, and data received from
 * lwIP is acknowledged (window reopened) only after it was written locally.
 */
#ifndef NETCORE_PROXY_H
#define NETCORE_PROXY_H

#include <stdint.h>

#include "netcore/loop.h"

#define NC_PROXY_MAX_FORWARDS 16

typedef struct nc_forward {
    uint16_t local_port;
    uint32_t ip;                /* host byte order */
    uint16_t port;
} nc_forward_t;

typedef struct nc_proxy_cfg {
    const char *listen_addr;    /* default "127.0.0.1" */
    uint16_t socks_port;        /* 0 = no SOCKS5 server */
    const char *socks_user;     /* optional RFC 1929 credentials (both or none) */
    const char *socks_password;
    nc_forward_t forwards[NC_PROXY_MAX_FORWARDS];
    int forward_count;
    int max_conns;              /* default 24 (bounded by lwIP TCP PCBs) */
    int connect_timeout_ms;     /* default 15000 */
} nc_proxy_cfg_t;

typedef struct nc_proxy_stats {
    unsigned long accepted, rejected, connected, failed, active;
    unsigned long long bytes_up, bytes_down;
} nc_proxy_stats_t;

typedef struct nc_proxy nc_proxy_t;

typedef void (*nc_proxy_log_cb)(void *user, const char *msg);

/* Opens the listening sockets. NULL on error (errno-style message via log). */
nc_proxy_t *nc_proxy_create(const nc_proxy_cfg_t *cfg, nc_proxy_log_cb log, void *user);
void        nc_proxy_destroy(nc_proxy_t *px);
nc_source_t nc_proxy_source(nc_proxy_t *px);
void        nc_proxy_get_stats(const nc_proxy_t *px, nc_proxy_stats_t *st);

#endif
