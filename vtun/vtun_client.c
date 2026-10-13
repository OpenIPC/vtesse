#include "vtesse/vtun_client.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "netcore/loop.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define DEF_HANDSHAKE_MS    15000
#define MIN_CHAL_STEP_MS    10000   /* multi killold: the server may wait ~5 s */
#define DEF_KA_INTERVAL_MS  10000
#define DEF_KA_COUNT        5
#define DEF_DATA_DELAY_MS   500     /* the server brings the tap up asynchronously */
#define DEF_RECONNECT_MIN   1000
#define DEF_RECONNECT_MAX   30000
#define DEF_OUTBUF          65536
#define STABLE_SESSION_MS   30000   /* reset backoff only after a stable session */
#define INBUF_SIZE          (2 + VT_FRAME_MAX_RX + 2048)
#define LINK_MTU            1500

enum vt_state {
    ST_STOPPED = 0,
    ST_WAIT_RECONNECT,
    ST_CONNECTING,
    ST_WAIT_GREETING,
    ST_WAIT_CHAL,
    ST_WAIT_FLAGS,
    ST_ESTABLISHED
};

struct vt {
    char *host, *session, *password;
    uint16_t port;
    vt_cfg_t cfg;

    int state;
    int fd;

    uint8_t inbuf[INBUF_SIZE];
    size_t in_len;
    size_t skip;                /* bytes of an oversized frame still to discard */

    uint8_t *out;
    size_t out_cap, out_off, out_len;

    uint64_t deadline;          /* handshake step deadline */
    uint64_t reconnect_at;
    uint64_t up_at;
    uint64_t data_ok_at;
    uint64_t last_rx;
    uint64_t last_ka_tx;
    int backoff_ms;

    char greeting[VT_MESG_SIZE];
    vt_flags_t flags;

    tunnel_rx_cb rx_cb;
    void *rx_user;
    tunnel_event_cb ev_cb;
    void *ev_user;
};

static void emit(vt_t *vt, int event, int err)
{
    if (vt->ev_cb)
        vt->ev_cb(vt->ev_user, event, err);
}

static int fatal_error(int err)
{
    switch (err) {
    case TUNNEL_ERR_CONFIG:
    case TUNNEL_ERR_AUTH:
    case TUNNEL_ERR_PROTOCOL:
    case TUNNEL_ERR_UNSUPPORTED_TYPE:
    case TUNNEL_ERR_UNSUPPORTED_TRANSPORT:
    case TUNNEL_ERR_COMPRESSION:
    case TUNNEL_ERR_ENCRYPTION:
        return 1;
    default:
        return 0;
    }
}

static void close_socket(vt_t *vt)
{
    if (vt->fd >= 0)
        close(vt->fd);
    vt->fd = -1;
    vt->in_len = vt->skip = 0;
    vt->out_off = vt->out_len = 0;
}

/* Tear down the connection; retry later unless the error is final. */
static void fail(vt_t *vt, int err)
{
    int was_up = vt->state == ST_ESTABLISHED;
    uint64_t now = nc_now_ms();

    close_socket(vt);
    if (fatal_error(err) || vt->cfg.no_reconnect) {
        vt->state = ST_STOPPED;
        emit(vt, TUNNEL_EV_FAILED, err);
        return;
    }
    if (was_up && now - vt->up_at >= STABLE_SESSION_MS)
        vt->backoff_ms = vt->cfg.reconnect_min_ms;
    vt->state = ST_WAIT_RECONNECT;
    vt->reconnect_at = now + (uint64_t)vt->backoff_ms;
    vt->backoff_ms *= 2;
    if (vt->backoff_ms > vt->cfg.reconnect_max_ms)
        vt->backoff_ms = vt->cfg.reconnect_max_ms;
    emit(vt, TUNNEL_EV_DOWN, err);
}

/* ---- output buffer ---- */

static void flush(vt_t *vt)
{
    while (vt->out_len > 0) {
        ssize_t n = send(vt->fd, vt->out + vt->out_off, vt->out_len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            fail(vt, TUNNEL_ERR_IO);
            return;
        }
        vt->out_off += (size_t)n;
        vt->out_len -= (size_t)n;
    }
    vt->out_off = 0;
}

