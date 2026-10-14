/*
 * JNI bridge of the Android app.
 *
 * Runs the VTun client, the lwIP stack and the local proxy (SOCKS5, port
 * forwards on 127.0.0.1) in one native thread: the libraries are
 * single-threaded. Java talks to that thread through a pipe (stop, network
 * changed) and reads a status snapshot kept under a mutex.
 *
 * One session per process: lwIP keeps global state and is initialised once, so
 * the app runs the tunnel service in its own process and ends that process on
 * stop.
 *
 * Configuration: the key=value lines of the vtesse-cli config file (host, port,
 * session, password, ip, gw, route, udp_relay) plus socks_port,
 * forward=LPORT:IP:RPORT (repeatable) and listen.
 */
#include <jni.h>

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "netcore/loop.h"
#include "netcore/proxy.h"
#include "netcore/route.h"
#include "netcore/stack.h"
#include "netcore/udprelay.h"
#include "vtesse/version.h"
#include "vtesse/vtun_client.h"

#define TAG          "vtesse"
#define LOG_LINES    40
#define LOG_LINE_MAX 160

enum { ST_IDLE, ST_CONNECTING, ST_UP, ST_DOWN, ST_FAILED, ST_STOPPED };
static const char *const state_names[] = { "idle", "connecting", "up", "down", "failed", "stopped" };

/* Shared with the Java threads; everything below the mutex. */
static struct {
    pthread_mutex_t mu;
    int state;
    int bound;
    char addr[16], mask[16], gw[16];
    char routes[NC_ROUTES_MAX][48];
    int route_count;
    char flags[32];
    char error[128];
    nc_proxy_stats_t ps;
    nc_udp_relay_stats_t us[NC_UDP_RELAY_MAX];
    char log[LOG_LINES][LOG_LINE_MAX];
    int log_next, log_count;
} S = { .mu = PTHREAD_MUTEX_INITIALIZER };

/* Owned by the loop thread once it runs. */
static pthread_t loop_thread;
static int started;
static int cmd_pipe[2] = { -1, -1 };
static nc_loop_t loop;
static vt_t *vt;
static nc_proxy_t *px;
static nc_udp_relay_t *relay[NC_UDP_RELAY_MAX];
static uint64_t t_start, next_stats;

static void logf_(int prio, const char *fmt, ...)
{
    char msg[LOG_LINE_MAX - 12];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    __android_log_print(prio, TAG, "%s", msg);
    pthread_mutex_lock(&S.mu);
    snprintf(S.log[S.log_next], LOG_LINE_MAX, "[%7.1f] %s",
             t_start ? (double)(nc_now_ms() - t_start) / 1000.0 : 0.0, msg);
    S.log_next = (S.log_next + 1) % LOG_LINES;
    if (S.log_count < LOG_LINES)
        S.log_count++;
    pthread_mutex_unlock(&S.mu);
}

static void set_state(int state, int err)
{
    pthread_mutex_lock(&S.mu);
    S.state = state;
    if (err)
        snprintf(S.error, sizeof(S.error), "%s", tunnel_strerror(err));
    else if (state == ST_UP)
        S.error[0] = '\0';
    pthread_mutex_unlock(&S.mu);
}

/* ---- events (loop thread) ---- */

static void on_bound(void)
{
    nc_stack_info_t in;
    int i;

    nc_stack_get_info(&in);
    pthread_mutex_lock(&S.mu);
    S.bound = 1;
    nc_ip4_format(in.addr, S.addr, sizeof(S.addr));
    nc_ip4_format(in.mask, S.mask, sizeof(S.mask));
    nc_ip4_format(in.gw, S.gw, sizeof(S.gw));
    S.route_count = in.routes.count;
    for (i = 0; i < in.routes.count; i++)
        nc_route_format(&in.routes.r[i], S.routes[i], sizeof(S.routes[i]));
    pthread_mutex_unlock(&S.mu);
    logf_(ANDROID_LOG_INFO, "address %s mask %s%s", S.addr, S.mask,
          in.default_route ? " (default route)" : "");
    for (i = 0; i < in.routes.count; i++)
        logf_(ANDROID_LOG_INFO, "  route %s", S.routes[i]);
}

