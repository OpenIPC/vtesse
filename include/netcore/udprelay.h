/*
 * UDP relay (#28): receive UDP datagrams addressed to the client's tunnel
 * address (lwIP stack) on a port and send each one, unchanged, to a host
 * address (by default 127.0.0.1) on the device.
 *
 * Use: a camera pushes RTP over UDP (OpenIPC Majestic "outgoing") to
 * udp://<tunnel address>:PORT; a local player listens on 127.0.0.1:LOCAL_PORT
 * (OpenIPC Decoder: udp://@:LOCAL_PORT). One direction only: nothing is sent
 * back to the camera.
 *
 * Inside the TCP tunnel the datagrams arrive reliably and in order; a datagram
 * is dropped only if the local socket's buffer is full (the reader is slow).
 */
#ifndef NETCORE_UDPRELAY_H
#define NETCORE_UDPRELAY_H

#include <stddef.h>
#include <stdint.h>

#define NC_UDP_RELAY_MAX 3          /* lwIP UDP PCBs left besides DHCP */

typedef struct nc_udp_relay_stats {
    unsigned long datagrams;        /* relayed */
    unsigned long dropped;          /* local send failed */
    unsigned long long bytes;
} nc_udp_relay_stats_t;

typedef struct nc_udp_relay nc_udp_relay_t;

/* Listen on UDP port `port` of the stack (call after nc_stack_init) and relay to
 * dst_addr:dst_port (dst_addr NULL = "127.0.0.1", dst_port 0 = port).
 * NULL on error, with a message in err. */
nc_udp_relay_t *nc_udp_relay_create(uint16_t port, const char *dst_addr, uint16_t dst_port,
                                    char *err, size_t err_size);
void nc_udp_relay_destroy(nc_udp_relay_t *r);
void nc_udp_relay_get_stats(const nc_udp_relay_t *r, nc_udp_relay_stats_t *st);

/* "PORT" or "PORT:LOCAL_PORT" -> 0 on success. */
int nc_udp_relay_parse(const char *s, uint16_t *port, uint16_t *local_port);

#endif
