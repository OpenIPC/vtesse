/* lwIP configuration for VTesse: NO_SYS, raw API, IPv4 only. */
#ifndef VTESSE_LWIPOPTS_H
#define VTESSE_LWIPOPTS_H

/* ---- system ---- */
#define NO_SYS                          1
#define LWIP_TIMERS                     1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_STATS                      0
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1   /* use the system htons/ntohl */
#define LWIP_HOOK_FILENAME              "nc_lwip_hooks.h"   /* prototypes */
#define LWIP_HOOK_IP4_ROUTE_SRC(src, dest)      nc_hook_ip4_route(src, dest)
#define LWIP_HOOK_ETHARP_GET_GW(netif, dest)    nc_hook_etharp_get_gw(netif, dest)
#define LWIP_HOOK_DHCP_PARSE_OPTION(netif, dhcp, state, msg, msg_type, option, len, pbuf, offset) \
    nc_hook_dhcp_parse_option(netif, dhcp, state, msg, msg_type, option, len, pbuf, offset)

/* ---- memory ---- */
#ifndef VTESSE_LWIP_SMALL
/* Throughput of one connection is at most window / RTT, and the RTT to a VTun
 * server is often 100-200 ms: a 16 x MSS window gave ~1.3 Mbit/s at 140 ms, too
 * little for a 1080p camera stream (the camera dropped frames, #24). ~125 KiB with
 * window scaling allows ~7 Mbit/s at 140 ms. The proxy acknowledges data only
 * after writing it locally, so pbufs pile up only while a local reader is slow. */
#define MEM_SIZE                        (512 * 1024)
#define PBUF_POOL_SIZE                  512
#define MEMP_NUM_PBUF                   64
#define MEMP_NUM_TCP_PCB                32
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   2
#define TCP_WND                         (88 * TCP_MSS)
#define TCP_SND_BUF                     (64 * TCP_MSS)
#define MEMP_NUM_TCP_SEG                512
#else   /* cameras / routers */
#define MEM_SIZE                        (48 * 1024)
#define PBUF_POOL_SIZE                  24
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_TCP_PCB                8
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define MEMP_NUM_TCP_SEG                40
#endif
/* lwIP structs hold pointers: align to the pointer size (8 on 64-bit) */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
#define MEM_ALIGNMENT                   8
#else
#define MEM_ALIGNMENT                   4
#endif
#define MEMP_NUM_UDP_PCB                4
#define MEMP_NUM_TCP_PCB_LISTEN         1
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

/* ---- protocols ---- */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define ARP_QUEUEING                    1
#define LWIP_ICMP                       1
#define LWIP_RAW                        0
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_IGMP                       0
#define LWIP_DNS                        0
#define LWIP_AUTOIP                     0
#define LWIP_ACD                        0
#define IP_FORWARD                      0
#define IP_REASSEMBLY                   1
#define IP_FRAG                         1

/* TCP over a TCP tunnel: be patient, no aggressive retransmits. */
#define TCP_MSS                         1460
#define LWIP_TCP_KEEPALIVE              1
#define TCP_MAXRTX                      12
#define TCP_SYNMAXRTX                   6

/* ---- DHCP ---- */
#define LWIP_DHCP                       1
#define LWIP_DHCP_DOES_ACD_CHECK        0
#define LWIP_DHCP_CHECK_LINK_UP         1
/* request classless static routes (RFC 3442) and the Microsoft variant */
#define DHCP_ADD_EXTRA_REQUEST_OPTIONS  , 121, 249

/* ---- netif ---- */
#define LWIP_SINGLE_NETIF               0   /* routing goes through LWIP_HOOK_IP4_ROUTE_SRC */
#define LWIP_HAVE_LOOPIF                0
#define LWIP_NETIF_LOOPBACK             0
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_HOSTNAME             0

#endif