static void on_stack_event(void *user, int ev, int tev, int err)
{
    (void)user;
    if (ev == NC_STACK_EV_ADDR_BOUND) {
        on_bound();
    } else if (ev == NC_STACK_EV_ADDR_LOST) {
        pthread_mutex_lock(&S.mu);
        S.bound = 0;
        pthread_mutex_unlock(&S.mu);
        logf_(ANDROID_LOG_INFO, "address lost");
    } else if (ev == NC_STACK_EV_TUNNEL) {
        switch (tev) {
        case TUNNEL_EV_CONNECTING:
            set_state(ST_CONNECTING, 0);
            logf_(ANDROID_LOG_INFO, "connecting");
            break;
        case TUNNEL_EV_UP: {
            char flags[32];

            vt_flags_format(vt_session_flags(vt), flags, sizeof(flags));
            pthread_mutex_lock(&S.mu);
            snprintf(S.flags, sizeof(S.flags), "%s", flags);
            pthread_mutex_unlock(&S.mu);
            set_state(ST_UP, 0);
            logf_(ANDROID_LOG_INFO, "up: %s, flags %s", vt_greeting(vt), flags);
            break;
        }
        case TUNNEL_EV_DOWN:
            set_state(ST_DOWN, err);
            logf_(ANDROID_LOG_WARN, "down: %s", tunnel_strerror(err));
            break;
        case TUNNEL_EV_FAILED:
            set_state(ST_FAILED, err);
            logf_(ANDROID_LOG_ERROR, "failed: %s", tunnel_strerror(err));
            break;
        }
    }
}

static void on_proxy_log(void *user, const char *msg)
{
    (void)user;
    logf_(ANDROID_LOG_INFO, "proxy: %s", msg);
}

/* ---- command pipe and stats timer (loop sources) ---- */

static int cmd_get_fd(void *ctx)
{
    (void)ctx;
    return cmd_pipe[0];
}

static void cmd_on_readable(void *ctx)
{
    char buf[16];
    ssize_t n, i;

    (void)ctx;
    while ((n = read(cmd_pipe[0], buf, sizeof(buf))) > 0)
        for (i = 0; i < n; i++) {
            if (buf[i] == 'q')
                loop.stop = 1;
            else if (buf[i] == 'n') {
                logf_(ANDROID_LOG_INFO, "network changed");
                vt_network_changed(vt);
            }
        }
}

static int stats_next(void *ctx)
{
    uint64_t now = nc_now_ms();

    (void)ctx;
    return now >= next_stats ? 0 : (int)(next_stats - now);
}

static void stats_on_timer(void *ctx)
{
    (void)ctx;
    next_stats = nc_now_ms() + 1000;
    pthread_mutex_lock(&S.mu);
    if (px)
        nc_proxy_get_stats(px, &S.ps);
    for (int i = 0; i < NC_UDP_RELAY_MAX; i++)
        if (relay[i])
            nc_udp_relay_get_stats(relay[i], &S.us[i]);
    pthread_mutex_unlock(&S.mu);
}

static void *loop_main(void *arg)
{
    tunnel_t t = vt_tunnel(vt);

    (void)arg;
    vt_start(vt);
    nc_loop_run(&loop, 0);

    t.ops->close(t.ctx);            /* CONN_CLOSE to the server */
    if (px)
        nc_proxy_destroy(px);
    px = NULL;
    for (int i = 0; i < NC_UDP_RELAY_MAX; i++) {
        nc_udp_relay_destroy(relay[i]);
        relay[i] = NULL;
    }
    vt_destroy(vt);
    vt = NULL;
    set_state(ST_STOPPED, 0);
    logf_(ANDROID_LOG_INFO, "stopped");
    return NULL;
}

/* ---- configuration ---- */

static int parse_ip4(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;

    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return -1;
    *out = a << 24 | b << 16 | c << 8 | d;
    return 0;
}

static int parse_prefix(const char *s, uint32_t *addr, int *prefix)
{
    char buf[32];
    const char *slash = strchr(s, '/');

    if (!slash || (size_t)(slash - s) >= sizeof(buf))
        return -1;
    memcpy(buf, s, (size_t)(slash - s));
    buf[slash - s] = '\0';
    *prefix = atoi(slash + 1);
    return *prefix < 0 || *prefix > 32 || parse_ip4(buf, addr) < 0 ? -1 : 0;
}

