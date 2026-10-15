#include "netcore/stack.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/dhcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#define FRAME_MAX 2048

static struct {
    int initialised;
    tunnel_t tun;
    nc_stack_cfg_t cfg;
    nc_stack_event_cb cb;
    void *user;

    struct netif netif;
    int link;                   /* enum tunnel_link the netif is configured for */
    int dhcp_started;
    int bound;

    nc_routes_t routes;         /* in effect */
    int use_default;            /* default route via netif gw */
    ip4_addr_t gw_addr[NC_ROUTES_MAX];

    nc_routes_t pending;        /* from the last DHCPACK */
    int pending_valid;          /* 121 (2) or 249 (1) seen, 0 none */
    u32_t pending_xid;
    int lease_used;             /* dhcp->lease_used seen last: it restarts at 0 on every ACK */
    int dhcp_state;             /* dhcp->state seen last */

    uint8_t txbuf[FRAME_MAX];
} st;

/* ---- platform functions required by lwIP ---- */

u32_t sys_now(void)
{
    return (u32_t)nc_now_ms();
}

uint32_t nc_random32(void)
{
    static uint32_t s;

    if (!s) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            if (read(fd, &s, sizeof(s)) != (ssize_t)sizeof(s))
                s = 0;
            close(fd);
        }
        s ^= (uint32_t)nc_now_ms() ^ ((uint32_t)getpid() << 16);
        if (!s)
            s = 0x9e3779b9u;
    }
    s ^= s << 13;   /* xorshift32 */
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static void emit(int ev, int tev, int err)
{
    if (st.cb)
        st.cb(st.user, ev, tev, err);
}

/* ---- routes ---- */

static void apply_routes(const nc_routes_t *rt, int use_default)
{
    int i;

    st.routes = *rt;
    st.use_default = use_default;
    for (i = 0; i < rt->count; i++)
        ip4_addr_set_u32(&st.gw_addr[i], lwip_htonl(rt->r[i].gw));
}

struct netif *nc_hook_ip4_route(const ip4_addr_t *src, const ip4_addr_t *dest)
{
    (void)src;
    if (!st.initialised || !netif_is_up(&st.netif) || !netif_is_link_up(&st.netif))
        return NULL;
    return nc_stack_route_ok(lwip_ntohl(ip4_addr_get_u32(dest))) ? &st.netif : NULL;
}

const ip4_addr_t *nc_hook_etharp_get_gw(struct netif *netif, const ip4_addr_t *dest)
{
    const nc_route_t *r;

    if (netif != &st.netif)
        return NULL;
    r = nc_routes_lookup(&st.routes, lwip_ntohl(ip4_addr_get_u32(dest)));
    if (!r)
        return NULL;                    /* default route: netif gateway */
    if (!r->gw)
        return dest;                    /* on-link route */
    return &st.gw_addr[r - st.routes.r];
}

void nc_hook_dhcp_parse_option(struct netif *netif, struct dhcp *dhcp, u8_t state,
                               struct dhcp_msg *msg, u8_t msg_type, u8_t option,
                               u8_t len, struct pbuf *p, u16_t offset)
{
    uint8_t buf[255];
    nc_routes_t rt;
    int prio = option == 121 ? 2 : option == 249 ? 1 : 0;

    (void)dhcp;
    (void)state;
    if (netif != &st.netif || !prio || msg_type != DHCP_ACK)
        return;
    if (pbuf_copy_partial(p, buf, len, offset) != len)
        return;
    if (nc_routes_parse_classless(buf, len, &rt) != 0)
        return;                         /* malformed: ignore the option (RFC 3442) */
    if (st.pending_xid != lwip_ntohl(msg->xid))
        st.pending_valid = 0;
    if (prio < st.pending_valid)
        return;                         /* 121 wins over 249 */
    st.pending = rt;
    st.pending_valid = prio;
    st.pending_xid = lwip_ntohl(msg->xid);
}

int nc_stack_route_ok(uint32_t dst)
{
    uint32_t addr, mask;

    if (!st.bound)
        return 0;
    addr = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(&st.netif)));
    mask = lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(&st.netif)));
    if ((dst & mask) == (addr & mask))
        return 1;
    if (nc_routes_lookup(&st.routes, dst))
        return 1;
    return st.use_default && !ip4_addr_isany_val(*netif_ip4_gw(&st.netif));
}

