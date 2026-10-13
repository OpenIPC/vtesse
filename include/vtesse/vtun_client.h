/*
 * VTun client (TCP transport, type tun / ether, no compression/encryption).
 *
 * Non-blocking, single-threaded, no global state. Exposes tunnel_ops_t.
 * Name resolution uses getaddrinfo() and may block briefly at connect time.
 */
#ifndef VTESSE_VTUN_CLIENT_H
#define VTESSE_VTUN_CLIENT_H

#include <stdint.h>

#include "netcore/tunnel.h"
#include "vtesse/vtun_proto.h"

enum vt_auth {
    VT_AUTH_XOR = 0        /* servers built without OpenSSL */
};

typedef struct vt_cfg {
    const char *host;
    uint16_t    port;
    const char *session;
    const char *password;
    int         auth;                   /* enum vt_auth */

    /* Optional tuning; 0 selects the default. */
    int handshake_timeout_ms;           /* per step, default 15000 (CHAL step >= 10000) */
    int ka_interval_ms;                 /* own ECHO_REQ period, default 10000 */
    int ka_count;                       /* dead after ka_count * interval silence, default 5 */
    int data_delay_ms;                  /* wait after OK FLAGS before data, default 500 */
    int reconnect_min_ms;               /* default 1000 */
    int reconnect_max_ms;               /* default 30000 */
    int out_buffer_size;                /* default 65536 */
    int no_reconnect;                   /* 1: report FAILED instead of retrying */
} vt_cfg_t;

typedef struct vt vt_t;

/* Copies the configuration. Returns NULL and sets *err on invalid config / OOM. */
vt_t *vt_create(const vt_cfg_t *cfg, int *err);
void  vt_destroy(vt_t *vt);

/* Start connecting (first attempt happens immediately). */
int   vt_start(vt_t *vt);

/* Tunnel interface. */
const tunnel_ops_t *vt_tunnel_ops(void);
tunnel_t vt_tunnel(vt_t *vt);

/* Diagnostics. */
const char       *vt_greeting(const vt_t *vt);     /* last server greeting, "" if none */
const vt_flags_t *vt_session_flags(const vt_t *vt); /* flags of the last session */
int               vt_is_up(const vt_t *vt);

#endif