static int parse_forward(const char *s, nc_forward_t *f)
{
    unsigned lp, rp;
    char ip[32];

    if (sscanf(s, "%u:%31[0-9.]:%u", &lp, ip, &rp) != 3 || !lp || lp > 65535 || !rp || rp > 65535)
        return -1;
    f->local_port = (uint16_t)lp;
    f->port = (uint16_t)rp;
    return parse_ip4(ip, &f->ip);
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

typedef struct app_cfg {
    char host[256], session[64], password[128], listen[64];
    int port, socks_port;
    uint16_t relay_port[NC_UDP_RELAY_MAX], relay_local[NC_UDP_RELAY_MAX];
    int nrelays;
    vt_cfg_t vc;
    nc_stack_cfg_t sc;
    nc_proxy_cfg_t pc;
} app_cfg_t;

static app_cfg_t cfg;   /* strings referenced by vt (copied) and the proxy (kept) */

/* Fills cfg from key=value text. NULL on success, else a message. */
static const char *parse_config(char *text)
{
    static char msg[128];
    char *line, *save = NULL;

    memset(&cfg, 0, sizeof(cfg));
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *k = trim(line), *v = strchr(k, '=');

        if (*k == '#' || !v)
            continue;
        *v++ = '\0';
        k = trim(k);
        v = trim(v);
        if (!strcmp(k, "host"))
            snprintf(cfg.host, sizeof(cfg.host), "%s", v);
        else if (!strcmp(k, "port"))
            cfg.port = atoi(v);
        else if (!strcmp(k, "session"))
            snprintf(cfg.session, sizeof(cfg.session), "%s", v);
        else if (!strcmp(k, "password"))
            snprintf(cfg.password, sizeof(cfg.password), "%s", v);
        else if (!strcmp(k, "listen"))
            snprintf(cfg.listen, sizeof(cfg.listen), "%s", v);
        else if (!strcmp(k, "socks_port"))
            cfg.socks_port = atoi(v);
        else if (!strcmp(k, "forward")) {
            if (cfg.pc.forward_count >= NC_PROXY_MAX_FORWARDS ||
                parse_forward(v, &cfg.pc.forwards[cfg.pc.forward_count]) < 0) {
                snprintf(msg, sizeof(msg), "bad forward '%s' (LPORT:A.B.C.D:RPORT, max %d)", v,
                         NC_PROXY_MAX_FORWARDS);
                return msg;
            }
            cfg.pc.forward_count++;
        } else if (!strcmp(k, "udp_relay")) {
            if (cfg.nrelays >= NC_UDP_RELAY_MAX ||
                nc_udp_relay_parse(v, &cfg.relay_port[cfg.nrelays], &cfg.relay_local[cfg.nrelays]) < 0) {
                snprintf(msg, sizeof(msg), "bad udp_relay '%s' (PORT[:LOCAL_PORT], max %d)", v, NC_UDP_RELAY_MAX);
                return msg;
            }
            cfg.nrelays++;
        } else if (!strcmp(k, "ip")) {
            int p;

            if (parse_prefix(v, &cfg.sc.addr, &p) < 0)
                return "bad ip (A.B.C.D/P)";
            cfg.sc.mask = p ? 0xffffffffu << (32 - p) : 0;
            cfg.sc.static_ip = 1;
        } else if (!strcmp(k, "gw")) {
            if (parse_ip4(v, &cfg.sc.gw) < 0)
                return "bad gw (A.B.C.D)";
        } else if (!strcmp(k, "route")) {
            uint32_t net;
            int p;

            if (parse_prefix(v, &net, &p) < 0 || nc_routes_add(&cfg.sc.routes, net, (uint8_t)p, 0) < 0)
                return "bad route (NET/P)";
        }
    }
    if (!cfg.host[0] || cfg.port <= 0 || cfg.port > 65535)
        return "server and port are required";
    if (nc_devid_to_mac(cfg.session, cfg.sc.mac) < 0)
        return "device ID: 12 characters 0-9A-F, unicast";
    if (!cfg.password[0])
        return "password is required";
    if (cfg.socks_port < 0 || cfg.socks_port > 65535)
        return "bad SOCKS5 port";
    if (!cfg.socks_port && !cfg.pc.forward_count && !cfg.nrelays)
        return "nothing to do: no SOCKS5, port forward or UDP relay";
    /* routes added before gw= is known: set their gateway now */
    {
        int i;

        for (i = 0; i < cfg.sc.routes.count; i++)
            cfg.sc.routes.r[i].gw = cfg.sc.gw;
    }
    return NULL;
}

