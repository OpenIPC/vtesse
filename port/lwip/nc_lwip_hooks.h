/* lwIP hooks implemented by netcore/stack.c. */
#ifndef VTESSE_NC_LWIP_HOOKS_H
#define VTESSE_NC_LWIP_HOOKS_H

#include "lwip/arch.h"
#include "lwip/ip4_addr.h"

struct netif;
struct dhcp;
struct dhcp_msg;
struct pbuf;

/* Only destinations covered by the route table go to the tunnel netif. */
struct netif *nc_hook_ip4_route(const ip4_addr_t *src, const ip4_addr_t *dest);
/* Next hop for off-subnet destinations (per-route gateway). */
const ip4_addr_t *nc_hook_etharp_get_gw(struct netif *netif, const ip4_addr_t *dest);
/* Options 121 / 249 in DHCP replies. */
void nc_hook_dhcp_parse_option(struct netif *netif, struct dhcp *dhcp, u8_t state,
                               struct dhcp_msg *msg, u8_t msg_type, u8_t option,
                               u8_t len, struct pbuf *p, u16_t offset);

/* The LWIP_HOOK_* macros are defined in lwipopts.h (ip4.h must see them). */

#endif
