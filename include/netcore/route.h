/*
 * Route table for the tunnel: classless static routes from DHCP option 121
 * (RFC 3442) or 249, plus helpers for the device ID / MAC format.
 * Addresses are IPv4 in host byte order.
 */
#ifndef NETCORE_ROUTE_H
#define NETCORE_ROUTE_H

#include <stddef.h>
#include <stdint.h>

#define NC_ROUTES_MAX 32

typedef struct nc_route {
    uint32_t net;       /* network, masked */
    uint32_t mask;
    uint32_t gw;        /* 0 = on-link */
    uint8_t  prefix;
} nc_route_t;

typedef struct nc_routes {
    nc_route_t r[NC_ROUTES_MAX];
    int count;
} nc_routes_t;

/* Parse an option 121/249 payload. Returns 0, or -1 if malformed (the whole
 * option must then be ignored, RFC 3442). */
int nc_routes_parse_classless(const uint8_t *opt, size_t len, nc_routes_t *out);

/* Add a route (net is masked). -1 if the table is full or prefix > 32. */
int nc_routes_add(nc_routes_t *rt, uint32_t net, uint8_t prefix, uint32_t gw);

/* Longest-prefix match, NULL if no route. */
const nc_route_t *nc_routes_lookup(const nc_routes_t *rt, uint32_t dst);

/* "a.b.c.d/p via g" */
void nc_route_format(const nc_route_t *r, char *out, size_t size);
void nc_ip4_format(uint32_t ip, char *out, size_t size);

/* Device ID: 12 characters 0-9A-F (upper case), no separators; the first
 * byte must not have the multicast bit set. Fills mac. 0 on success. */
int nc_devid_to_mac(const char *id, uint8_t mac[6]);

/* Device ID from a MAC in the form "aa:bb:cc:dd:ee:ff" (as in
 * /sys/class/net/<if>/address; either case, trailing newline allowed).
 * Writes 12 upper case characters and a NUL to id. -1 for a malformed,
 * multicast or all-zero MAC. */
int nc_devid_from_mac_str(const char *mac, char id[13]);

#endif