/* ---- JNI ---- */

static jstring jerr(JNIEnv *env, const char *msg)
{
    return (*env)->NewStringUTF(env, msg);
}

JNIEXPORT jstring JNICALL
Java_org_openipc_vtesse_Native_start(JNIEnv *env, jclass cls, jstring jconfig)
{
    const char *utf;
    char *text;
    const char *msg;
    nc_source_t src;
    tunnel_t t;
    int err;

    (void)cls;
    if (started)
        return jerr(env, "already started in this process");
    utf = (*env)->GetStringUTFChars(env, jconfig, NULL);
    text = utf ? strdup(utf) : NULL;
    if (utf)
        (*env)->ReleaseStringUTFChars(env, jconfig, utf);
    if (!text)
        return jerr(env, "out of memory");
    t_start = nc_now_ms();
    msg = parse_config(text);
    memset(text, 0, strlen(text));      /* password */
    free(text);
    if (msg)
        return jerr(env, msg);

    cfg.vc.host = cfg.host;
    cfg.vc.port = (uint16_t)cfg.port;
    cfg.vc.session = cfg.session;
    cfg.vc.password = cfg.password;
    cfg.vc.auth = VT_AUTH_XOR;
    vt = vt_create(&cfg.vc, &err);
    memset(cfg.password, 0, sizeof(cfg.password));
    if (!vt)
        return jerr(env, tunnel_strerror(err));
    t = vt_tunnel(vt);

    nc_loop_init(&loop);
    src = nc_source_from_tunnel(&t);
    nc_loop_add(&loop, &src);
    if (nc_stack_init(&t, &cfg.sc, on_stack_event, NULL) < 0) {
        vt_destroy(vt);
        return jerr(env, "stack initialisation failed");
    }
    src = nc_stack_timer_source();
    nc_loop_add(&loop, &src);

    {
        int i;
        char rmsg[128];

        for (i = 0; i < cfg.nrelays; i++) {
            relay[i] = nc_udp_relay_create(cfg.relay_port[i], NULL, cfg.relay_local[i], rmsg, sizeof(rmsg));
            if (!relay[i]) {
                vt_destroy(vt);
                return jerr(env, rmsg);
            }
            logf_(ANDROID_LOG_INFO, "udp relay :%u -> 127.0.0.1:%u", (unsigned)cfg.relay_port[i],
                  (unsigned)cfg.relay_local[i]);
        }
    }

    cfg.pc.listen_addr = cfg.listen[0] ? cfg.listen : "127.0.0.1";
    cfg.pc.socks_port = (uint16_t)cfg.socks_port;
    if (cfg.socks_port || cfg.pc.forward_count) {
        px = nc_proxy_create(&cfg.pc, on_proxy_log, NULL);
        if (!px) {
            vt_destroy(vt);
            return jerr(env, "cannot open the local ports (in use?)");
        }
        src = nc_proxy_source(px);
        nc_loop_add(&loop, &src);
    }

    if (pipe(cmd_pipe) < 0) {
        nc_proxy_destroy(px);
        vt_destroy(vt);
        return jerr(env, strerror(errno));
    }
    fcntl(cmd_pipe[0], F_SETFL, fcntl(cmd_pipe[0], F_GETFL) | O_NONBLOCK);
    memset(&src, 0, sizeof(src));
    src.get_fd = cmd_get_fd;
    src.on_readable = cmd_on_readable;
    nc_loop_add(&loop, &src);
    memset(&src, 0, sizeof(src));
    src.next_timeout_ms = stats_next;
    src.on_timer = stats_on_timer;
    nc_loop_add(&loop, &src);

    if (cfg.socks_port)
        logf_(ANDROID_LOG_INFO, "socks5 on %s:%d", cfg.pc.listen_addr, cfg.socks_port);
    {
        int i;

        for (i = 0; i < cfg.pc.forward_count; i++) {
            char ip[16];

            nc_ip4_format(cfg.pc.forwards[i].ip, ip, sizeof(ip));
            logf_(ANDROID_LOG_INFO, "forward %s:%u -> %s:%u", cfg.pc.listen_addr,
                  (unsigned)cfg.pc.forwards[i].local_port, ip, (unsigned)cfg.pc.forwards[i].port);
        }
    }
    set_state(ST_CONNECTING, 0);
    if (pthread_create(&loop_thread, NULL, loop_main, NULL) != 0) {
        nc_proxy_destroy(px);
        vt_destroy(vt);
        return jerr(env, "cannot start the network thread");
    }
    started = 1;
    return NULL;
}

