/*
 * vtesse-cli: Linux console client.
 *
 * Connects to a VTun server, runs the userspace stack (lwIP) over the tunnel,
 * obtains address and routes via DHCP (type ether) and reports events.
 * Local proxy: --socks PORT (SOCKS5 CONNECT) and --forward LPORT:IP:RPORT.
 * --fetch performs one HTTP GET through the stack to check TCP end to end.
 *
 * Credentials are read from a key=value file, never from the command line.
 * The session name is the device ID and doubles as the client MAC.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "netcore/loop.h"
#include "netcore/proxy.h"
#include "netcore/route.h"
#include "netcore/stack.h"
#include "vtesse/vtun_client.h"

#define DEFAULT_CONFIG "~/.config/vtesse/probe.conf"

typedef struct cfg_file {
    char host[256];
    char session[64];
    char password[128];
    char auth[16];
    char socks_user[256];
    char socks_password[256];
    int port;
} cfg_file_t;

static nc_loop_t loop;
static int verbose;

static void on_signal(int sig)
{
    (void)sig;
    loop.stop = 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: vtesse-cli [--config FILE] [--seconds N] [-v]\n"
            "                  [--socks PORT] [--forward LPORT:IP:RPORT]... [--listen ADDR]\n"
            "                  [--fetch http://IP[:PORT]/PATH]\n"
            "                  [--debug-ip A.B.C.D/P --debug-gw A.B.C.D]\n"
            "  FILE: key=value lines: host, port, session, password, auth (xor),\n"
            "        optional socks_user, socks_password (SOCKS5 user/password auth)\n"
            "        default " DEFAULT_CONFIG "\n");
}

static void expand_home(const char *in, char *out, size_t size)
{
    const char *home = getenv("HOME");

    if (in[0] == '~' && in[1] == '/' && home)
        snprintf(out, size, "%s%s", home, in + 1);
    else
        snprintf(out, size, "%s", in);
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static int load_config(const char *path, cfg_file_t *c)
{
    char full[512], line[512];
    FILE *f;

    memset(c, 0, sizeof(*c));
    snprintf(c->auth, sizeof(c->auth), "xor");
    expand_home(path, full, sizeof(full));
    f = fopen(full, "r");
    if (!f) {
        fprintf(stderr, "cannot open %s: %s\n", full, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *k = trim(line), *v = strchr(k, '=');

        if (*k == '#' || !v)
            continue;
        *v++ = '\0';
        k = trim(k);
        v = trim(v);
        if (!strcmp(k, "host"))
            snprintf(c->host, sizeof(c->host), "%s", v);
        else if (!strcmp(k, "port"))
            c->port = atoi(v);
        else if (!strcmp(k, "session"))
            snprintf(c->session, sizeof(c->session), "%s", v);
        else if (!strcmp(k, "password"))
            snprintf(c->password, sizeof(c->password), "%s", v);
        else if (!strcmp(k, "auth"))
            snprintf(c->auth, sizeof(c->auth), "%s", v);
        else if (!strcmp(k, "socks_user"))
            snprintf(c->socks_user, sizeof(c->socks_user), "%s", v);
        else if (!strcmp(k, "socks_password"))
            snprintf(c->socks_password, sizeof(c->socks_password), "%s", v);
    }
    fclose(f);
    if (!c->host[0] || c->port <= 0 || c->port > 65535 || !c->session[0] || !c->password[0]) {
        fprintf(stderr, "%s: host, port, session and password are required\n", full);
        return -1;
    }
    if (strcmp(c->auth, "xor") != 0) {
        fprintf(stderr, "%s: unsupported auth '%s' (only xor)\n", full, c->auth);
        return -1;
    }
    return 0;
}

static double elapsed(uint64_t t0)
{
    return (double)(nc_now_ms() - t0) / 1000.0;
}

static uint64_t t_start;
static vt_t *vt;

/* ---- --fetch: one HTTP GET through lwIP ---- */

static struct {
    int wanted, started, done;
    uint32_t ip;
    uint16_t port;
    char path[256];
    unsigned long bytes;
    char status[96];
    size_t status_len;
} fetch;