/* ---- netif ---- */

static const uint8_t *flatten(struct pbuf *p, size_t *len)
{
    if (p->tot_len > sizeof(st.txbuf))
        return NULL;
    *len = p->tot_len;
    if (!p->next)
        return p->payload;
    pbuf_copy_partial(p, st.txbuf, p->tot_len, 0);
    return st.txbuf;
}

static err_t l2_linkoutput(struct netif *netif, struct pbuf *p)
{
    size_t len;
    const uint8_t *d = flatten(p, &len);

    (void)netif;
    if (d)
        st.tun.ops->send(st.tun.ctx, d, len);   /* drops are like lost frames */
    return ERR_OK;
}

static err_t l3_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    (void)ipaddr;
    return l2_linkoutput(netif, p);
}

static void configure_link(int link)
{
    struct netif *n = &st.netif;

    st.link = link;
    if (link == TUNNEL_LINK_L2) {
        n->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
        n->output = etharp_output;
        n->linkoutput = l2_linkoutput;
        n->input = ethernet_input;
    } else {
        n->flags &= (u8_t)~(NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET);
        n->output = l3_output;
        n->linkoutput = NULL;
        n->input = ip_input;
    }
}

static err_t netif_init_cb(struct netif *n)
{
    n->name[0] = 'v';
    n->name[1] = 't';
    n->mtu = 1500;
    n->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(n->hwaddr, st.cfg.mac, ETH_HWADDR_LEN);
    st.link = TUNNEL_LINK_NONE;
    configure_link(TUNNEL_LINK_L2);
    return ERR_OK;
}

/* Routes of the DHCPACK that made the lease current: option 121/249 if it had them. */
static void apply_dhcp_routes(const struct dhcp *d)
{
    int have121 = st.pending_valid && d && st.pending_xid == d->xid;
    nc_routes_t none;

    memset(&none, 0, sizeof(none));
    /* RFC 3442: with classless routes the router option is ignored */
    apply_routes(have121 ? &st.pending : &none, !have121 || st.cfg.opt3_default_route);
    st.pending_valid = 0;
    st.lease_used = d ? d->lease_used : 0;
    st.dhcp_state = DHCP_STATE_BOUND;
}

/* A renewal or a reboot after a reconnect keeps the address, so status_cb is not
 * called, and the DHCPREQUEST reuses the xid. A new ACK shows as a return to BOUND
 * (from RENEWING, REBINDING or REBOOTING), or, if no check saw the state in between,
 * as lease_used going back: dhcp_bind() restarts it at 0 on every ACK. Its routes
 * replace the old ones (none if it had none). */
static void check_renewal(void)
{
    const struct dhcp *d = netif_dhcp_data(&st.netif);
    int prev = st.dhcp_state;

    if (!d || st.cfg.static_ip)
        return;
    st.dhcp_state = d->state;
    if (!st.bound || d->state != DHCP_STATE_BOUND)
        return;
    if (prev != DHCP_STATE_BOUND || d->lease_used < st.lease_used)
        apply_dhcp_routes(d);
    st.lease_used = d->lease_used;
}

static void status_cb(struct netif *n)
{
    int has_addr = netif_is_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n));

    if (has_addr && !st.bound) {
        if (!st.cfg.static_ip)
            apply_dhcp_routes(netif_dhcp_data(n));
        st.bound = 1;
        emit(NC_STACK_EV_ADDR_BOUND, 0, 0);
    } else if (!has_addr && st.bound) {
        st.bound = 0;
        emit(NC_STACK_EV_ADDR_LOST, 0, 0);
    }
}

/* ---- tunnel callbacks ---- */

static int l2_accept(const uint8_t *f, size_t len)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint16_t type;

    if (len < 14)
        return 0;
    if (memcmp(f, st.cfg.mac, 6) != 0 && memcmp(f, bcast, 6) != 0)
        return 0;                       /* shared bridge: not for us */
    type = (uint16_t)(f[12] << 8 | f[13]);
    return type == 0x0800 || type == 0x0806;
}

