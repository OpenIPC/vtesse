/*
 * UDP relay: lwIP UDP PCB on the tunnel side, a connected UDP socket on the
 * host side. Runs in the loop thread (lwIP input calls on_recv); the host
 * socket is non-blocking, so a slow reader drops datagrams instead of
 * stalling the tunnel.
 */
#include "netcore/udprelay.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lwip/pbuf.h"
#include "lwip/udp.h"

struct nc_udp_relay {
    struct udp_pcb *pcb;
    int fd;
    nc_udp_relay_stats_t st;
    uint8_t buf[65536];
};

static void on_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    nc_udp_relay_t *r = arg;
    u16_t len;

    (void)pcb;
    (void)addr;
    (void)port;
    if (!p)
        return;
    len = pbuf_copy_partial(p, r->buf, sizeof(r->buf) < 0xffff ? sizeof(r->buf) : 0xffff, 0);
    pbuf_free(p);
    if (send(r->fd, r->buf, len, MSG_DONTWAIT) == (ssize_t)len) {
        r->st.datagrams++;
        r->st.bytes += len;
    } else {
        r->st.dropped++;            /* buffer full, or nobody listens (ECONNREFUSED) */
    }
}

nc_udp_relay_t *nc_udp_relay_create(uint16_t port, const char *dst_addr, uint16_t dst_port,
                                    char *err, size_t err_size)
{
    struct sockaddr_in dst;
    nc_udp_relay_t *r;
    int sndbuf = 1 << 20;

    if (!port) {
        snprintf(err, err_size, "udp relay: port 0");
        return NULL;
    }
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(dst_port ? dst_port : port);
    if (inet_pton(AF_INET, dst_addr ? dst_addr : "127.0.0.1", &dst.sin_addr) != 1) {
        snprintf(err, err_size, "udp relay: bad address '%s'", dst_addr);
        return NULL;
    }
    r = calloc(1, sizeof(*r));
    if (!r) {
        snprintf(err, err_size, "udp relay: out of memory");
        return NULL;
    }
    r->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (r->fd < 0 || connect(r->fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        snprintf(err, err_size, "udp relay: local socket: %s", strerror(errno));
        goto fail;
    }
    fcntl(r->fd, F_SETFL, fcntl(r->fd, F_GETFL) | O_NONBLOCK);
    (void)setsockopt(r->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    r->pcb = udp_new();
    if (!r->pcb) {
        snprintf(err, err_size, "udp relay: no lwIP UDP PCB left (max %d relays)", NC_UDP_RELAY_MAX);
        goto fail;
    }
    if (udp_bind(r->pcb, IP4_ADDR_ANY, port) != ERR_OK) {
        snprintf(err, err_size, "udp relay: port %u in use", (unsigned)port);
        goto fail;
    }
    udp_recv(r->pcb, on_recv, r);
    return r;

fail:
    nc_udp_relay_destroy(r);
    return NULL;
}

void nc_udp_relay_destroy(nc_udp_relay_t *r)
{
    if (!r)
        return;
    if (r->pcb)
        udp_remove(r->pcb);
    if (r->fd >= 0)
        close(r->fd);
    free(r);
}

void nc_udp_relay_get_stats(const nc_udp_relay_t *r, nc_udp_relay_stats_t *st)
{
    *st = r->st;
}

static int parse_port(const char *s, char **end, uint16_t *out)
{
    unsigned long v;

    if (*s < '0' || *s > '9')
        return -1;
    v = strtoul(s, end, 10);
    if (!v || v > 65535)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

int nc_udp_relay_parse(const char *s, uint16_t *port, uint16_t *local_port)
{
    char *end;

    if (parse_port(s, &end, port) < 0)
        return -1;
    if (!*end) {
        *local_port = *port;
        return 0;
    }
    if (*end != ':' || parse_port(end + 1, &end, local_port) < 0 || *end)
        return -1;
    return 0;
}
