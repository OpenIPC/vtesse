#include "netcore/proxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lwip/tcp.h"
#include "netcore/route.h"
#include "netcore/stack.h"

#ifndef NC_WITH_SOCKS
#define NC_WITH_SOCKS 1     /* make SOCKS=0 leaves out the SOCKS5 server */
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define DEF_MAX_CONNS       24
#define DEF_CONNECT_MS      15000
#define HS_MAX              600     /* SOCKS handshake + early data */
#define TX_MAX              8192    /* local -> lwIP staging */
#define READ_BUDGET         65536   /* per dispatch and connection */
#define LISTEN_MAX          (1 + NC_PROXY_MAX_FORWARDS)

/* SOCKS5 reply codes (RFC 1928) */
#define REP_OK              0x00
#define REP_FAILURE         0x01
#define REP_NET_UNREACH     0x03
#define REP_HOST_UNREACH    0x04
#define REP_REFUSED         0x05
#define REP_CMD_UNSUPP      0x07
#define REP_ATYP_UNSUPP     0x08

enum conn_state {
    C_GREETING,
    C_AUTH,
    C_REQUEST,
    C_CONNECTING,
    C_RELAY
};

typedef struct conn {
    nc_proxy_t *px;
    int fd;
    struct tcp_pcb *pcb;
    int state;
    int socks;                  /* SOCKS5 client: a reply is owed */
    int replied;

    uint8_t hs[HS_MAX];
    size_t hs_len;

    uint8_t tx[TX_MAX];         /* local -> remote, not yet accepted by lwIP */
    size_t tx_off, tx_len;
    struct pbuf *rx;            /* remote -> local, not yet written */

    int local_eof, remote_eof, fin_sent, shut_wr;
    int dead, abort;

    uint64_t deadline;
    uint32_t dst;
    uint16_t dport;
} conn_t;

typedef struct slot {
    int listener;               /* -1: connection, 0: SOCKS, 1..: forward index + 1 */
    conn_t *conn;
} slot_t;

struct nc_proxy {
    nc_proxy_cfg_t cfg;
    char listen_addr[64];
    char user[256], pass[256];
    int auth;

    int lfd[LISTEN_MAX];        /* [0] SOCKS, [1 + i] forward i; -1 if unused */

    conn_t **conns;
    int nconns;

    slot_t slots[NC_LOOP_MAX_FDS];
    nc_proxy_stats_t st;
    nc_proxy_log_cb log;
    void *log_user;
};

static void logf_(nc_proxy_t *px, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    if (!px->log)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    px->log(px->log_user, buf);
}

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static void dst_str(const conn_t *c, char *out, size_t size)
{
    char ip[16];

    nc_ip4_format(c->dst, ip, sizeof(ip));
    snprintf(out, size, "%s:%u", ip, (unsigned)c->dport);
}

/* ---- SOCKS replies ---- */

static void socks_reply(conn_t *c, uint8_t rep)
{
    uint8_t r[10] = { 5, 0, 0, 1, 0, 0, 0, 0, 0, 0 };

    if (!c->socks || c->replied)
        return;
    c->replied = 1;
    r[1] = rep;
    if (rep == REP_OK && c->pcb) {
        uint32_t a = ip4_addr_get_u32(ip_2_ip4(&c->pcb->local_ip));    /* network order */
        memcpy(r + 4, &a, 4);
        r[8] = (uint8_t)(c->pcb->local_port >> 8);
        r[9] = (uint8_t)c->pcb->local_port;
    }
    if (send(c->fd, r, sizeof(r), MSG_NOSIGNAL) != (ssize_t)sizeof(r) && rep == REP_OK)
        c->dead = c->abort = 1;
}

static void fail_conn(conn_t *c, uint8_t rep, const char *why)
{
    char d[24];

    dst_str(c, d, sizeof(d));
    logf_(c->px, "%s %s: %s", c->socks ? "socks" : "forward", d, why);
    socks_reply(c, rep);
    c->px->st.failed++;
    c->dead = 1;
    c->abort = 1;
}

/* ---- data pumps ---- */

static void pump_down(conn_t *c)
{
    while (c->rx) {
        struct pbuf *q = c->rx;
        ssize_t w = send(c->fd, q->payload, q->len, MSG_NOSIGNAL);

        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            c->dead = c->abort = 1;
            return;
        }
        c->px->st.bytes_down += (unsigned long long)w;
        c->rx = pbuf_free_header(c->rx, (u16_t)w);
        if (c->pcb)
            tcp_recved(c->pcb, (u16_t)w);
    }
    if (c->remote_eof && !c->shut_wr) {
        shutdown(c->fd, SHUT_WR);
        c->shut_wr = 1;
    }
}

