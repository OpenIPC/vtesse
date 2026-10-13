/*
 * vtesse-cli: Linux console client.
 *
 * Stage 1, step 1: connects to a VTun server, keeps the session alive and
 * reports link events and received traffic. The userspace stack (lwIP, DHCP)
 * is added on top of this in the next steps.
 *
 * Credentials are read from a key=value file, never from the command line.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcore/loop.h"
#include "vtesse/vtun_client.h"

#define DEFAULT_CONFIG "~/.config/vtesse/probe.conf"

typedef struct cfg_file {
    char host[256];
    char session[64];
    char password[128];
    char auth[16];
    int port;
} cfg_file_t;

typedef struct stats {
    unsigned long frames, bytes, ipv4, arp, ipv6, other;
} stats_t;

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
            "  FILE: key=value lines: host, port, session, password, auth (xor)\n"
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

static void on_event(void *user, int ev, int err)
{
    char flags[64];

    (void)user;
    if (ev == TUNNEL_EV_UP) {
        vt_flags_format(vt_session_flags(vt), flags, sizeof(flags));
        printf("[%8.3f] up: %s, flags %s\n", elapsed(t_start), vt_greeting(vt), flags);
    } else if (err) {
        printf("[%8.3f] %s: %s\n", elapsed(t_start), tunnel_event_name(ev), tunnel_strerror(err));
    } else {
        printf("[%8.3f] %s\n", elapsed(t_start), tunnel_event_name(ev));
    }
    if (ev == TUNNEL_EV_FAILED)
        loop.stop = 1;
    fflush(stdout);
}

static void on_rx(void *user, const uint8_t *p, size_t n)
{
    stats_t *s = user;
    uint16_t type = n >= 14 ? (uint16_t)((p[12] << 8) | p[13]) : 0;

    s->frames++;
    s->bytes += n;
    if (vt_session_flags(vt)->type == 'u')
        s->ipv4++;
    else if (type == 0x0800)
        s->ipv4++;
    else if (type == 0x0806)
        s->arp++;
    else if (type == 0x86dd)
        s->ipv6++;
    else
        s->other++;
    if (verbose)
        printf("[%8.3f] rx %zu bytes, type 0x%04x\n", elapsed(t_start), n, type);
}

int main(int argc, char **argv)
{
    const char *config = DEFAULT_CONFIG;
    double seconds = 0;
    cfg_file_t c;
    vt_cfg_t vc;
    stats_t st;
    tunnel_t t;
    nc_source_t src;
    int i, err;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            config = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else {
            usage();
            return 2;
        }
    }
    if (load_config(config, &c) < 0)
        return 2;

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

    memset(&st, 0, sizeof(st));
    t = vt_tunnel(vt);
    t.ops->set_event_cb(vt, on_event, NULL);
    t.ops->set_rx_cb(vt, on_rx, &st);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    nc_loop_init(&loop);
    src = nc_source_from_tunnel(&t);
    nc_loop_add(&loop, &src);

    t_start = nc_now_ms();
    vt_start(vt);
    nc_loop_run(&loop, seconds > 0 ? t_start + (uint64_t)(seconds * 1000) : 0);

    printf("[%8.3f] rx frames %lu (%lu bytes): ipv4 %lu, arp %lu, ipv6 %lu, other %lu\n",
           elapsed(t_start), st.frames, st.bytes, st.ipv4, st.arp, st.ipv6, st.other);
    err = vt_is_up(vt) ? 0 : 1;
    t.ops->close(vt);
    vt_destroy(vt);
    return err;
}
