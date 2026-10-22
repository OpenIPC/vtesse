/*
 * vtesse-cli: Linux console client.
 *
 * A userspace stack (lwIP) over the tunnel: address and routes via DHCP (type
 * ether), manual or learnt from the first packet (type tun); local proxy:
 * --socks PORT and --forward LPORT:IP:RPORT; inbound forwards from the tunnel
 * address: --accept PORT[:IP:PORT]; --fetch performs one HTTP GET through the
 * stack. No kernel interface: the TAP backend was removed in 2.0.0 (#72).
 *
 * Credentials are read from a key=value file, never from the command line
 * ("--install" writes that file from its keys, see install.c).
 * The session name is the device ID and doubles as the client MAC.
 */
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "env.h"
#include "install.h"
#include "netcore/loop.h"
#include "netcore/route.h"
#include "vtesse/version.h"
#include "vtesse/vtun_client.h"
#include "vtesse/vtun_proto.h"

#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "netcore/proxy.h"
#include "netcore/stack.h"
#include "netcore/udprelay.h"   /* limits only; the relay itself needs lwIP */

/* optional modules (make SOCKS=0 UDP_RELAY=0) */
#ifndef NC_WITH_SOCKS
#define NC_WITH_SOCKS 1
#endif
#ifndef VTESSE_WITH_UDP_RELAY
#define VTESSE_WITH_UDP_RELAY 1
#endif

#if !VTESSE_WITH_UDP_RELAY
/* built without the UDP relay (udprelay.c not linked): the options are refused */
int nc_udp_relay_parse(const char *s, uint16_t *port, uint16_t *local_port)
{
    (void)s; (void)port; (void)local_port;
    return -1;
}
nc_udp_relay_t *nc_udp_relay_create(uint16_t port, const char *dst_addr, uint16_t dst_port,
                                    char *err, size_t err_size)
{
    (void)port; (void)dst_addr; (void)dst_port;
    snprintf(err, err_size, "built without the UDP relay (make UDP_RELAY=0)");
    return NULL;
}
void nc_udp_relay_get_stats(const nc_udp_relay_t *r, nc_udp_relay_stats_t *st)
{
    (void)r;
    memset(st, 0, sizeof(*st));
}
void nc_udp_relay_destroy(nc_udp_relay_t *r)
{
    (void)r;
}
#endif

#define DEFAULT_CONFIG "~/.config/vtesse/probe.conf"
#define ROUTE_WAIT_LOG_S 60     /* seconds between the "still waiting" lines (#73) */