static void flush_tx(conn_t *c)
{
    while (c->tx_len && c->pcb) {
        size_t n = tcp_sndbuf(c->pcb);

        if (n == 0)
            break;
        if (n > c->tx_len)
            n = c->tx_len;
        if (tcp_write(c->pcb, c->tx + c->tx_off, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK)
            break;                      /* out of segments: retry later */
        c->tx_off += n;
        c->tx_len -= n;
        c->px->st.bytes_up += n;
    }
    if (!c->tx_len)
        c->tx_off = 0;
    if (c->pcb && c->local_eof && !c->tx_len && !c->fin_sent) {
        tcp_shutdown(c->pcb, 0, 1);
        c->fin_sent = 1;
    }
    if (c->pcb)
        tcp_output(c->pcb);
}

static int can_read_local(const conn_t *c)
{
    return c->state == C_RELAY && c->pcb && !c->local_eof && !c->tx_len &&
           tcp_sndbuf(c->pcb) > 0 && tcp_sndqueuelen(c->pcb) < TCP_SND_QUEUELEN - 4;
}

static void pump_up(conn_t *c)
{
    size_t budget = READ_BUDGET;

    flush_tx(c);
    while (budget && can_read_local(c)) {
        size_t want = tcp_sndbuf(c->pcb);
        ssize_t r;

        if (want > TX_MAX)
            want = TX_MAX;
        r = recv(c->fd, c->tx, want, 0);
        if (r == 0) {
            c->local_eof = 1;
        } else if (r < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                c->dead = c->abort = 1;
            break;
        } else {
            c->tx_off = 0;
            c->tx_len = (size_t)r;
            budget -= (size_t)r < budget ? (size_t)r : budget;
        }
        flush_tx(c);
    }
}

/* ---- lwIP callbacks (never free the connection here) ---- */

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    conn_t *c = arg;

    (void)err;
    if (!c) {
        if (p) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }
        return ERR_OK;
    }
    if (!p) {
        c->remote_eof = 1;
    } else if (c->dead) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    } else if (c->rx) {
        pbuf_cat(c->rx, p);
    } else {
        c->rx = p;
    }
    pump_down(c);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    conn_t *c = arg;

    if (!c)
        return;
    c->pcb = NULL;              /* already freed by lwIP */
    if (c->state == C_CONNECTING)
        fail_conn(c, err == ERR_RST ? REP_REFUSED : REP_HOST_UNREACH,
                  err == ERR_RST ? "connection refused" :
                  err == ERR_ABRT ? "connect timeout" : "host unreachable");
    else
        c->dead = c->abort = 1;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    conn_t *c = arg;

    (void)pcb;
    (void)err;
    if (!c)
        return ERR_OK;
    c->state = C_RELAY;
    c->deadline = 0;
    c->px->st.connected++;
    socks_reply(c, REP_OK);
    flush_tx(c);                /* data the client sent before the reply */
    return ERR_OK;
}

static void start_connect(conn_t *c)
{
    ip4_addr_t a;
    struct tcp_pcb *pcb;

    if (!nc_stack_route_ok(c->dst)) {
        fail_conn(c, REP_NET_UNREACH, "no route via tunnel (or no address yet)");
        return;
    }
    pcb = tcp_new();
    if (!pcb) {
        fail_conn(c, REP_FAILURE, "out of TCP PCBs");
        return;
    }
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    tcp_nagle_disable(pcb);
    ip4_addr_set_u32(&a, lwip_htonl(c->dst));
    c->pcb = pcb;
    c->state = C_CONNECTING;
    c->deadline = nc_now_ms() + (uint64_t)c->px->cfg.connect_timeout_ms;
    if (tcp_connect(pcb, &a, c->dport, on_connected) != ERR_OK) {
        c->pcb = NULL;
        tcp_arg(pcb, NULL);
        tcp_abort(pcb);
        fail_conn(c, REP_FAILURE, "connect failed");
    }
}

#if NC_WITH_SOCKS
/* ---- SOCKS5 handshake ---- */

static void consume(conn_t *c, size_t n)
{
    memmove(c->hs, c->hs + n, c->hs_len - n);
    c->hs_len -= n;
}

