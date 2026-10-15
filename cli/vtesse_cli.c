/*
 * vtesse-cli: Linux console client.
 *
 * Two backends:
 *  - lwIP (default): userspace stack over the tunnel, address and routes via
 *    DHCP (type ether), local proxy: --socks PORT and --forward LPORT:IP:RPORT;
 *    --fetch performs one HTTP GET through the stack.
 *  - TAP (--tap): kernel interface through /dev/net/tun; addressing is done by
 *    the system (e.g. a DHCP client started from the --hook script).
 * Build without lwIP: make LWIP=0 (TAP only).
 *
 * Credentials are read from a key=value file, never from the command line.
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

#include "netcore/loop.h"
#include "netcore/route.h"
#include "vtesse/version.h"
#include "vtesse/vtun_client.h"

#ifndef VTESSE_WITH_LWIP
#define VTESSE_WITH_LWIP 1
#endif

#if VTESSE_WITH_LWIP
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "netcore/proxy.h"
#include "netcore/stack.h"
#endif
#include "netcore/udprelay.h"   /* limits only; the relay itself needs lwIP */

/* optional modules (make SOCKS=0 UDP_RELAY=0 TAP=0) */
#ifndef NC_WITH_SOCKS
#define NC_WITH_SOCKS VTESSE_WITH_LWIP
#endif
#ifndef VTESSE_WITH_UDP_RELAY
#define VTESSE_WITH_UDP_RELAY VTESSE_WITH_LWIP
#endif
#ifndef VTESSE_WITH_TAP
#ifdef __linux__
#define VTESSE_WITH_TAP 1
#else
#define VTESSE_WITH_TAP 0
#endif
#endif
#if VTESSE_WITH_TAP
#include "netcore/tapdev.h"
#endif

#if VTESSE_WITH_LWIP && !VTESSE_WITH_UDP_RELAY
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

