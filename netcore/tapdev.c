#include "netcore/tapdev.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>     /* before linux/if.h: old kernel headers need struct sockaddr */
#include <unistd.h>

#include <linux/if.h>
#include <linux/if_tun.h>

#ifndef ARPHRD_ETHER
#define ARPHRD_ETHER 1      /* linux/if_arp.h drags in netlink headers */
#endif

#ifndef TUNSETCARRIER
#define TUNSETCARRIER _IOW('T', 226, int)
#endif

#define FRAME_BUF   (2048 + 64)
#define READ_BUDGET 64          /* frames per readiness event */

struct nc_tap {
    tunnel_t tun;
    nc_tap_cfg_t cfg;
    char req_name[IFNAMSIZ];
    nc_tap_event_cb cb;
    void *user;

    int fd;
    char ifname[IFNAMSIZ];
    int link;
    int carrier_ok;             /* TUNSETCARRIER supported */
    nc_tap_stats_t st;
    uint8_t buf[FRAME_BUF];
};

static void emit(nc_tap_t *tap, int ev, int tev, int err)
{
    if (tap->cb)
        tap->cb(tap->user, ev, tev, err);
}

static int ifreq_ioctl(unsigned long req, struct ifreq *ifr)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0), rc;

    if (s < 0)
        return -1;
    rc = ioctl(s, req, ifr);
    if (rc < 0) {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }
    close(s);
    return 0;
}

static void disable_ipv6(const char *ifname)
{
    char path[96];
    int fd;

    /* IPv4 only; best effort (keeps router solicitations off the bridge) */
    snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/disable_ipv6", ifname);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)!write(fd, "1", 1);
        close(fd);
    }
}

static void set_carrier(nc_tap_t *tap, int on)
{
    if (tap->fd < 0 || !tap->carrier_ok)
        return;
    if (ioctl(tap->fd, TUNSETCARRIER, &on) < 0)
        tap->carrier_ok = 0;    /* old kernel: interface stays up */
}

static int create_iface(nc_tap_t *tap, int link)
{
    struct ifreq ifr;
    int fd, e;

    fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = (short)((link == TUNNEL_LINK_L2 ? IFF_TAP : IFF_TUN) | IFF_NO_PI);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", tap->req_name);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0)
        goto fail;
    snprintf(tap->ifname, sizeof(tap->ifname), "%s", ifr.ifr_name);

    if (link == TUNNEL_LINK_L2) {
        memset(&ifr.ifr_ifru, 0, sizeof(ifr.ifr_ifru));
        ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
        memcpy(ifr.ifr_hwaddr.sa_data, tap->cfg.mac, 6);
        if (ifreq_ioctl(SIOCSIFHWADDR, &ifr) < 0)
            goto fail;
    }
    memset(&ifr.ifr_ifru, 0, sizeof(ifr.ifr_ifru));
    ifr.ifr_mtu = tap->cfg.mtu;
    if (ifreq_ioctl(SIOCSIFMTU, &ifr) < 0)
        goto fail;
    disable_ipv6(tap->ifname);
    memset(&ifr.ifr_ifru, 0, sizeof(ifr.ifr_ifru));
    if (ifreq_ioctl(SIOCGIFFLAGS, &ifr) < 0)
        goto fail;
    ifr.ifr_flags |= IFF_UP;
    if (ifreq_ioctl(SIOCSIFFLAGS, &ifr) < 0)
        goto fail;

    tap->fd = fd;
    tap->link = link;
    tap->carrier_ok = 1;
    set_carrier(tap, 1);
    return 0;
fail:
    e = errno;
    close(fd);
    tap->ifname[0] = '\0';
    return -e;
}

/* ---- tunnel callbacks ---- */

static void on_rx(void *user, const uint8_t *pkt, size_t len)
{
    nc_tap_t *tap = user;

    if (tap->fd < 0)
        return;
    if (write(tap->fd, pkt, len) == (ssize_t)len)
        tap->st.frames_in++;
    else
        tap->st.dropped++;      /* EAGAIN: kernel queue full, like a lost frame */
}