static void on_rx(void *user, const uint8_t *pkt, size_t len)
{
    struct pbuf *p;

    (void)user;
    if (st.link == TUNNEL_LINK_L2 ? !l2_accept(pkt, len) : (len < 20 || (pkt[0] >> 4) != 4))
        return;
    p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p)
        return;
    pbuf_take(p, pkt, (u16_t)len);
    if (st.netif.input(p, &st.netif) != ERR_OK)
        pbuf_free(p);
    check_renewal();
}

static void on_tunnel_event(void *user, int ev, int err)
{
    (void)user;
    if (ev == TUNNEL_EV_UP) {
        int link = st.tun.ops->link_type(st.tun.ctx);

        if (link != st.link)
            configure_link(link);
        /* The L2 segment may have changed while we were away: a Linux bridge
         * without a fixed MAC takes a port's MAC, so the gateway MAC changes
         * when taps come and go. Forget all neighbours. */
        if (link == TUNNEL_LINK_L2)
            etharp_cleanup_netif(&st.netif);
        netif_set_up(&st.netif);
        netif_set_link_up(&st.netif);   /* restarts DHCP with the previous address */
        if (st.cfg.static_ip) {
            ip4_addr_t a, m, g;

            ip4_addr_set_u32(&a, lwip_htonl(st.cfg.addr));
            ip4_addr_set_u32(&m, lwip_htonl(st.cfg.mask));
            ip4_addr_set_u32(&g, lwip_htonl(st.cfg.gw));
            /* like option 121: explicit routes, or a default route via gw */
            apply_routes(&st.cfg.routes, st.cfg.gw != 0 && st.cfg.routes.count == 0);
            netif_set_addr(&st.netif, &a, &m, &g);
        } else if (link == TUNNEL_LINK_L2 && !st.dhcp_started) {
            st.dhcp_started = 1;
            dhcp_start(&st.netif);
        }
    } else if (ev == TUNNEL_EV_DOWN || ev == TUNNEL_EV_FAILED) {
        /* keep the lease; it is requested again after reconnect */
        netif_set_link_down(&st.netif);
    }
    emit(NC_STACK_EV_TUNNEL, ev, err);
}

/* ---- timers ---- */

static int timer_next(void *ctx)
{
    u32_t t = sys_timeouts_sleeptime();

    (void)ctx;
    if (t == SYS_TIMEOUTS_SLEEPTIME_INFINITE)
        return -1;
    return t > 0x7fffffff ? 0x7fffffff : (int)t;
}

static void timer_run(void *ctx)
{
    (void)ctx;
    sys_check_timeouts();
    check_renewal();
}

nc_source_t nc_stack_timer_source(void)
{
    nc_source_t s;

    memset(&s, 0, sizeof(s));
    s.next_timeout_ms = timer_next;
    s.on_timer = timer_run;
    return s;
}

/* ---- public ---- */

int nc_stack_init(const tunnel_t *t, const nc_stack_cfg_t *cfg, nc_stack_event_cb cb, void *user)
{
    if (st.initialised)
        return -1;
    memset(&st, 0, sizeof(st));
    st.tun = *t;
    st.cfg = *cfg;
    st.cb = cb;
    st.user = user;

    lwip_init();
    if (!netif_add_noaddr(&st.netif, NULL, netif_init_cb, ethernet_input))
        return -1;
    netif_set_status_callback(&st.netif, status_cb);
    /* no netif_set_default(): only routed destinations use the tunnel */

    t->ops->set_rx_cb(t->ctx, on_rx, NULL);
    t->ops->set_event_cb(t->ctx, on_tunnel_event, NULL);
    st.initialised = 1;
    return 0;
}

void nc_stack_get_info(nc_stack_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->bound = st.bound;
    if (!st.bound)
        return;
    info->addr = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(&st.netif)));
    info->mask = lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(&st.netif)));
    info->gw = lwip_ntohl(ip4_addr_get_u32(netif_ip4_gw(&st.netif)));
    info->default_route = st.use_default && info->gw != 0;
    info->routes = st.routes;
}

struct netif *nc_stack_netif(void)
{
    return &st.netif;
}