static void socks_send(conn_t *c, const uint8_t *p, size_t n)
{
    if (send(c->fd, p, n, MSG_NOSIGNAL) != (ssize_t)n)
        c->dead = c->abort = 1;
}

static int parse_ip4_text(const uint8_t *s, size_t len, uint32_t *out)
{
    char buf[16];
    unsigned a, b, cc, d;
    char tail;

    if (len == 0 || len >= sizeof(buf))
        return -1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    if (sscanf(buf, "%u.%u.%u.%u%c", &a, &b, &cc, &d, &tail) != 4 || a > 255 || b > 255 || cc > 255 || d > 255)
        return -1;
    *out = a << 24 | b << 16 | cc << 8 | d;
    return 0;
}

static void socks_process(conn_t *c)
{
    nc_proxy_t *px = c->px;

    while (!c->dead) {
        uint8_t *h = c->hs;
        size_t need;

        if (c->state == C_GREETING) {
            uint8_t rep[2] = { 5, 0xff };
            size_t i;

            if (c->hs_len < 2)
                return;
            if (h[0] != 5) {
                c->dead = c->abort = 1;
                return;
            }
            need = 2u + h[1];
            if (c->hs_len < need)
                return;
            for (i = 2; i < need; i++)
                if (h[i] == (px->auth ? 0x02 : 0x00))
                    rep[1] = px->auth ? 0x02 : 0x00;
            socks_send(c, rep, 2);
            consume(c, need);
            if (rep[1] == 0xff) {
                logf_(px, "socks: no acceptable authentication method");
                c->dead = 1;
                return;
            }
            c->state = px->auth ? C_AUTH : C_REQUEST;
        } else if (c->state == C_AUTH) {
            uint8_t rep[2] = { 1, 1 };
            size_t ul, pl;

            if (c->hs_len < 2)
                return;
            ul = h[1];
            if (c->hs_len < 3 + ul)
                return;
            pl = h[2 + ul];
            need = 3 + ul + pl;
            if (c->hs_len < need)
                return;
            if (h[0] == 1 && ul == strlen(px->user) && pl == strlen(px->pass) &&
                memcmp(h + 2, px->user, ul) == 0 && memcmp(h + 3 + ul, px->pass, pl) == 0)
                rep[1] = 0;
            socks_send(c, rep, 2);
            consume(c, need);
            if (rep[1]) {
                logf_(px, "socks: authentication failed");
                c->dead = 1;
                return;
            }
            c->state = C_REQUEST;
        } else if (c->state == C_REQUEST) {
            uint8_t atyp;

            if (c->hs_len < 5)
                return;
            atyp = h[3];
            need = atyp == 1 ? 10 : atyp == 3 ? 7u + h[4] : atyp == 4 ? 22 : 0;
            if (!need || h[0] != 5) {
                c->socks = 1;
                fail_conn(c, REP_ATYP_UNSUPP, "unsupported address type");
                return;
            }
            if (c->hs_len < need)
                return;
            c->dport = (uint16_t)(h[need - 2] << 8 | h[need - 1]);
            if (h[1] != 1) {
                fail_conn(c, REP_CMD_UNSUPP, "only CONNECT is supported");
                return;
            }
            if (atyp == 1) {
                c->dst = (uint32_t)h[4] << 24 | (uint32_t)h[5] << 16 | (uint32_t)h[6] << 8 | h[7];
            } else if (atyp == 3 && parse_ip4_text(h + 5, h[4], &c->dst) == 0) {
                /* domain field holding an IPv4 literal (socks5h clients) */
            } else {
                fail_conn(c, REP_ATYP_UNSUPP, atyp == 4 ? "IPv6 is not supported" :
                                                          "host names are not supported (no DNS)");
                return;
            }
            consume(c, need);
            /* anything after the request is early application data */
            memcpy(c->tx, c->hs, c->hs_len);
            c->tx_off = 0;
            c->tx_len = c->hs_len;
            c->hs_len = 0;
            start_connect(c);
            return;
        } else {
            return;
        }
    }
}

static void read_handshake(conn_t *c)
{
    ssize_t r = recv(c->fd, c->hs + c->hs_len, sizeof(c->hs) - c->hs_len, 0);

    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        c->dead = 1;
        return;
    }
    if (r < 0)
        return;
    c->hs_len += (size_t)r;
    socks_process(c);
    if (!c->dead && c->state < C_CONNECTING && c->hs_len == sizeof(c->hs))
        c->dead = c->abort = 1;     /* oversized handshake */
}
#else
/* built without SOCKS5 (make SOCKS=0): no SOCKS listener is ever opened */
static void read_handshake(conn_t *c)
{
    c->dead = c->abort = 1;
}
#endif