/* Append a+b atomically (both or nothing). */
static int queue2(vt_t *vt, const uint8_t *a, size_t al, const uint8_t *b, size_t bl)
{
    if (vt->out_len + al + bl > vt->out_cap)
        return -1;
    if (vt->out_off + vt->out_len + al + bl > vt->out_cap) {
        memmove(vt->out, vt->out + vt->out_off, vt->out_len);
        vt->out_off = 0;
    }
    memcpy(vt->out + vt->out_off + vt->out_len, a, al);
    vt->out_len += al;
    if (bl) {
        memcpy(vt->out + vt->out_off + vt->out_len, b, bl);
        vt->out_len += bl;
    }
    return 0;
}

static int queue_mesg(vt_t *vt, const char *prefix, const char *arg)
{
    uint8_t m[VT_MESG_SIZE];

    if (vt_mesg_build(m, prefix, arg) < 0 || queue2(vt, m, sizeof(m), NULL, 0) < 0)
        return -1;
    return 0;
}

static void queue_flag(vt_t *vt, uint16_t flag)
{
    uint8_t h[2];

    vt_hdr_put(h, flag);
    queue2(vt, h, 2, NULL, 0);  /* a full buffer means the link is stuck anyway */
}

/* ---- connection ---- */

static void start_connect(vt_t *vt)
{
    struct addrinfo hints, *res = NULL, *ai;
    char port[8];
    int fd = -1, one = 1;

    vt->state = ST_CONNECTING;
    emit(vt, TUNNEL_EV_CONNECTING, TUNNEL_OK);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* VTun 3.0.x is IPv4 only */
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%u", (unsigned)vt->port);
    if (getaddrinfo(vt->host, port, &hints, &res) != 0 || !res) {
        fail(vt, TUNNEL_ERR_RESOLVE);
        return;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 || errno == EINPROGRESS)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fail(vt, TUNNEL_ERR_CONNECT);
        return;
    }
    vt->fd = fd;
    vt->deadline = nc_now_ms() + (uint64_t)vt->cfg.handshake_timeout_ms;
}

static void establish(vt_t *vt)
{
    uint64_t now = nc_now_ms();

    vt->state = ST_ESTABLISHED;
    vt->up_at = now;
    vt->data_ok_at = now + (uint64_t)vt->cfg.data_delay_ms;
    vt->last_rx = now;
    vt->last_ka_tx = now;
    queue_flag(vt, VT_ECHO_REQ);    /* like the stock client */
    emit(vt, TUNNEL_EV_UP, TUNNEL_OK);
}

/* One 50-byte control message during the handshake. */
static void handle_mesg(vt_t *vt, char *m)
{
    uint8_t chal[VT_CHAL_SIZE];
    char text[VT_CHAL_TEXT_SIZE];
    uint64_t step;
    int err;

    m[VT_MESG_SIZE - 1] = '\0';
    switch (vt->state) {
    case ST_WAIT_GREETING:
        if (strncmp(m, "VTUN", 4) != 0) {
            fail(vt, TUNNEL_ERR_PROTOCOL);
            return;
        }
        snprintf(vt->greeting, sizeof(vt->greeting), "%s", m);
        vt->greeting[strcspn(vt->greeting, "\r\n")] = '\0';
        queue_mesg(vt, "HOST: ", vt->session);
        vt->state = ST_WAIT_CHAL;
        vt->deadline = nc_now_ms() + (uint64_t)vt->cfg.handshake_timeout_ms;
        break;

    case ST_WAIT_CHAL:
        if (strncmp(m, "OK", 2) != 0 || vt_chal_decode(m, chal) != 0) {
            fail(vt, strncmp(m, "ERR", 3) == 0 ? TUNNEL_ERR_AUTH : TUNNEL_ERR_PROTOCOL);
            return;
        }
        vt_chal_xor(chal, vt->password);
        vt_chal_encode(chal, text);
        queue_mesg(vt, "CHAL: ", text);
        vt->state = ST_WAIT_FLAGS;
        step = (uint64_t)vt->cfg.handshake_timeout_ms;
        if (step < MIN_CHAL_STEP_MS)
            step = MIN_CHAL_STEP_MS;
        vt->deadline = nc_now_ms() + step;
        break;

    case ST_WAIT_FLAGS:
        if (strncmp(m, "OK", 2) != 0) {
            fail(vt, strncmp(m, "ERR", 3) == 0 ? TUNNEL_ERR_AUTH : TUNNEL_ERR_PROTOCOL);
            return;
        }
        if (vt_flags_parse(m, &vt->flags) != 0) {
            fail(vt, TUNNEL_ERR_PROTOCOL);
            return;
        }
        err = vt_flags_check(&vt->flags);
        if (err != TUNNEL_OK) {
            fail(vt, err);
            return;
        }
        establish(vt);
        break;

    default:
        break;
    }
}

