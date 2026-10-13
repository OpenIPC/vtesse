/*
 * Minimal poll()-based loop helper.
 *
 * Libraries never call this themselves; it exists for the CLI and tests.
 * Hosts with their own loop drive the same callbacks directly.
 */
#ifndef NETCORE_LOOP_H
#define NETCORE_LOOP_H

#include <stdint.h>

#include "netcore/tunnel.h"

#define NC_LOOP_MAX_SOURCES 16

typedef struct nc_source {
    int  (*get_fd)(void *ctx);          /* -1: no fd (timer-only source) */
    int  (*wants_write)(void *ctx);     /* may be NULL */
    void (*on_readable)(void *ctx);     /* may be NULL */
    void (*on_writable)(void *ctx);     /* may be NULL */
    int  (*next_timeout_ms)(void *ctx); /* may be NULL; 0 = due, -1 = none */
    void (*on_timer)(void *ctx);        /* may be NULL */
    void *ctx;
} nc_source_t;

typedef struct nc_loop {
    nc_source_t src[NC_LOOP_MAX_SOURCES];
    int count;
    volatile int stop;
} nc_loop_t;

/* Monotonic milliseconds. */
uint64_t nc_now_ms(void);

void nc_loop_init(nc_loop_t *loop);
int  nc_loop_add(nc_loop_t *loop, const nc_source_t *src);
nc_source_t nc_source_from_tunnel(const tunnel_t *t);

/* One poll() round, waiting at most max_wait_ms (-1 = until an fd or timer fires).
 * Returns the poll() result (< 0 on error other than EINTR). */
int  nc_loop_run_once(nc_loop_t *loop, int max_wait_ms);

/* Run until loop->stop is set or until deadline_ms (monotonic, 0 = no deadline). */
void nc_loop_run(nc_loop_t *loop, uint64_t deadline_ms);

#endif