static void on_tunnel_event(void *user, int ev, int err)
{
    nc_tap_t *tap = user;

    if (ev == TUNNEL_EV_UP) {
        int link = tap->tun.ops->link_type(tap->tun.ctx);

        if (tap->fd < 0) {
            int rc = create_iface(tap, link);
            emit(tap, NC_TAP_EV_TUNNEL, ev, err);
            if (rc < 0)
                emit(tap, NC_TAP_EV_ERROR, 0, -rc);
            else
                emit(tap, NC_TAP_EV_CREATED, 0, 0);
            return;
        }
        if (link != tap->link) {
            emit(tap, NC_TAP_EV_TUNNEL, ev, err);
            emit(tap, NC_TAP_EV_ERROR, 0, EPROTOTYPE);  /* session type changed */
            return;
        }
        set_carrier(tap, 1);
        emit(tap, NC_TAP_EV_TUNNEL, ev, err);
        emit(tap, NC_TAP_EV_RECONNECTED, 0, 0);
        return;
    }
    if (ev == TUNNEL_EV_DOWN || ev == TUNNEL_EV_FAILED)
        set_carrier(tap, 0);
    emit(tap, NC_TAP_EV_TUNNEL, ev, err);
}

/* ---- loop source ---- */

static int tap_get_fd(void *ctx)
{
    return ((nc_tap_t *)ctx)->fd;
}

static void tap_readable(void *ctx)
{
    nc_tap_t *tap = ctx;
    int i;

    for (i = 0; i < READ_BUDGET && tap->fd >= 0; i++) {
        ssize_t n = read(tap->fd, tap->buf, sizeof(tap->buf));

        if (n <= 0)
            return;             /* EAGAIN or error: wait for the next event */
        if (tap->tun.ops->send(tap->tun.ctx, tap->buf, (size_t)n) == 0)
            tap->st.frames_out++;
        else
            tap->st.dropped++;  /* tunnel down or output buffer full */
    }
}

nc_source_t nc_tap_source(nc_tap_t *tap)
{
    nc_source_t s;

    memset(&s, 0, sizeof(s));
    s.get_fd = tap_get_fd;
    s.on_readable = tap_readable;
    s.ctx = tap;
    return s;
}

/* ---- public ---- */

nc_tap_t *nc_tap_create(const tunnel_t *t, const nc_tap_cfg_t *cfg, nc_tap_event_cb cb, void *user)
{
    nc_tap_t *tap = calloc(1, sizeof(*tap));

    if (!tap)
        return NULL;
    tap->tun = *t;
    tap->cfg = *cfg;
    tap->cb = cb;
    tap->user = user;
    tap->fd = -1;
    snprintf(tap->req_name, sizeof(tap->req_name), "%s",
             cfg->ifname && *cfg->ifname ? cfg->ifname : "vtesse%d");
    tap->cfg.ifname = NULL;
    if (tap->cfg.mtu <= 0)
        tap->cfg.mtu = 1500;
    t->ops->set_rx_cb(t->ctx, on_rx, tap);
    t->ops->set_event_cb(t->ctx, on_tunnel_event, tap);
    return tap;
}

void nc_tap_destroy(nc_tap_t *tap)
{
    if (!tap)
        return;
    if (tap->fd >= 0)
        close(tap->fd);         /* a non-persistent interface disappears */
    free(tap);
}

const char *nc_tap_ifname(const nc_tap_t *tap)
{
    return tap->ifname;
}

int nc_tap_link(const nc_tap_t *tap)
{
    return tap->fd >= 0 ? tap->link : TUNNEL_LINK_NONE;
}

void nc_tap_get_stats(const nc_tap_t *tap, nc_tap_stats_t *st)
{
    *st = tap->st;
}