/* ---- connection table ---- */

static void destroy_conn(nc_proxy_t *px, int idx)
{
    conn_t *c = px->conns[idx];

    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_err(c->pcb, NULL);
        if (c->abort || tcp_close(c->pcb) != ERR_OK)
            tcp_abort(c->pcb);
    }
    if (c->rx)
        pbuf_free(c->rx);
    if (c->fd >= 0)
        close(c->fd);
    free(c);
    px->conns[idx] = px->conns[--px->nconns];
    px->st.active--;
}

static void service(nc_proxy_t *px)
{
    int i;

    for (i = 0; i < px->nconns; i++) {
        conn_t *c = px->conns[i];

        if (!c->dead && c->state == C_RELAY) {
            if (!c->pcb)
                c->dead = 1;
            else if (c->tx_len || (c->local_eof && !c->fin_sent))
                flush_tx(c);
            if (c->rx)
                pump_down(c);
            if (c->local_eof && c->remote_eof && !c->rx && !c->tx_len && c->fin_sent)
                c->dead = 1;        /* both directions finished */
            else if (c->remote_eof && c->shut_wr && !c->pcb)
                c->dead = 1;
        }
        if (c->dead) {
            destroy_conn(px, i);
            i--;
        }
    }
}

static void accept_on(nc_proxy_t *px, int which)
{
    for (;;) {
        int fd = accept(px->lfd[which], NULL, NULL), one = 1;
        conn_t *c;

        if (fd < 0)
            return;
        set_nonblock(fd);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (px->nconns >= px->cfg.max_conns || !(c = calloc(1, sizeof(*c)))) {
            close(fd);
            px->st.rejected++;
            logf_(px, "connection limit reached (%d)", px->cfg.max_conns);
            continue;
        }
        c->px = px;
        c->fd = fd;
        px->conns[px->nconns++] = c;
        px->st.accepted++;
        px->st.active++;
        if (which == 0) {
            c->socks = 1;
            c->state = C_GREETING;
        } else {
            const nc_forward_t *f = &px->cfg.forwards[which - 1];
            c->dst = f->ip;
            c->dport = f->port;
            start_connect(c);
        }
    }
}

/* ---- loop source ---- */

static int px_fill(void *ctx, struct pollfd *pfd, int max)
{
    nc_proxy_t *px = ctx;
    int n = 0, i;

    service(px);
    for (i = 0; i < LISTEN_MAX && n < max; i++) {
        if (px->lfd[i] < 0)
            continue;
        pfd[n].fd = px->lfd[i];
        pfd[n].events = POLLIN;
        px->slots[n].listener = i;
        px->slots[n].conn = NULL;
        n++;
    }
    for (i = 0; i < px->nconns && n < max; i++) {
        conn_t *c = px->conns[i];
        short ev = 0;

        if (c->state < C_CONNECTING)
            ev = POLLIN;
        else if (c->state == C_RELAY) {
            if (can_read_local(c))
                ev |= POLLIN;
            if (c->rx)
                ev |= POLLOUT;
        }
        pfd[n].fd = c->fd;
        pfd[n].events = ev;
        px->slots[n].listener = -1;
        px->slots[n].conn = c;
        n++;
    }
    return n;
}

static void px_dispatch(void *ctx, const struct pollfd *pfd, int n)
{
    nc_proxy_t *px = ctx;
    int i;

    for (i = 0; i < n; i++) {
        short re = pfd[i].revents;
        conn_t *c = px->slots[i].conn;

        if (!re)
            continue;
        if (px->slots[i].listener >= 0) {
            accept_on(px, px->slots[i].listener);
            continue;
        }
        if (c->dead)
            continue;
        if (re & POLLERR) {
            c->dead = c->abort = 1;
            continue;
        }
        if (re & POLLOUT)
            pump_down(c);
        if (re & (POLLIN | POLLHUP)) {
            if (c->state < C_CONNECTING)
                read_handshake(c);
            else if (c->state == C_RELAY)
                pump_up(c);
            else if (re & POLLHUP)
                c->dead = c->abort = 1;     /* client gave up while connecting */
        }
    }
    service(px);
}

static int px_next_timeout(void *ctx)
{
    nc_proxy_t *px = ctx;
    uint64_t now = nc_now_ms(), next = 0;
    int i;

    for (i = 0; i < px->nconns; i++) {
        const conn_t *c = px->conns[i];
        if (c->state == C_CONNECTING && c->deadline && (!next || c->deadline < next))
            next = c->deadline;
    }
    if (!next)
        return -1;
    return next <= now ? 0 : (int)(next - now);
}