/* Frames after OK FLAGS. Returns bytes consumed. */
static size_t handle_frames(vt_t *vt, const uint8_t *p, size_t len)
{
    size_t used = 0;

    while (vt->state == ST_ESTABLISHED) {
        uint16_t hdr, flen, fl;

        if (vt->skip) {
            size_t n = len - used < vt->skip ? len - used : vt->skip;
            used += n;
            vt->skip -= n;
            if (vt->skip)
                break;
            continue;
        }
        if (len - used < 2)
            break;
        hdr = vt_hdr_get(p + used);
        fl = hdr & VT_FLAGS_MASK;
        flen = hdr & VT_FSIZE_MASK;
        if (fl) {
            used += 2;
            if (fl & VT_CONN_CLOSE) {
                fail(vt, TUNNEL_ERR_PEER_CLOSED);
                break;
            }
            if (fl & VT_ECHO_REQ)
                queue_flag(vt, VT_ECHO_REP);
            continue;   /* ECHO_REP and unknown flags carry no payload */
        }
        if (flen > VT_FRAME_MAX_RX) {
            used += 2;
            vt->skip = flen;
            continue;
        }
        if (len - used < 2u + flen)
            break;
        if (flen && vt->rx_cb)
            vt->rx_cb(vt->rx_user, p + used + 2, flen);
        used += 2u + flen;
    }
    return used;
}

static void process_input(vt_t *vt)
{
    size_t used = 0;

    while (vt->state >= ST_WAIT_GREETING && vt->state <= ST_WAIT_FLAGS &&
           vt->in_len - used >= VT_MESG_SIZE) {
        char m[VT_MESG_SIZE];

        memcpy(m, vt->inbuf + used, VT_MESG_SIZE);
        used += VT_MESG_SIZE;
        handle_mesg(vt, m);
    }
    if (vt->state == ST_ESTABLISHED)
        used += handle_frames(vt, vt->inbuf + used, vt->in_len - used);
    if (vt->fd < 0)
        return;     /* failed while processing; buffers already reset */
    if (used) {
        memmove(vt->inbuf, vt->inbuf + used, vt->in_len - used);
        vt->in_len -= used;
    }
}

/* ---- tunnel_ops ---- */

static int op_get_fd(void *ctx)
{
    return ((vt_t *)ctx)->fd;
}

static int op_wants_write(void *ctx)
{
    vt_t *vt = ctx;

    return vt->fd >= 0 && (vt->state == ST_CONNECTING || vt->out_len > 0);
}

static void op_on_writable(void *ctx)
{
    vt_t *vt = ctx;

    if (vt->fd < 0)
        return;
    if (vt->state == ST_CONNECTING) {
        int err = 0;
        socklen_t l = sizeof(err);

        if (getsockopt(vt->fd, SOL_SOCKET, SO_ERROR, &err, &l) < 0 || err) {
            fail(vt, TUNNEL_ERR_CONNECT);
            return;
        }
        vt->state = ST_WAIT_GREETING;
        vt->deadline = nc_now_ms() + (uint64_t)vt->cfg.handshake_timeout_ms;
        return;
    }
    flush(vt);
}

