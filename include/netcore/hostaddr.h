/*
 * Addresses of the host itself (not of the tunnel).
 */
#ifndef NETCORE_HOSTADDR_H
#define NETCORE_HOSTADDR_H

#include <stdint.h>

/* The IPv4 address (host byte order) of the interface of the first default route
 * in /proc/net/route; 0: no default route or no IPv4 address on it. Inbound
 * forwards (#67) go there by default: the camera's own LAN address. */
uint32_t nc_default_route_addr(void);

#endif