static void send_cmd(char c)
{
    if (started && cmd_pipe[1] >= 0)
        (void)!write(cmd_pipe[1], &c, 1);
}

JNIEXPORT void JNICALL
Java_org_openipc_vtesse_Native_stop(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    if (!started)
        return;
    send_cmd('q');
    pthread_join(loop_thread, NULL);
    started = 0;
}

JNIEXPORT void JNICALL
Java_org_openipc_vtesse_Native_networkChanged(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    send_cmd('n');
}

/* key=value lines; "route" and "log" repeat. */
JNIEXPORT jstring JNICALL
Java_org_openipc_vtesse_Native_status(JNIEnv *env, jclass cls)
{
    static char buf[16384];
    size_t n = 0;
    int i;

    (void)cls;
#define PUT(...) do { if (n < sizeof(buf)) n += (size_t)snprintf(buf + n, sizeof(buf) - n, __VA_ARGS__); } while (0)
    pthread_mutex_lock(&S.mu);
    PUT("state=%s\n", state_names[S.state]);
    PUT("bound=%d\n", S.bound);
    if (S.bound) {
        PUT("addr=%s\nmask=%s\ngw=%s\n", S.addr, S.mask, S.gw);
        for (i = 0; i < S.route_count; i++)
            PUT("route=%s\n", S.routes[i]);
    }
    if (S.flags[0])
        PUT("flags=%s\n", S.flags);
    if (S.error[0])
        PUT("error=%s\n", S.error);
    PUT("accepted=%lu\nactive=%lu\nfailed=%lu\nrejected=%lu\nup=%llu\ndown=%llu\n",
        S.ps.accepted, S.ps.active, S.ps.failed, S.ps.rejected, S.ps.bytes_up, S.ps.bytes_down);
    /* udp=PORT,LOCAL_PORT,datagrams,bytes,dropped */
    for (i = 0; i < cfg.nrelays && i < NC_UDP_RELAY_MAX; i++)
        PUT("udp=%u,%u,%lu,%llu,%lu\n", (unsigned)cfg.relay_port[i], (unsigned)cfg.relay_local[i],
            S.us[i].datagrams, S.us[i].bytes, S.us[i].dropped);
    for (i = 0; i < S.log_count; i++)
        PUT("log=%s\n", S.log[(S.log_next - S.log_count + i + LOG_LINES) % LOG_LINES]);
    pthread_mutex_unlock(&S.mu);
#undef PUT
    if (n >= sizeof(buf))
        buf[sizeof(buf) - 1] = '\0';
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_org_openipc_vtesse_Native_version(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, VTESSE_VERSION);
}

/* Device ID check for the settings screen: NULL if valid, else a message. */
JNIEXPORT jstring JNICALL
Java_org_openipc_vtesse_Native_checkDeviceId(JNIEnv *env, jclass cls, jstring jid)
{
    const char *id = (*env)->GetStringUTFChars(env, jid, NULL);
    uint8_t mac[6];
    int ok = id && nc_devid_to_mac(id, mac) == 0;

    (void)cls;
    if (id)
        (*env)->ReleaseStringUTFChars(env, jid, id);
    return ok ? NULL : jerr(env, "12 characters 0-9A-F, first byte unicast");
}