static void op_on_readable(void *ctx)
{
    vt_t *vt = ctx;

    if (vt->fd < 0 || vt->state == ST_CONNECTING)
        return;
    for (;;) {
        ssize_t n;

        if (vt->in_len == sizeof(vt->inbuf)) {
            fail(vt, TUNNEL_ERR_PROTOCOL);  /* cannot happen with valid framing */
            return;
        }
        n = recv(vt->fd, vt->inbuf + vt->in_len, sizeof(vt->inbuf) - vt->in_len, 0);
        if (n == 0) {
            fail(vt, TUNNEL_ERR_PEER_CLOSED);   /* retriable, also during the handshake */
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            fail(vt, TUNNEL_ERR_IO);
            return;
        }
        vt->in_len += (size_t)n;
        vt->last_rx = nc_now_ms();
        process_input(vt);
        if (vt->fd < 0)
            return;
    }
    if (vt->out_len)
        flush(vt);
}

static int ms_until(uint64_t t, uint64_t now)
{
    if (t <= now)
        return 0;
    return t - now > 0x7fffffff ? 0x7fffffff : (int)(t - now);
}

static int op_next_timeout_ms(void *ctx)
{
    vt_t *vt = ctx;
    uint64_t now = nc_now_ms();
    int a, b;

    switch (vt->state) {
    case ST_WAIT_RECONNECT:
        return ms_until(vt->reconnect_at, now);
    case ST_CONNECTING:
    case ST_WAIT_GREETING:
    case ST_WAIT_CHAL:
    case ST_WAIT_FLAGS:
        return ms_until(vt->deadline, now);
    case ST_ESTABLISHED:
        a = ms_until(vt->last_ka_tx + (uint64_t)vt->cfg.ka_interval_ms, now);
        b = ms_until(vt->last_rx + (uint64_t)vt->cfg.ka_interval_ms * (uint64_t)vt->cfg.ka_count, now);
        return a < b ? a : b;
    default:
        return -1;
    }
}

static void op_on_timer(void *ctx)
{
    vt_t *vt = ctx;
    uint64_t now = nc_now_ms();

    switch (vt->state) {
    case ST_WAIT_RECONNECT:
        if (now >= vt->reconnect_at)
            start_connect(vt);
        break;
    case ST_CONNECTING:
    case ST_WAIT_GREETING:
    case ST_WAIT_CHAL:
    case ST_WAIT_FLAGS:
        if (now >= vt->deadline)
            fail(vt, vt->state == ST_CONNECTING ? TUNNEL_ERR_CONNECT : TUNNEL_ERR_TIMEOUT);
        break;
    case ST_ESTABLISHED:
        if (now - vt->last_rx >= (uint64_t)vt->cfg.ka_interval_ms * (uint64_t)vt->cfg.ka_count) {
            fail(vt, TUNNEL_ERR_KEEPALIVE);
            break;
        }
        if (now - vt->last_ka_tx >= (uint64_t)vt->cfg.ka_interval_ms) {
            vt->last_ka_tx = now;
            queue_flag(vt, VT_ECHO_REQ);
            flush(vt);
        }
        break;
    default:
        break;
    }
}

static void op_set_rx_cb(void *ctx, tunnel_rx_cb cb, void *user)
{
    vt_t *vt = ctx;

    vt->rx_cb = cb;
    vt->rx_user = user;
}

static void op_set_event_cb(void *ctx, tunnel_event_cb cb, void *user)
{
    vt_t *vt = ctx;

    vt->ev_cb = cb;
    vt->ev_user = user;
}

static int op_send(void *ctx, const uint8_t *pkt, size_t len)
{
    vt_t *vt = ctx;
    uint8_t h[2];

    if (vt->state != ST_ESTABLISHED || len == 0 || len > VT_FRAME_SIZE)
        return -1;
    if (nc_now_ms() < vt->data_ok_at)
        return -1;
    vt_hdr_put(h, (uint16_t)len);
    if (queue2(vt, h, 2, pkt, len) < 0)
        return -1;
    flush(vt);
    return 0;
}

static int op_link_type(void *ctx)
{
    vt_t *vt = ctx;

    if (vt->state != ST_ESTABLISHED)
        return TUNNEL_LINK_NONE;
    return vt->flags.type == 'e' ? TUNNEL_LINK_L2 : TUNNEL_LINK_L3;
}

static int op_mtu(void *ctx)
{
    (void)ctx;
    return LINK_MTU;
}