static int parse_ip4(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;

    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return -1;
    *out = a << 24 | b << 16 | c << 8 | d;
    return 0;
}

static int parse_url(const char *url)
{
    char host[64];
    const char *p = url, *slash, *colon;
    size_t hl;

    if (strncmp(p, "http://", 7) == 0)
        p += 7;
    slash = strchr(p, '/');
    hl = slash ? (size_t)(slash - p) : strlen(p);
    if (hl == 0 || hl >= sizeof(host))
        return -1;
    memcpy(host, p, hl);
    host[hl] = '\0';
    fetch.port = 80;
    colon = strchr(host, ':');
    if (colon) {
        fetch.port = (uint16_t)atoi(colon + 1);
        host[colon - host] = '\0';
    }
    snprintf(fetch.path, sizeof(fetch.path), "%s", slash ? slash : "/");
    return parse_ip4(host, &fetch.ip);
}

static void fetch_finish(struct tcp_pcb *pcb, const char *how)
{
    char ip[16];

    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK)
            tcp_abort(pcb);
    }
    fetch.done = 1;
    nc_ip4_format(fetch.ip, ip, sizeof(ip));
    printf("[%8.3f] fetch %s:%u%s: %s, %lu bytes, \"%s\"\n", (double)(nc_now_ms() - t_start) / 1000.0,
           ip, fetch.port, fetch.path, how, fetch.bytes, fetch.status);
    fflush(stdout);
}

static err_t fetch_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct pbuf *q;

    (void)arg;
    (void)err;
    if (!p) {
        fetch_finish(pcb, "ok");
        return ERR_OK;
    }
    for (q = p; q; q = q->next) {
        size_t i;
        for (i = 0; i < q->len && fetch.status_len < sizeof(fetch.status) - 1; i++) {
            char c = ((char *)q->payload)[i];
            if (c == '\r' || c == '\n') {
                fetch.status_len = sizeof(fetch.status);    /* first line complete */
                break;
            }
            fetch.status[fetch.status_len++] = c;
        }
    }
    fetch.bytes += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void fetch_err(void *arg, err_t err)
{
    (void)arg;
    fetch_finish(NULL, err == ERR_RST ? "reset" : err == ERR_ABRT ? "aborted" : "timeout/error");
}

static err_t fetch_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    char req[400], ip[16];
    int n;

    (void)arg;
    (void)err;
    nc_ip4_format(fetch.ip, ip, sizeof(ip));
    n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: vtesse-cli\r\n\r\n",
                 fetch.path, ip);
    tcp_write(pcb, req, (u16_t)n, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

static void fetch_start(void)
{
    struct tcp_pcb *pcb;
    ip4_addr_t a;

    fetch.started = 1;
    if (!nc_stack_route_ok(fetch.ip)) {
        snprintf(fetch.status, sizeof(fetch.status), "no route via tunnel");
        fetch_finish(NULL, "failed");
        return;
    }
    pcb = tcp_new();
    if (!pcb) {
        fetch_finish(NULL, "out of memory");
        return;
    }
    ip4_addr_set_u32(&a, lwip_htonl(fetch.ip));
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, fetch_recv);
    tcp_err(pcb, fetch_err);
    if (tcp_connect(pcb, &a, fetch.port, fetch_connected) != ERR_OK) {
        tcp_abort(pcb);
        fetch_finish(NULL, "connect failed");
    }
}

static void on_proxy_log(void *user, const char *msg)
{
    (void)user;
    printf("[%8.3f] proxy: %s\n", (double)(nc_now_ms() - t_start) / 1000.0, msg);
    fflush(stdout);
}

static int parse_forward(const char *s, nc_forward_t *f)
{
    char ip[32];
    unsigned lp, rp;

    if (sscanf(s, "%u:%31[0-9.]:%u", &lp, ip, &rp) != 3 || !lp || lp > 65535 || !rp || rp > 65535)
        return -1;
    f->local_port = (uint16_t)lp;
    f->port = (uint16_t)rp;
    return parse_ip4(ip, &f->ip);
}