static void px_timer(void *ctx)
{
    nc_proxy_t *px = ctx;
    uint64_t now = nc_now_ms();
    int i;

    for (i = 0; i < px->nconns; i++) {
        conn_t *c = px->conns[i];
        if (c->state == C_CONNECTING && c->deadline && now >= c->deadline && c->pcb) {
            struct tcp_pcb *pcb = c->pcb;
            c->deadline = 0;
            tcp_abort(pcb);         /* calls on_err(ERR_ABRT) -> "connect timeout" */
        }
    }
    service(px);
}

nc_source_t nc_proxy_source(nc_proxy_t *px)
{
    nc_source_t s;

    memset(&s, 0, sizeof(s));
    s.poll_fill = px_fill;
    s.poll_dispatch = px_dispatch;
    s.next_timeout_ms = px_next_timeout;
    s.on_timer = px_timer;
    s.ctx = px;
    return s;
}

/* ---- create / destroy ---- */

static int open_listener(nc_proxy_t *px, uint16_t port)
{
    struct sockaddr_in a;
    int fd, one = 1;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, px->listen_addr, &a.sin_addr) != 1) {
        logf_(px, "invalid listen address %s", px->listen_addr);
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 16) < 0) {
        logf_(px, "cannot listen on %s:%u: %s", px->listen_addr, (unsigned)port, strerror(errno));
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

nc_proxy_t *nc_proxy_create(const nc_proxy_cfg_t *cfg, nc_proxy_log_cb log, void *user)
{
    nc_proxy_t *px = calloc(1, sizeof(*px));
    int i;

    if (!px)
        return NULL;
    px->cfg = *cfg;
    px->log = log;
    px->log_user = user;
    for (i = 0; i < LISTEN_MAX; i++)
        px->lfd[i] = -1;
    snprintf(px->listen_addr, sizeof(px->listen_addr), "%s",
             cfg->listen_addr && *cfg->listen_addr ? cfg->listen_addr : "127.0.0.1");
    if (cfg->socks_user && *cfg->socks_user) {
        if (strlen(cfg->socks_user) > 255 || !cfg->socks_password || strlen(cfg->socks_password) > 255) {
            logf_(px, "invalid SOCKS credentials");
            goto fail;
        }
        snprintf(px->user, sizeof(px->user), "%s", cfg->socks_user);
        snprintf(px->pass, sizeof(px->pass), "%s", cfg->socks_password);
        px->auth = 1;
    }
    px->cfg.listen_addr = px->cfg.socks_user = px->cfg.socks_password = NULL;
    if (px->cfg.max_conns <= 0)
        px->cfg.max_conns = DEF_MAX_CONNS;
    if (px->cfg.connect_timeout_ms <= 0)
        px->cfg.connect_timeout_ms = DEF_CONNECT_MS;
    if (px->cfg.forward_count < 0 || px->cfg.forward_count > NC_PROXY_MAX_FORWARDS)
        goto fail;
    px->conns = calloc((size_t)px->cfg.max_conns, sizeof(*px->conns));
    if (!px->conns)
        goto fail;
#if !NC_WITH_SOCKS
    if (cfg->socks_port) {
        logf_(px, "built without SOCKS5 (make SOCKS=0)");
        goto fail;
    }
#endif
    if (cfg->socks_port && (px->lfd[0] = open_listener(px, cfg->socks_port)) < 0)
        goto fail;
    for (i = 0; i < px->cfg.forward_count; i++)
        if ((px->lfd[1 + i] = open_listener(px, px->cfg.forwards[i].local_port)) < 0)
            goto fail;
    return px;
fail:
    nc_proxy_destroy(px);
    return NULL;
}

void nc_proxy_destroy(nc_proxy_t *px)
{
    int i;

    if (!px)
        return;
    while (px->nconns > 0) {
        px->conns[0]->abort = 1;
        destroy_conn(px, 0);
    }
    for (i = 0; i < LISTEN_MAX; i++)
        if (px->lfd[i] >= 0)
            close(px->lfd[i]);
    memset(px->pass, 0, sizeof(px->pass));
    free(px->conns);
    free(px);
}

void nc_proxy_get_stats(const nc_proxy_t *px, nc_proxy_stats_t *st)
{
    *st = px->st;
}