static void op_close(void *ctx)
{
    vt_t *vt = ctx;

    if (vt->state == ST_ESTABLISHED && vt->fd >= 0) {
        uint8_t h[2];

        flush(vt);
        vt_hdr_put(h, VT_CONN_CLOSE);
        if (vt->fd >= 0 && vt->out_len == 0)
            (void)!send(vt->fd, h, 2, MSG_NOSIGNAL);
    }
    close_socket(vt);
    vt->state = ST_STOPPED;
}

static const tunnel_ops_t vt_ops = {
    op_get_fd, op_wants_write, op_on_readable, op_on_writable,
    op_next_timeout_ms, op_on_timer, op_set_rx_cb, op_set_event_cb,
    op_send, op_link_type, op_mtu, op_close
};

/* ---- public ---- */

const tunnel_ops_t *vt_tunnel_ops(void)
{
    return &vt_ops;
}

tunnel_t vt_tunnel(vt_t *vt)
{
    tunnel_t t;

    t.ops = &vt_ops;
    t.ctx = vt;
    return t;
}

vt_t *vt_create(const vt_cfg_t *cfg, int *err)
{
    vt_t *vt;

    *err = TUNNEL_ERR_CONFIG;
    if (!cfg || !cfg->host || !*cfg->host || !cfg->port ||
        !vt_session_name_valid(cfg->session) || !cfg->password || !*cfg->password ||
        cfg->auth != VT_AUTH_XOR)
        return NULL;

    *err = TUNNEL_ERR_NOMEM;
    vt = calloc(1, sizeof(*vt));
    if (!vt)
        return NULL;
    vt->cfg = *cfg;
    vt->host = strdup(cfg->host);
    vt->session = strdup(cfg->session);
    vt->password = strdup(cfg->password);
    vt->port = cfg->port;
    vt->cfg.host = vt->cfg.session = vt->cfg.password = NULL;
    if (!vt->cfg.handshake_timeout_ms)
        vt->cfg.handshake_timeout_ms = DEF_HANDSHAKE_MS;
    if (!vt->cfg.ka_interval_ms)
        vt->cfg.ka_interval_ms = DEF_KA_INTERVAL_MS;
    if (!vt->cfg.ka_count)
        vt->cfg.ka_count = DEF_KA_COUNT;
    if (!vt->cfg.data_delay_ms)
        vt->cfg.data_delay_ms = DEF_DATA_DELAY_MS;
    if (!vt->cfg.reconnect_min_ms)
        vt->cfg.reconnect_min_ms = DEF_RECONNECT_MIN;
    if (!vt->cfg.reconnect_max_ms)
        vt->cfg.reconnect_max_ms = DEF_RECONNECT_MAX;
    if (!vt->cfg.out_buffer_size)
        vt->cfg.out_buffer_size = DEF_OUTBUF;
    vt->out_cap = (size_t)vt->cfg.out_buffer_size;
    vt->out = malloc(vt->out_cap);
    vt->fd = -1;
    vt->state = ST_STOPPED;
    vt->backoff_ms = vt->cfg.reconnect_min_ms;
    if (!vt->host || !vt->session || !vt->password || !vt->out ||
        vt->out_cap < VT_MESG_SIZE + 2 + VT_FRAME_SIZE) {
        vt_destroy(vt);
        return NULL;
    }
    *err = TUNNEL_OK;
    return vt;
}

void vt_destroy(vt_t *vt)
{
    if (!vt)
        return;
    close_socket(vt);
    if (vt->password)
        memset(vt->password, 0, strlen(vt->password));
    free(vt->host);
    free(vt->session);
    free(vt->password);
    free(vt->out);
    free(vt);
}

int vt_start(vt_t *vt)
{
    if (vt->state != ST_STOPPED)
        return -1;
    vt->backoff_ms = vt->cfg.reconnect_min_ms;
    start_connect(vt);
    return 0;
}

const char *vt_greeting(const vt_t *vt)
{
    return vt->greeting;
}

const vt_flags_t *vt_session_flags(const vt_t *vt)
{
    return &vt->flags;
}

int vt_is_up(const vt_t *vt)
{
    return vt->state == ST_ESTABLISHED;
}