/* ---- events ---- */

static void print_info(void)
{
    nc_stack_info_t in;
    char a[16], m[16], g[16], r[48];
    int i;

    nc_stack_get_info(&in);
    nc_ip4_format(in.addr, a, sizeof(a));
    nc_ip4_format(in.mask, m, sizeof(m));
    nc_ip4_format(in.gw, g, sizeof(g));
    printf("[%8.3f] address %s mask %s, option 3 %s%s\n", elapsed(t_start), a, m, g,
           in.default_route ? " (default route)" : " (ignored: classless routes present)");
    for (i = 0; i < in.routes.count; i++) {
        nc_route_format(&in.routes.r[i], r, sizeof(r));
        printf("[%8.3f]   route %s\n", elapsed(t_start), r);
    }
}

static void on_stack_event(void *user, int ev, int tev, int err)
{
    char flags[64];

    (void)user;
    if (ev == NC_STACK_EV_ADDR_BOUND) {
        print_info();
        if (fetch.wanted && !fetch.started)
            fetch_start();
    } else if (ev == NC_STACK_EV_ADDR_LOST) {
        printf("[%8.3f] address lost\n", elapsed(t_start));
    } else if (tev == TUNNEL_EV_UP) {
        vt_flags_format(vt_session_flags(vt), flags, sizeof(flags));
        printf("[%8.3f] up: %s, flags %s\n", elapsed(t_start), vt_greeting(vt), flags);
    } else if (err) {
        printf("[%8.3f] %s: %s\n", elapsed(t_start), tunnel_event_name(tev), tunnel_strerror(err));
    } else {
        printf("[%8.3f] %s\n", elapsed(t_start), tunnel_event_name(tev));
    }
    if (tev == TUNNEL_EV_FAILED)
        loop.stop = 1;
    fflush(stdout);
}

static int parse_prefix(const char *s, uint32_t *addr, uint32_t *mask)
{
    char buf[32];
    const char *slash = strchr(s, '/');
    int p;

    if (!slash || (size_t)(slash - s) >= sizeof(buf))
        return -1;
    memcpy(buf, s, (size_t)(slash - s));
    buf[slash - s] = '\0';
    p = atoi(slash + 1);
    if (p < 0 || p > 32 || parse_ip4(buf, addr) < 0)
        return -1;
    *mask = p ? 0xffffffffu << (32 - p) : 0;
    return 0;
}