typedef struct cfg_file {
    char host[256];
    char session[64];
    char id_from[32];       /* interface whose MAC is the device ID */
    char ip[32], gw[32];    /* manual IPv4 (type tun, or instead of DHCP) */
    char route[16][32];     /* NET/P, repeatable */
    char udp_relay[NC_UDP_RELAY_MAX][16];   /* PORT[:LOCAL_PORT], repeatable */
    int udp_relay_count;
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

static void usage(void)
{
    fprintf(stderr,
            "usage: vtesse-cli [--config FILE] [--seconds N] [-v] [--version]\n"
#if VTESSE_WITH_LWIP
            "  lwIP backend (default):\n"
            "                  [--socks PORT] [--forward LPORT:IP:RPORT]... [--listen ADDR]\n"
            "                  [--fetch http://IP[:PORT]/PATH] [--udp-relay PORT[:LOCAL_PORT]]...\n"
            "                  [--ip A.B.C.D/P [--gw A.B.C.D]]   (manual IPv4 instead of DHCP)\n"
#endif
#if VTESSE_WITH_TAP
            "  TAP backend:    --tap [--ifname NAME] [--hook SCRIPT]\n"
#endif
            "  --id-from IFACE  device ID (session name) = MAC of IFACE, e.g. eth0\n"
            "  FILE: key=value lines: host, port, session, password, auth (xor),\n"
            "        optional id_from (instead of session: MAC of that interface),\n"
            "        ip=A.B.C.D/P, gw=A.B.C.D, route=NET/P (repeatable): manual IPv4\n"
            "        udp_relay=PORT[:LOCAL_PORT] (repeatable): UDP to the tunnel address -> 127.0.0.1\n"
            "        (needed for type tun; with --tap passed to the hook),\n"
            "        socks_user, socks_password (SOCKS5 user/password auth)\n"
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
        else if (!strcmp(k, "ip"))
            snprintf(c->ip, sizeof(c->ip), "%s", v);
        else if (!strcmp(k, "gw"))
            snprintf(c->gw, sizeof(c->gw), "%s", v);
        else if (!strcmp(k, "udp_relay")) {
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
    }
    fclose(f);
    if (!c->host[0] || c->port <= 0 || c->port > 65535 || !c->password[0]) {
        fprintf(stderr, "%s: host, port, session (or id_from) and password are required\n", full);
        return -1;
    }
    if (strcmp(c->auth, "xor") != 0) {
        fprintf(stderr, "%s: unsupported auth '%s' (only xor)\n", full, c->auth);
        return -1;
    }
    return 0;
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


#if VTESSE_WITH_LWIP

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
#if VTESSE_WITH_LWIP
static struct {
    uint16_t port, local;
    nc_udp_relay_t *r;
} relays[NC_UDP_RELAY_MAX];
static int nrelays;
#endif

static void print_info(void)
{
    nc_stack_info_t in;
    char a[16], m[16], g[16], r[48];
    int i;

    nc_stack_get_info(&in);
    nc_ip4_format(in.addr, a, sizeof(a));
    nc_ip4_format(in.mask, m, sizeof(m));
    nc_ip4_format(in.gw, g, sizeof(g));
    if (manual_ip)
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

#endif /* VTESSE_WITH_LWIP */

static void print_tunnel_event(int tev, int err)
{
    char flags[64];

    if (tev == TUNNEL_EV_UP) {
        vt_flags_format(vt_session_flags(vt), flags, sizeof(flags));
        printf("[%8.3f] up: %s, flags %s\n", elapsed(t_start), vt_greeting(vt), flags);
    } else if (err) {
        printf("[%8.3f] %s: %s\n", elapsed(t_start), tunnel_event_name(tev), tunnel_strerror(err));
    } else {
        printf("[%8.3f] %s\n", elapsed(t_start), tunnel_event_name(tev));
    }
    if (tev == TUNNEL_EV_FAILED)
        loop.stop = 1;
}

#if VTESSE_WITH_TAP
/* ---- TAP backend: hook script ---- */

static const char *hook;
static const char *session_name;
static nc_tap_t *tap;

static void reap_children(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* Hooks still running, by PID: as PID 1 (in a container) we also inherit the DHCP
 * client the hook daemonized, so waiting for "any child" would never return. */
static pid_t hook_pids[16];

/* 1: the hook has exited (or was already reaped), 0: still running. EINTR is
 * retried, so an interrupted check never counts as an exit. */
static int hook_done(pid_t pid)
{
    pid_t r;

    while ((r = waitpid(pid, NULL, WNOHANG)) < 0 && errno == EINTR)
        ;
    return r != 0;      /* pid, or -1 with ECHILD */
}

/* Waits for the hooks started so far: "stop" must not overtake an earlier "up" that
 * has yet to start the DHCP client (it would be left running after we exit). A hook
 * still running after HOOK_WAIT_MS is terminated, so shutdown cannot hang on it. */
#define HOOK_WAIT_MS 5000

static void wait_hooks(void)
{
    uint64_t deadline = nc_now_ms() + HOOK_WAIT_MS;
    size_t i;

    for (i = 0; i < sizeof(hook_pids) / sizeof(hook_pids[0]); i++) {
        while (hook_pids[i] > 0) {
            if (hook_done(hook_pids[i])) {
                hook_pids[i] = 0;               /* finished, or already reaped */
            } else if (nc_now_ms() >= deadline) {
                uint64_t grace = nc_now_ms() + 1000;
                printf("[%8.3f] hook: pid %d still running, terminated\n", elapsed(t_start),
                       (int)hook_pids[i]);
                /* the whole group: also a udhcpc still in the hook's foreground */
                kill(-hook_pids[i], SIGTERM);
                while (!hook_done(hook_pids[i]) && nc_now_ms() < grace) {
                    struct timespec ts = { 0, 20 * 1000000L };
                    nanosleep(&ts, NULL);
                }
                kill(-hook_pids[i], SIGKILL);
                while (waitpid(hook_pids[i], NULL, 0) < 0 && errno == EINTR)
                    ;
                hook_pids[i] = 0;
            } else {
                struct timespec ts = { 0, 20 * 1000000L };
                nanosleep(&ts, NULL);
            }
        }
    }
}

/* Runs "$hook <event>" asynchronously with VTESSE_* in the environment. */
static void run_hook(const char *event)
{
    pid_t pid;
    size_t slot;

    reap_children();
    if (!hook)
        return;
    /* a free slot first: every hook we start must be one shutdown can wait for */
    for (slot = 0; slot < sizeof(hook_pids) / sizeof(hook_pids[0]); slot++)
        if (hook_pids[slot] <= 0 || hook_done(hook_pids[slot]))
            break;
    if (slot == sizeof(hook_pids) / sizeof(hook_pids[0])) {
        printf("[%8.3f] hook: too many hooks still running, \"%s\" not started\n",
               elapsed(t_start), event);
        return;
    }
    pid = fork();
    if (pid == 0) {
        setpgid(0, 0);      /* own process group, so a stuck hook can be ended as a whole */
        setenv("VTESSE_EVENT", event, 1);
        setenv("VTESSE_IFNAME", nc_tap_ifname(tap), 1);
        setenv("VTESSE_LINK", nc_tap_link(tap) == TUNNEL_LINK_L3 ? "tun" : "ether", 1);
        setenv("VTESSE_SESSION", session_name, 1);
        execl("/bin/sh", "sh", hook, event, (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
        printf("[%8.3f] hook: fork failed: %s\n", elapsed(t_start), strerror(errno));
    else {
        setpgid(pid, pid);  /* also here: no race with a kill() before the child ran */
        hook_pids[slot] = pid;
    }
}

static void on_tap_event(void *user, int ev, int tev, int err)
{
    (void)user;
    switch (ev) {
    case NC_TAP_EV_CREATED:
        printf("[%8.3f] interface %s (%s) created\n", elapsed(t_start), nc_tap_ifname(tap),
               nc_tap_link(tap) == TUNNEL_LINK_L3 ? "tun" : "tap");
        if (!hook)
            printf("[%8.3f] no --hook: configure the address yourself (e.g. udhcpc -i %s)\n",
                   elapsed(t_start), nc_tap_ifname(tap));
        run_hook("up");
        break;
    case NC_TAP_EV_RECONNECTED:
        printf("[%8.3f] interface %s: carrier on\n", elapsed(t_start), nc_tap_ifname(tap));
        run_hook("reconnect");
        break;
    case NC_TAP_EV_ERROR:
        printf("[%8.3f] cannot create the interface: %s%s\n", elapsed(t_start), strerror(err),
               err == EPERM || err == EACCES ? " (needs root or CAP_NET_ADMIN)" :
               err == ENOENT || err == ENODEV ? " (no /dev/net/tun: load the tun module)" : "");
        loop.stop = 1;
        break;
    default:
        print_tunnel_event(tev, err);
        if (tev == TUNNEL_EV_DOWN && nc_tap_link(tap) != TUNNEL_LINK_NONE)
            run_hook("down");
        break;
    }
    fflush(stdout);
}
#endif /* VTESSE_WITH_TAP */


int main(int argc, char **argv)
{
    const char *config = DEFAULT_CONFIG, *id_from = NULL, *ip_arg = NULL, *gw_arg = NULL;
    uint32_t st_addr = 0, st_mask = 0, st_gw = 0;
    nc_routes_t st_routes;
    double seconds = 0;
    cfg_file_t c;
    vt_cfg_t vc;
    uint8_t mac[6];
    tunnel_t t;
    nc_source_t src;
    int i, err, rc, use_tap = !VTESSE_WITH_LWIP, lwip_opts = 0;
#if VTESSE_WITH_LWIP
    nc_stack_cfg_t sc;
    nc_proxy_cfg_t pc;
    nc_proxy_t *px = NULL;
#endif
#if VTESSE_WITH_TAP
    nc_tap_cfg_t tc;

    memset(&tc, 0, sizeof(tc));
#endif
#if VTESSE_WITH_LWIP
    memset(&sc, 0, sizeof(sc));
    memset(&pc, 0, sizeof(pc));
#endif
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
        else if (!strcmp(argv[i], "--id-from") && i + 1 < argc)
            id_from = argv[++i];
        else if (!strcmp(argv[i], "--version")) {
            printf("vtesse-cli %s (%s%s%s%s%s)\n", VTESSE_VERSION,
                   VTESSE_WITH_LWIP ? "lwip, forwards" : "",
                   VTESSE_WITH_LWIP && NC_WITH_SOCKS ? ", socks5" : "",
                   VTESSE_WITH_LWIP && VTESSE_WITH_UDP_RELAY ? ", udp-relay" : "",
                   VTESSE_WITH_LWIP && VTESSE_WITH_TAP ? ", " : "",
                   VTESSE_WITH_TAP ? "tap" : "");
            return 0;
        }
#if VTESSE_WITH_TAP
        else if (!strcmp(argv[i], "--tap"))
            use_tap = 1;
        else if (!strcmp(argv[i], "--ifname") && i + 1 < argc)
            tc.ifname = argv[++i];
        else if (!strcmp(argv[i], "--hook") && i + 1 < argc)
            hook = argv[++i];
#else
        else if (!strcmp(argv[i], "--tap") || !strcmp(argv[i], "--ifname") || !strcmp(argv[i], "--hook")) {
            fprintf(stderr, "%s: built without the TAP/TUN backend (make TAP=0)\n", argv[i]);
            return 2;
        }
#endif
#if VTESSE_WITH_LWIP
        else if (!strcmp(argv[i], "--fetch") && i + 1 < argc) {
            if (parse_url(argv[++i]) < 0) {
                fprintf(stderr, "--fetch: expected http://IPv4[:port]/path\n");
                return 2;
            }
            fetch.wanted = lwip_opts = 1;
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
            lwip_opts = 1;
        } else if (!strcmp(argv[i], "--forward") && i + 1 < argc) {
            if (pc.forward_count >= NC_PROXY_MAX_FORWARDS ||
                parse_forward(argv[++i], &pc.forwards[pc.forward_count]) < 0) {
                fprintf(stderr, "--forward: expected LPORT:A.B.C.D:RPORT (max %d)\n", NC_PROXY_MAX_FORWARDS);
                return 2;
            }
            pc.forward_count++;
            lwip_opts = 1;
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
            lwip_opts = 1;
        } else if (!strcmp(argv[i], "--listen") && i + 1 < argc) {
            pc.listen_addr = argv[++i];
            lwip_opts = 1;
        } else if ((!strcmp(argv[i], "--ip") || !strcmp(argv[i], "--debug-ip")) && i + 1 < argc) {
            ip_arg = argv[++i];
        } else if ((!strcmp(argv[i], "--gw") || !strcmp(argv[i], "--debug-gw")) && i + 1 < argc) {
            gw_arg = argv[++i];
        }
#endif
        else {
            usage();
            return 2;
        }
    }
    if (use_tap && lwip_opts) {
        fprintf(stderr, "--tap cannot be combined with --socks/--forward/--listen/--fetch/--udp-relay\n");
        return 2;
    }
    if (!VTESSE_WITH_TAP && use_tap) {
        fprintf(stderr, "built without any backend\n");
        return 2;
    }
    if (load_config(config, &c) < 0)
        return 2;
    if (c.udp_relay_count && !(VTESSE_WITH_LWIP && VTESSE_WITH_UDP_RELAY)) {
        fprintf(stderr, "%s: udp_relay: built without the UDP relay\n", config);
        return 2;
    }
    if (c.udp_relay_count && use_tap) {
        fprintf(stderr, "%s: udp_relay needs the lwIP backend (with --tap the kernel receives UDP itself)\n", config);
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
    if (c.ip[0] && parse_prefix(c.ip, &st_addr, &st_mask) < 0) {
        fprintf(stderr, "ip: expected A.B.C.D/P, got '%s'\n", c.ip);
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
    nc_loop_init(&loop);
    src = nc_source_from_tunnel(&t);
    nc_loop_add(&loop, &src);

#if VTESSE_WITH_TAP
    if (use_tap) {
        memcpy(tc.mac, mac, 6);
        /* manual IPv4 for the hook (it configures the interface) */
        if (c.ip[0]) {
            char routes[16 * 32 + 16] = "";

            for (i = 0; i < c.route_count; i++) {
                strcat(routes, i ? " " : "");
                strcat(routes, c.route[i]);
            }
            setenv("VTESSE_IP", c.ip, 1);
            if (c.gw[0])
                setenv("VTESSE_GW", c.gw, 1);
            if (routes[0])
                setenv("VTESSE_ROUTES", routes, 1);
        }
        session_name = c.session;
        tap = nc_tap_create(&t, &tc, on_tap_event, NULL);
        if (!tap) {
            fprintf(stderr, "out of memory\n");
            return 2;
        }
        src = nc_tap_source(tap);
        nc_loop_add(&loop, &src);
    }
#endif
#if VTESSE_WITH_LWIP
    if (!use_tap) {
        memcpy(sc.mac, mac, 6);
        if (c.ip[0]) {
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
            src = nc_proxy_source(px);
            nc_loop_add(&loop, &src);
        }
    }
#endif

    t_start = nc_now_ms();
    vt_start(vt);
    nc_loop_run(&loop, seconds > 0 ? t_start + (uint64_t)(seconds * 1000) : 0);

    rc = vt_is_up(vt) ? 0 : 1;
#if VTESSE_WITH_LWIP
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
#endif
#if VTESSE_WITH_TAP
    if (tap) {
        nc_tap_stats_t ts;
        nc_tap_get_stats(tap, &ts);
        printf("[%8.3f] interface %s: frames in %lu, out %lu, dropped %lu\n", elapsed(t_start),
               nc_tap_ifname(tap), ts.frames_in, ts.frames_out, ts.dropped);
        if (nc_tap_link(tap) == TUNNEL_LINK_NONE && vt_is_up(vt))
            rc = 1;     /* interface creation failed */
        wait_hooks();
        run_hook("stop");
        wait_hooks();
        nc_tap_destroy(tap);
    }
#endif
    t.ops->close(vt);
    vt_destroy(vt);
    return rc;
}