typedef struct cfg_file {
    char host[256];
    char session[64];
    char id_from[32];       /* interface whose MAC is the device ID */
    char ip[32], gw[32];    /* manual IPv4 (type tun, or instead of DHCP) */
    char route[16][32];     /* NET/P, repeatable */
    char udp_relay[NC_UDP_RELAY_MAX][16];   /* PORT[:LOCAL_PORT], repeatable */
    int udp_relay_count;
    char accept[NC_PROXY_MAX_ACCEPTS][32];  /* PORT[:IP:PORT], repeatable (#67) */
    int accept_count;
    int route_count;
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

/* 0 when there is a default route; -1 when a signal stopped the wait (#73) */
static int wait_default_route(void)
{
    unsigned waited = 0;

    while (!ve_has_default_route()) {
        if (loop.stop)
            return -1;
        if (!waited)
            printf("waiting for a default route\n");
        else if (waited % ROUTE_WAIT_LOG_S == 0)
            printf("still waiting for a default route (%u min)\n", waited / 60);
        fflush(stdout);
        sleep(1);
        waited++;
    }
    if (waited)
        printf("default route present after %u s\n", waited);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: vtesse-cli [--config FILE] [--seconds N] [-v] [--version]\n"
            "                  [--socks PORT] [--forward LPORT:IP:RPORT]... [--listen ADDR]\n"
            "                  [--accept PORT[:IP:PORT]]...  (inbound: tunnel address:PORT -> host)\n"
            "                  [--fetch http://IP[:PORT]/PATH] [--udp-relay PORT[:LOCAL_PORT]]...\n"
            "                  [--ip A.B.C.D/P|learn [--gw A.B.C.D]]   (manual IPv4 instead of DHCP)\n"
            "  --id-from IFACE  device ID (session name) = MAC of IFACE, e.g. eth0\n"
            "  --show-config    print the settings in effect and exit\n"
            "  --no-env         ignore the U-Boot environment (fw_printenv vtesse, vtesse_*)\n"
            "  FILE: key=value lines: host, port, session, password, auth (xor),\n"
            "        optional id_from (instead of session: MAC of that interface),\n"
            "        ip=A.B.C.D/P, gw=A.B.C.D, route=NET/P (repeatable): manual IPv4\n"
            "        (needed for type tun; ip=learn: the address of the first packet),\n"
            "        accept=PORT[:IP:PORT] (repeatable, up to %d): inbound forwards, by default\n"
            "        to the same port on the address of the default-route interface,\n"
            "        udp_relay=PORT[:LOCAL_PORT] (repeatable): UDP to the tunnel address -> 127.0.0.1\n"
            "        socks_user, socks_password (SOCKS5 user/password auth)\n"
            "        default " DEFAULT_CONFIG "\n", NC_PROXY_MAX_ACCEPTS);
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

/* Reads the file into c. A missing file is an error only when must_exist; then
 * returns 1 (the settings come from the U-Boot environment). */
static int load_config(const char *path, cfg_file_t *c, int must_exist)
{
    char full[512], line[512];
    FILE *f;

    memset(c, 0, sizeof(*c));
    snprintf(c->auth, sizeof(c->auth), "xor");
    expand_home(path, full, sizeof(full));
    f = fopen(full, "r");
    if (!f) {
        if (errno == ENOENT && !must_exist)
            return 1;
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
        else if (!strcmp(k, "port")) {
            char *end;
            long port = strtol(v, &end, 10);

            /* -1: refused later; 0 would be taken for "not set" and become 5000 */
            c->port = *v && !*end && port >= 1 && port <= 65535 ? (int)port : -1;
        }
        else if (!strcmp(k, "session"))
            snprintf(c->session, sizeof(c->session), "%s", v);
        else if (!strcmp(k, "ip"))
            snprintf(c->ip, sizeof(c->ip), "%s", v);
        else if (!strcmp(k, "gw"))
            snprintf(c->gw, sizeof(c->gw), "%s", v);
        else if (!strcmp(k, "accept")) {
            if (c->accept_count >= NC_PROXY_MAX_ACCEPTS) {
                fprintf(stderr, "%s: at most %d accept lines\n", full, NC_PROXY_MAX_ACCEPTS);
                fclose(f);
                return -1;
            }
            snprintf(c->accept[c->accept_count++], sizeof(c->accept[0]), "%s", v);
        } else if (!strcmp(k, "udp_relay")) {
            if (c->udp_relay_count >= NC_UDP_RELAY_MAX) {
                fprintf(stderr, "%s: at most %d udp_relay lines\n", full, NC_UDP_RELAY_MAX);
                fclose(f);
                return -1;
            }
            snprintf(c->udp_relay[c->udp_relay_count++], sizeof(c->udp_relay[0]), "%s", v);
        } else if (!strcmp(k, "route")) {
            if (c->route_count >= (int)(sizeof(c->route) / sizeof(c->route[0]))) {
                fprintf(stderr, "%s: too many route lines\n", full);
                fclose(f);
                return -1;
            }
            snprintf(c->route[c->route_count++], sizeof(c->route[0]), "%s", v);
        } else if (!strcmp(k, "id_from"))
            snprintf(c->id_from, sizeof(c->id_from), "%s", v);
        else if (!strcmp(k, "password"))
            snprintf(c->password, sizeof(c->password), "%s", v);
        else if (!strcmp(k, "auth"))
            snprintf(c->auth, sizeof(c->auth), "%s", v);
        else if (!strcmp(k, "socks_user"))
            snprintf(c->socks_user, sizeof(c->socks_user), "%s", v);
        else if (!strcmp(k, "socks_password"))
            snprintf(c->socks_password, sizeof(c->socks_password), "%s", v);
        else if (!strcmp(k, "tap") || !strcmp(k, "hook") || !strcmp(k, "ifname")) {
            /* a 1.x setting must not be ignored silently: the client would run another mode */
            fprintf(stderr, "%s: %s: the TAP backend was removed in vtesse 2.0.0 (#72); remove the line "
                            "(inbound access: accept=PORT)\n", full, k);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

static int check_config(const cfg_file_t *c, const char *where)
{
    if (!c->host[0] || c->port <= 0 || c->port > 65535 || !c->password[0]) {
        fprintf(stderr, "%s: host, port, session (or id_from) and password are required\n", where);
        return -1;
    }
    if (strcmp(c->auth, "xor") != 0) {
        fprintf(stderr, "%s: unsupported auth '%s' (only xor)\n", where, c->auth);
        return -1;
    }
    return 0;
}

static int env_any(const ve_t *e)
{
    return e->host[0] || e->port[0] || e->session[0] || e->id_from[0] || e->password[0] ||
           e->ip[0] || e->gw[0];
}

/* The U-Boot variables override the file (#64), each one by itself. */
static void apply_env(cfg_file_t *c, const ve_t *e)
{
    char names[160] = "";
    size_t i;

    for (i = 0; i < ve_nvars; i++) {
        const char *key = ve_vars[i].key, *v = NULL;
        char *dst = NULL;
        size_t size = 0;

        if (!strcmp(key, "host"))
            v = e->host, dst = c->host, size = sizeof(c->host);
        else if (!strcmp(key, "port"))
            v = e->port;
        else if (!strcmp(key, "session"))
            v = e->session, dst = c->session, size = sizeof(c->session);
        else if (!strcmp(key, "id_from"))
            v = e->id_from, dst = c->id_from, size = sizeof(c->id_from);
        else if (!strcmp(key, "password"))
            v = e->password, dst = c->password, size = sizeof(c->password);
        else if (!strcmp(key, "ip"))
            v = e->ip, dst = c->ip, size = sizeof(c->ip);
        else if (!strcmp(key, "gw"))
            v = e->gw, dst = c->gw, size = sizeof(c->gw);
        if (!v || !v[0])
            continue;
        if (dst)
            snprintf(dst, size, "%s", v);
        else {
            char *end;
            long port = strtol(v, &end, 10);

            /* -1: check_config() refuses it, a typo must not become port 5000 */
            c->port = *end || port < 1 || port > 65535 ? -1 : (int)port;
            if (c->port < 0)
                fprintf(stderr, "vtesse_port: expected 1..65535, got '%s'\n", v);
        }
        /* session and id_from exclude each other: the variable replaces the file's */
        if (!strcmp(key, "session"))
            c->id_from[0] = '\0';
        else if (!strcmp(key, "id_from"))
            c->session[0] = '\0';
        snprintf(names + strlen(names), sizeof(names) - strlen(names), " %s", ve_vars[i].name);
    }
    printf("settings from the U-Boot environment:%s\n", names);
}

static int parse_ip4(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;

    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return -1;
    *out = a << 24 | b << 16 | c << 8 | d;
    return 0;
}

static int parse_prefix(const char *s, uint32_t *addr, uint32_t *mask)
{
    char buf[32];
    const char *slash = strchr(s, '/'), *d;
    int p = 0;

    if (!slash || (size_t)(slash - s) >= sizeof(buf))
        return -1;
    memcpy(buf, s, (size_t)(slash - s));
    buf[slash - s] = '\0';
    /* digits only: "/garbage" must not become /0 */
    for (d = slash + 1; *d >= '0' && *d <= '9' && p <= 32; d++)
        p = p * 10 + (*d - '0');
    if (d == slash + 1 || *d || p > 32 || parse_ip4(buf, addr) < 0)
        return -1;
    *mask = p ? 0xffffffffu << (32 - p) : 0;
    return 0;
}

/* Device ID from the MAC of a local interface (/sys/class/net/IFACE/address). */
static int id_from_interface(const char *ifname, char *id, size_t size)
{
    char path[96], line[64];
    FILE *f;
    int rc;

    if (!ifname[0] || strchr(ifname, '/') || !strcmp(ifname, ".") || !strcmp(ifname, "..") ||
        strlen(ifname) > 31 || size < 13) {
        fprintf(stderr, "id_from: bad interface name '%s'\n", ifname);
        return -1;
    }
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "id_from: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }
    rc = fgets(line, sizeof(line), f) ? nc_devid_from_mac_str(line, id) : -1;
    fclose(f);
    if (rc < 0)
        fprintf(stderr, "id_from: %s has no usable unicast MAC\n", ifname);
    return rc;
}

static double elapsed(uint64_t t0)
{
    return (double)(nc_now_ms() - t0) / 1000.0;
}

static uint64_t t_start;
static vt_t *vt;

static void print_tunnel_event(int tev, int err);



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
        char *end;
        long port = strtol(colon + 1, &end, 10);
        if (end == colon + 1 || *end || port < 1 || port > 65535)
            return -1;
        fetch.port = (uint16_t)port;
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

static int manual_ip;   /* ip= / --ip: no DHCP */
static int learn_mode;  /* ip=learn: type tun only (#68) */
static int learnt_once;
static int config_error;    /* found only once the session is up: exit code 2 */
static struct {
    uint16_t port, local;
    nc_udp_relay_t *r;
} relays[NC_UDP_RELAY_MAX];
static int nrelays;

static void print_info(void)
{
    nc_stack_info_t in;
    char a[16], m[16], g[16], r[48];
    int i;

    nc_stack_get_info(&in);
    learnt_once = 1;
    nc_ip4_format(in.addr, a, sizeof(a));
    nc_ip4_format(in.mask, m, sizeof(m));
    nc_ip4_format(in.gw, g, sizeof(g));
    if (learn_mode)
        printf("[%8.3f] address %s mask %s (learnt), peer %s%s\n", elapsed(t_start), a, m, g,
               in.default_route ? " (default route)" : "");
    else if (manual_ip)
        printf("[%8.3f] address %s mask %s (manual), gw %s%s\n", elapsed(t_start), a, m, g,
               in.default_route ? " (default route)" : "");
    else
        printf("[%8.3f] address %s mask %s, option 3 %s%s\n", elapsed(t_start), a, m, g,
               in.default_route ? " (default route)" : " (ignored: classless routes present)");
    for (i = 0; i < in.routes.count; i++) {
        nc_route_format(&in.routes.r[i], r, sizeof(r));
        printf("[%8.3f]   route %s\n", elapsed(t_start), r);
    }
    for (i = 0; i < nrelays; i++)
        printf("[%8.3f]   udp relay %s:%u -> 127.0.0.1:%u\n", elapsed(t_start), a,
               (unsigned)relays[i].port, (unsigned)relays[i].local);
}

static void on_stack_event(void *user, int ev, int tev, int err)
{
    (void)user;
    if (ev == NC_STACK_EV_ADDR_BOUND) {
        print_info();
        if (fetch.wanted && !fetch.started)
            fetch_start();
    } else if (ev == NC_STACK_EV_ADDR_LOST) {
        printf("[%8.3f] address lost\n", elapsed(t_start));
    } else {
        print_tunnel_event(tev, err);
    }
    fflush(stdout);
}


static void print_accept(const nc_accept_t *a, const char *prefix)
{
    char ip[16];

    if (a->ip)
        nc_ip4_format(a->ip, ip, sizeof(ip));
    printf("%s%u -> %s:%u\n", prefix, (unsigned)a->port, a->ip ? ip : "(default-route address)",
           (unsigned)a->local_port);
}

static void print_tunnel_event(int tev, int err)
{
    char flags[64];

    if (tev == TUNNEL_EV_UP) {
        vt_flags_format(vt_session_flags(vt), flags, sizeof(flags));
        printf("[%8.3f] up: %s, flags %s\n", elapsed(t_start), vt_greeting(vt), flags);
        if (learn_mode && vt_tunnel(vt).ops->link_type(vt) != TUNNEL_LINK_L3) {
            printf("[%8.3f] ip=learn needs a type tun session; this one is type ether (use DHCP)\n",
                   elapsed(t_start));
            config_error = 1;
            loop.stop = 1;
        } else if (learn_mode && !learnt_once)
            printf("[%8.3f] waiting for the first packet from the server for the address\n", elapsed(t_start));
    } else if (err) {
        printf("[%8.3f] %s: %s\n", elapsed(t_start), tunnel_event_name(tev), tunnel_strerror(err));
    } else {
        printf("[%8.3f] %s\n", elapsed(t_start), tunnel_event_name(tev));
    }
    if (tev == TUNNEL_EV_FAILED)
        loop.stop = 1;
}



int main(int argc, char **argv)
{
    const char *config = DEFAULT_CONFIG, *id_from = NULL, *ip_arg = NULL, *gw_arg = NULL;
    int no_env = 0, show_config = 0, env_mode = 0, derived_password = 0;
    ve_t env;
    uint32_t st_addr = 0, st_mask = 0, st_gw = 0;
    nc_routes_t st_routes;
    double seconds = 0;
    cfg_file_t c;
    vt_cfg_t vc;
    uint8_t mac[6];
    tunnel_t t;
    nc_source_t src;
    int i, err, rc, stop_early, learn_ip = 0, accept_none = 0;
    nc_stack_cfg_t sc;
    nc_proxy_cfg_t pc;
    nc_proxy_t *px = NULL;

    memset(&sc, 0, sizeof(sc));
    memset(&pc, 0, sizeof(pc));
    if (vi_wanted(argc, argv))
        return vi_main(argc, argv);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            config = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            const char *arg = argv[++i];
            char *end;
            seconds = strtod(arg, &end);
            /* "1m" or "-5" must not mean "forever"; inf or 1e30 would overflow the deadline */
            if (end == arg || *end || !(seconds > 0) || !(seconds <= 1e9)) {
                fprintf(stderr, "--seconds: expected a positive number of seconds up to 1e9, got '%s'\n", arg);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "--no-env"))
            no_env = 1;
        else if (!strcmp(argv[i], "--show-config"))
            show_config = 1;
        else if (!strcmp(argv[i], "--id-from") && i + 1 < argc)
            id_from = argv[++i];
        else if (!strcmp(argv[i], "--version")) {
            printf("vtesse-cli %s (lwip, forwards, accept%s%s)\n", VTESSE_VERSION,
                   NC_WITH_SOCKS ? ", socks5" : "", VTESSE_WITH_UDP_RELAY ? ", udp-relay" : "");
            return 0;
        }
        else if (!strcmp(argv[i], "--tap") || !strcmp(argv[i], "--ifname") || !strcmp(argv[i], "--hook")) {
            fprintf(stderr, "%s: the TAP backend was removed in vtesse 2.0.0 (#72): the client runs the "
                            "userspace stack only; inbound access: --accept PORT\n", argv[i]);
            return 2;
        }
        else if (!strcmp(argv[i], "--fetch") && i + 1 < argc) {
            if (parse_url(argv[++i]) < 0) {
                fprintf(stderr, "--fetch: expected http://IPv4[:port]/path\n");
                return 2;
            }
            fetch.wanted = 1;
        } else if (!strcmp(argv[i], "--socks") && i + 1 < argc && !NC_WITH_SOCKS) {
            fprintf(stderr, "--socks: built without SOCKS5 (make SOCKS=0)\n");
            return 2;
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
        } else if (!strcmp(argv[i], "--udp-relay") && !VTESSE_WITH_UDP_RELAY) {
            fprintf(stderr, "--udp-relay: built without the UDP relay (make UDP_RELAY=0)\n");
            return 2;
        } else if (!strcmp(argv[i], "--udp-relay") && i + 1 < argc) {
            if (nrelays >= NC_UDP_RELAY_MAX ||
                nc_udp_relay_parse(argv[++i], &relays[nrelays].port, &relays[nrelays].local) < 0) {
                fprintf(stderr, "--udp-relay: expected PORT[:LOCAL_PORT] (max %d)\n", NC_UDP_RELAY_MAX);
                return 2;
            }
            nrelays++;
        } else if (!strcmp(argv[i], "--accept") && i + 1 < argc && !strcmp(argv[i + 1], "none")) {
            accept_none = 1;
            i++;
        } else if (!strcmp(argv[i], "--accept") && i + 1 < argc) {
            if (pc.accept_count >= NC_PROXY_MAX_ACCEPTS ||
                nc_accept_parse(argv[++i], &pc.accepts[pc.accept_count]) < 0) {
                fprintf(stderr, "--accept: expected PORT or PORT:A.B.C.D:PORT (max %d)\n", NC_PROXY_MAX_ACCEPTS);
                return 2;
            }
            pc.accept_count++;
        } else if (!strcmp(argv[i], "--listen") && i + 1 < argc) {
            pc.listen_addr = argv[++i];
        } else if ((!strcmp(argv[i], "--ip") || !strcmp(argv[i], "--debug-ip")) && i + 1 < argc) {
            ip_arg = argv[++i];
        } else if ((!strcmp(argv[i], "--gw") || !strcmp(argv[i], "--debug-gw")) && i + 1 < argc) {
            gw_arg = argv[++i];
        }
        else {
            usage();
            return 2;
        }
    }
    if (!no_env)
        env_mode = ve_load(&env);
    rc = load_config(config, &c, !env_mode);
    if (rc < 0)
        return 2;
    /* every vtesse_* variable overrides its key; only "vtesse" (the server) also
     * allows a missing file and brings the VTun defaults */
    /* which one is meant cannot be told: the other one may be stale */
    if (!no_env && env.session[0] && env.id_from[0]) {
        fprintf(stderr, "vtesse_session and vtesse_id_from are both set in the U-Boot environment: "
                        "clear one (fw_setenv vtesse_id_from)\n");
        return 2;
    }
    if (!no_env && env_any(&env))
        apply_env(&c, &env);
    /* the settings that do not need the device's MAC are checked before the route wait:
     * a mistake must not wait for the network to be reported */
    /* manual IPv4: command line overrides the file */
    if (ip_arg)
        snprintf(c.ip, sizeof(c.ip), "%s", ip_arg);
    if (gw_arg)
        snprintf(c.gw, sizeof(c.gw), "%s", gw_arg);
    memset(&st_routes, 0, sizeof(st_routes));
    if (!c.ip[0] && (c.gw[0] || c.route_count)) {
        fprintf(stderr, "gw/route need ip\n");
        return 2;
    }
    learn_ip = !strcmp(c.ip, "learn");     /* type tun: the first packet tells it (#68) */
    if (learn_ip && c.gw[0]) {
        fprintf(stderr, "ip=learn: no gw (the peer is the source of the first packet)\n");
        return 2;
    }
    if (c.ip[0] && !learn_ip && parse_prefix(c.ip, &st_addr, &st_mask) < 0) {
        fprintf(stderr, "ip: expected A.B.C.D/P or learn, got '%s'\n", c.ip);
        return 2;
    }
    if (c.gw[0] && parse_ip4(c.gw, &st_gw) < 0) {
        fprintf(stderr, "gw: expected A.B.C.D, got '%s'\n", c.gw);
        return 2;
    }
    for (i = 0; i < c.route_count; i++) {
        uint32_t net, m;
        int p = 0;

        if (parse_prefix(c.route[i], &net, &m) < 0) {
            fprintf(stderr, "route: expected NET/P, got '%s'\n", c.route[i]);
            return 2;
        }
        while (p < 32 && (m & (0x80000000u >> p)))
            p++;
        nc_routes_add(&st_routes, net, (uint8_t)p, st_gw);
    }

    /* inbound forwards from the file come after the command line ones (#67); "none" (file
     * or command line) means no inbound forwards at all, also no service defaults */
    for (i = 0; i < c.accept_count; i++) {
        if (!strcmp(c.accept[i], "none")) {
            accept_none = 1;
            continue;
        }
        if (pc.accept_count >= NC_PROXY_MAX_ACCEPTS ||
            nc_accept_parse(c.accept[i], &pc.accepts[pc.accept_count]) < 0) {
            fprintf(stderr, "%s: accept: expected PORT or PORT:A.B.C.D:PORT (max %d), got '%s'\n", config,
                    NC_PROXY_MAX_ACCEPTS, c.accept[i]);
            return 2;
        }
        pc.accept_count++;
    }
    /* the service started from the U-Boot environment: the camera's web, ssh and RTSP are
     * reachable from the server, on the camera's own LAN address (its web asks for the
     * password as from the LAN) */
    if (accept_none && pc.accept_count) {
        fprintf(stderr, "accept=none cannot be combined with other accept lines or --accept\n");
        return 2;
    }
    if (env_mode && !pc.accept_count && !accept_none) {
        static const uint16_t def[] = { 80, 22, 554 };

        for (i = 0; i < (int)(sizeof(def) / sizeof(def[0])); i++) {
            pc.accepts[i].port = pc.accepts[i].local_port = def[i];
            pc.accepts[i].ip = 0;
        }
        pc.accept_count = i;
    }
    /* without a default route there is nothing to connect to (#73): wait for one in every
     * start mode, with no time limit. At boot the uplink (wlan0 with a background udhcpc)
     * may get it after the service starts; the MAC is then the uplink's, not eth0's. Any
     * default route ends the wait: through PPPoE or tun (no MAC) eth0 is taken as before. */
    if (!show_config) {
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        if (wait_default_route() < 0)
            return 0;               /* stopped while waiting */
    }
    if (env_mode) {
        /* only the server given: the VTun defaults of OpenIPC's tunnel */
        if (!c.port)
            c.port = 5000;
        if (!c.session[0] && !c.id_from[0] && !id_from &&
            ve_default_iface(c.id_from, sizeof(c.id_from)) < 0) {
            fprintf(stderr, "no interface with a MAC for the device ID: set vtesse_session or vtesse_id_from\n");
            return 2;
        }
    }
    if (c.udp_relay_count && !VTESSE_WITH_UDP_RELAY) {
        fprintf(stderr, "%s: udp_relay: built without the UDP relay\n", config);
        return 2;
    }
    if (id_from)
        snprintf(c.id_from, sizeof(c.id_from), "%s", id_from);
    if (c.id_from[0]) {
        if (c.session[0] && !id_from) {
            fprintf(stderr, "%s: session and id_from are mutually exclusive\n", config);
            return 2;
        }
        if (id_from_interface(c.id_from, c.session, sizeof(c.session)) < 0)
            return 2;
        printf("device ID %s (MAC of %s)\n", c.session, c.id_from);
    } else if (!c.session[0]) {
        fprintf(stderr, "%s: session or id_from is required\n", config);
        return 2;
    }
    if (env_mode && !c.password[0]) {
        char mac_str[18], pw_if[32];

        /* the default-route interface, as OpenIPC's tunnel does, whatever the session */
        if (ve_default_iface(pw_if, sizeof(pw_if)) < 0)
            pw_if[0] = '\0';
        if (!pw_if[0] || ve_iface_mac(pw_if, mac_str) < 0) {
            fprintf(stderr, "no interface with a MAC for the password: set vtesse_password\n");
            return 2;
        }
        ve_vtun_password(mac_str, c.password);
        derived_password = 1;
        printf("password: SHA-1 of the MAC of %s, as VTun on OpenIPC\n", pw_if);
    }
    if (check_config(&c, env_mode ? "U-Boot environment and config" : config) < 0)
        return 2;
    if (show_config && !vt_session_name_valid(c.session)) {
        fprintf(stderr, "session '%s': no spaces, colons or control characters\n", c.session);
        return 2;
    }
    /* the same check as at start, without creating the relays */
    for (i = 0; show_config && i < c.udp_relay_count; i++) {
        uint16_t port, local;

        if (nrelays + i >= NC_UDP_RELAY_MAX || nc_udp_relay_parse(c.udp_relay[i], &port, &local) < 0) {
            fprintf(stderr, "%s: udp_relay: expected PORT[:LOCAL_PORT] (max %d)\n", config, NC_UDP_RELAY_MAX);
            return 2;
        }
    }
    if (show_config) {
        printf("host %s\nport %d\nsession %s\npassword %s\n", c.host, c.port, c.session,
               derived_password ? c.password : "(set)");
        if (c.ip[0])
            printf("ip %s%s%s\n", c.ip, c.gw[0] ? " gw " : "", c.gw);
        for (i = 0; i < c.route_count; i++)
            printf("route %s\n", c.route[i]);
        for (i = 0; i < c.udp_relay_count; i++)
            printf("udp_relay %s\n", c.udp_relay[i]);
        for (i = 0; i < pc.accept_count; i++)
            print_accept(&pc.accepts[i], "accept ");
        if (c.socks_user[0])
            printf("socks_user %s\nsocks_password (set)\n", c.socks_user);
        return 0;
    }
    if (nc_devid_to_mac(c.session, mac) < 0) {
        /* not a device ID: still usable as a session name, MAC is derived */
        uint8_t h = 0;
        for (i = 0; c.session[i]; i++)
            h = (uint8_t)(h * 31 + (uint8_t)c.session[i]);
        memcpy(mac, "\x02\x00\x5e\x00\x00\x00", 6);
        mac[5] = h;
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

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    stop_early = loop.stop;     /* a signal after the route wait (#73) is kept */
    nc_loop_init(&loop);
    loop.stop = stop_early;
    src = nc_source_from_tunnel(&t);
    nc_loop_add(&loop, &src);

    memcpy(sc.mac, mac, 6);
    if (learn_ip) {
        sc.learn_ip = learn_mode = 1;
        sc.routes = st_routes;
    } else if (c.ip[0]) {
        sc.static_ip = manual_ip = 1;
        sc.addr = st_addr;
        sc.mask = st_mask;
        sc.gw = st_gw;
        sc.routes = st_routes;
    }
    if (nc_stack_init(&t, &sc, on_stack_event, NULL) < 0) {
        fprintf(stderr, "stack initialisation failed\n");
        return 2;
    }
    src = nc_stack_timer_source();
    nc_loop_add(&loop, &src);
    /* UDP relays from the file come after the command line ones */
    for (i = 0; i < c.udp_relay_count; i++) {
        if (nrelays >= NC_UDP_RELAY_MAX ||
            nc_udp_relay_parse(c.udp_relay[i], &relays[nrelays].port, &relays[nrelays].local) < 0) {
            fprintf(stderr, "%s: udp_relay: expected PORT[:LOCAL_PORT] (max %d)\n", config, NC_UDP_RELAY_MAX);
            return 2;
        }
        nrelays++;
    }
    for (i = 0; i < nrelays; i++) {
        char msg[128];

        relays[i].r = nc_udp_relay_create(relays[i].port, NULL, relays[i].local, msg, sizeof(msg));
        if (!relays[i].r) {
            fprintf(stderr, "%s\n", msg);
            return 2;
        }
        printf("udp relay :%u -> 127.0.0.1:%u\n", (unsigned)relays[i].port, (unsigned)relays[i].local);
    }
    if (pc.socks_port || pc.forward_count || pc.accept_count) {
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
        for (i = 0; i < pc.accept_count; i++)
            print_accept(&pc.accepts[i], "accept tunnel:");
        src = nc_proxy_source(px);
        nc_loop_add(&loop, &src);
    }

    t_start = nc_now_ms();
    vt_start(vt);
    nc_loop_run(&loop, seconds > 0 ? t_start + (uint64_t)(seconds * 1000) : 0);

    rc = config_error ? 2 : vt_is_up(vt) ? 0 : 1;
    if (px) {
        nc_proxy_stats_t ps;
        nc_proxy_get_stats(px, &ps);
        printf("[%8.3f] proxy: accepted %lu, connected %lu, failed %lu, rejected %lu, up %llu B, down %llu B\n",
               elapsed(t_start), ps.accepted, ps.connected, ps.failed, ps.rejected, ps.bytes_up, ps.bytes_down);
        nc_proxy_destroy(px);
    }
    if (fetch.wanted && (!fetch.done || strncmp(fetch.status, "HTTP/", 5) != 0))
        rc = 1;
    for (i = 0; i < nrelays; i++) {
        nc_udp_relay_stats_t us;

        nc_udp_relay_get_stats(relays[i].r, &us);
        printf("[%8.3f] udp relay :%u: datagrams %lu, %llu B, dropped %lu\n", elapsed(t_start),
               (unsigned)relays[i].port, us.datagrams, us.bytes, us.dropped);
        nc_udp_relay_destroy(relays[i].r);
    }
    t.ops->close(vt);
    vt_destroy(vt);
    return rc;
}