int main(int argc, char **argv)
{
    const char *config = DEFAULT_CONFIG;
    double seconds = 0;
    cfg_file_t c;
    vt_cfg_t vc;
    nc_stack_cfg_t sc;
    nc_proxy_cfg_t pc;
    nc_proxy_t *px = NULL;
    tunnel_t t;
    nc_source_t src;
    int i, err, rc;

    memset(&sc, 0, sizeof(sc));
    memset(&pc, 0, sizeof(pc));
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            config = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "--fetch") && i + 1 < argc) {
            if (parse_url(argv[++i]) < 0) {
                fprintf(stderr, "--fetch: expected http://IPv4[:port]/path\n");
                return 2;
            }
            fetch.wanted = 1;
        } else if (!strcmp(argv[i], "--socks") && i + 1 < argc) {
            int p = atoi(argv[++i]);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, "--socks: expected a port\n");
                return 2;
            }
            pc.socks_port = (uint16_t)p;
        } else if (!strcmp(argv[i], "--forward") && i + 1 < argc) {
            if (pc.forward_count >= NC_PROXY_MAX_FORWARDS ||
                parse_forward(argv[++i], &pc.forwards[pc.forward_count]) < 0) {
                fprintf(stderr, "--forward: expected LPORT:A.B.C.D:RPORT (max %d)\n", NC_PROXY_MAX_FORWARDS);
                return 2;
            }
            pc.forward_count++;
        } else if (!strcmp(argv[i], "--listen") && i + 1 < argc) {
            pc.listen_addr = argv[++i];
        } else if (!strcmp(argv[i], "--debug-ip") && i + 1 < argc) {
            if (parse_prefix(argv[++i], &sc.addr, &sc.mask) < 0) {
                fprintf(stderr, "--debug-ip: expected A.B.C.D/P\n");
                return 2;
            }
            sc.static_ip = 1;
        } else if (!strcmp(argv[i], "--debug-gw") && i + 1 < argc) {
            if (parse_ip4(argv[++i], &sc.gw) < 0) {
                fprintf(stderr, "--debug-gw: expected A.B.C.D\n");
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (load_config(config, &c) < 0)
        return 2;
    if (nc_devid_to_mac(c.session, sc.mac) < 0) {
        /* not a device ID: still usable as a session name, MAC is derived */
        uint8_t h = 0;
        for (i = 0; c.session[i]; i++)
            h = (uint8_t)(h * 31 + (uint8_t)c.session[i]);
        memcpy(sc.mac, "\x02\x00\x5e\x00\x00\x00", 6);
        sc.mac[5] = h;
        fprintf(stderr, "warning: session name is not a device ID (12 x 0-9A-F); using MAC 02:00:5e:00:00:%02x\n", h);
    }

    memset(&vc, 0, sizeof(vc));
    vc.host = c.host;
    vc.port = (uint16_t)c.port;
    vc.session = c.session;
    vc.password = c.password;
    vc.auth = VT_AUTH_XOR;
    vt = vt_create(&vc, &err);
    memset(c.password, 0, sizeof(c.password));
    if (!vt) {
        fprintf(stderr, "configuration error: %s\n", tunnel_strerror(err));
        return 2;
    }

    t = vt_tunnel(vt);
    if (nc_stack_init(&t, &sc, on_stack_event, NULL) < 0) {
        fprintf(stderr, "stack initialisation failed\n");
        return 2;
    }

    if (pc.socks_port || pc.forward_count) {
        pc.socks_user = c.socks_user;
        pc.socks_password = c.socks_password;
        px = nc_proxy_create(&pc, on_proxy_log, NULL);
        memset(c.socks_password, 0, sizeof(c.socks_password));
        if (!px) {
            fprintf(stderr, "cannot start the local proxy\n");
            return 2;
        }
        if (pc.socks_port)
            printf("socks5 on %s:%u%s\n", pc.listen_addr ? pc.listen_addr : "127.0.0.1",
                   (unsigned)pc.socks_port, c.socks_user[0] ? " (user/password)" : "");
        for (i = 0; i < pc.forward_count; i++) {
            char ip[16];
            nc_ip4_format(pc.forwards[i].ip, ip, sizeof(ip));
            printf("forward %s:%u -> %s:%u\n", pc.listen_addr ? pc.listen_addr : "127.0.0.1",
                   (unsigned)pc.forwards[i].local_port, ip, (unsigned)pc.forwards[i].port);
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    nc_loop_init(&loop);
    src = nc_source_from_tunnel(&t);
    nc_loop_add(&loop, &src);
    src = nc_stack_timer_source();
    nc_loop_add(&loop, &src);
    if (px) {
        src = nc_proxy_source(px);
        nc_loop_add(&loop, &src);
    }

    t_start = nc_now_ms();
    vt_start(vt);
    nc_loop_run(&loop, seconds > 0 ? t_start + (uint64_t)(seconds * 1000) : 0);

    if (px) {
        nc_proxy_stats_t ps;
        nc_proxy_get_stats(px, &ps);
        printf("[%8.3f] proxy: accepted %lu, connected %lu, failed %lu, rejected %lu, up %llu B, down %llu B\n",
               elapsed(t_start), ps.accepted, ps.connected, ps.failed, ps.rejected, ps.bytes_up, ps.bytes_down);
        nc_proxy_destroy(px);
    }
    rc = vt_is_up(vt) ? 0 : 1;
    if (fetch.wanted && (!fetch.done || strncmp(fetch.status, "HTTP/", 5) != 0))
        rc = 1;
    t.ops->close(vt);
    vt_destroy(vt);
    return rc;
}
