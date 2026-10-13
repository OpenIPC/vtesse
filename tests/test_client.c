/*
 * vtun_client scenarios against a scripted VTun server running in a child
 * process on a loopback socket.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "netcore/loop.h"
#include "unit.h"
#include "vtesse/vtun_client.h"

#define PASSWORD "testpassword-0123456789"
#define SESSION  "02BE0C000001"

static const uint8_t server_chal[VT_CHAL_SIZE] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};

/* ---------- scripted server (child process, blocking I/O) ---------- */

static void die(const char *what)
{
    fprintf(stderr, "server: %s\n", what);
    _exit(1);
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void srv_write(int fd, const void *p, size_t n)
{
    if (write(fd, p, n) != (ssize_t)n)
        die("write");
}

static void srv_read(int fd, void *p, size_t n)
{
    size_t got = 0;

    while (got < n) {
        ssize_t r = read(fd, (uint8_t *)p + got, n - got);
        if (r <= 0)
            die("read");
        got += (size_t)r;
    }
}

static void srv_mesg(int fd, const char *text)
{
    uint8_t m[VT_MESG_SIZE];

    memset(m, 0, sizeof(m));
    memcpy(m, text, strlen(text));
    srv_write(fd, m, sizeof(m));
}

static void srv_flag(int fd, uint16_t flag)
{
    uint8_t h[2];

    vt_hdr_put(h, flag);
    srv_write(fd, h, 2);
}

/* Full handshake; final_reply is sent after a correct CHAL response. */
static void srv_handshake(int fd, const char *final_reply)
{
    uint8_t m[VT_MESG_SIZE], expect[VT_CHAL_SIZE], got[VT_CHAL_SIZE];
    char text[VT_CHAL_TEXT_SIZE], line[VT_MESG_SIZE + 16];
    int i;

    srv_mesg(fd, "VTUN server ver 3.X 10/13/2026\n");
    srv_read(fd, m, sizeof(m));
    if (memcmp(m, "HOST: " SESSION "\n", 19) != 0)
        die("bad HOST");
    for (i = 19; i < VT_MESG_SIZE; i++)
        if (m[i])
            die("HOST not NUL padded");
    vt_chal_encode(server_chal, text);
    snprintf(line, sizeof(line), "OK CHAL: %s\n", text);
    srv_mesg(fd, line);
    srv_read(fd, m, sizeof(m));
    m[VT_MESG_SIZE - 1] = 0;
    if (strncmp((char *)m, "CHAL: ", 6) != 0 || vt_chal_decode((char *)m, got) != 0)
        die("bad CHAL");
    memcpy(expect, server_chal, sizeof(expect));
    vt_chal_xor(expect, PASSWORD);
    if (memcmp(expect, got, sizeof(got)) != 0)
        die("wrong XOR response");
    srv_mesg(fd, final_reply);
}

/* Read frames until a data frame arrives; flag frames are recorded. */
static size_t srv_read_data(int fd, uint8_t *buf, size_t cap, int *seen_req, int *seen_rep)
{
    for (;;) {
        uint8_t h[2];
        uint16_t hdr, len;

        srv_read(fd, h, 2);
        hdr = vt_hdr_get(h);
        if (hdr & VT_FLAGS_MASK) {
            if (hdr & VT_ECHO_REQ)
                (*seen_req)++;
            if (hdr & VT_ECHO_REP)
                (*seen_rep)++;
            continue;
        }
        len = hdr & VT_FSIZE_MASK;
        if (len > cap)
            die("frame too large");
        srv_read(fd, buf, len);
        return len;
    }
}

typedef void (*script_fn)(int listen_fd);

static pid_t start_server(script_fn script, uint16_t *port)
{
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    int one = 1, lfd = socket(AF_INET, SOCK_STREAM, 0);
    pid_t pid;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lfd, 4) < 0 ||
        getsockname(lfd, (struct sockaddr *)&a, &l) < 0) {
        perror("listen");
        exit(1);
    }
    *port = ntohs(a.sin_port);
    pid = fork();
    if (pid == 0) {
        alarm(10);
        script(lfd);
        _exit(0);
    }
    close(lfd);
    return pid;
}

static int server_status(pid_t pid)
{
    int st = 0;

    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

/* ---------- client side ---------- */

typedef struct rec {
    int events[32];
    int errs[32];
    int nev;
    uint8_t rx[8][64];
    size_t rxlen[8];
    int nrx;
} rec_t;

static void on_event(void *user, int ev, int err)
{
    rec_t *r = user;

    if (r->nev < 32) {
        r->events[r->nev] = ev;
        r->errs[r->nev++] = err;
    }
}

static void on_rx(void *user, const uint8_t *p, size_t n)
{
    rec_t *r = user;

    if (r->nrx < 8 && n <= 64) {
        memcpy(r->rx[r->nrx], p, n);
        r->rxlen[r->nrx++] = n;
    }
}

static int last_event(const rec_t *r)
{
    return r->nev ? r->events[r->nev - 1] : 0;
}

static vt_t *make_client(uint16_t port, rec_t *r, int no_reconnect)
{
    vt_cfg_t cfg;
    vt_t *vt;
    int err;
    tunnel_t t;

    memset(&cfg, 0, sizeof(cfg));
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.session = SESSION;
    cfg.password = PASSWORD;
    cfg.auth = VT_AUTH_XOR;
    cfg.handshake_timeout_ms = 400;
    cfg.ka_interval_ms = 200;
    cfg.ka_count = 3;
    cfg.data_delay_ms = 200;
    cfg.reconnect_min_ms = 100;
    cfg.no_reconnect = no_reconnect;
    vt = vt_create(&cfg, &err);
    if (!vt) {
        fprintf(stderr, "vt_create: %s\n", tunnel_strerror(err));
        exit(1);
    }
    memset(r, 0, sizeof(*r));
    t = vt_tunnel(vt);
    t.ops->set_event_cb(vt, on_event, r);
    t.ops->set_rx_cb(vt, on_rx, r);
    return vt;
}

/* Run the loop until pred() is true or timeout_ms passes. */
static int run_until(vt_t *vt, int (*pred)(vt_t *, rec_t *), rec_t *r, int timeout_ms)
{
    nc_loop_t loop;
    tunnel_t t = vt_tunnel(vt);
    nc_source_t s = nc_source_from_tunnel(&t);
    uint64_t end = nc_now_ms() + (uint64_t)timeout_ms;

    nc_loop_init(&loop);
    nc_loop_add(&loop, &s);
    while (nc_now_ms() < end) {
        if (pred(vt, r))
            return 1;
        nc_loop_run_once(&loop, 20);
    }
    return pred(vt, r);
}

static int pred_up(vt_t *vt, rec_t *r) { (void)r; return vt_is_up(vt); }
static int pred_failed(vt_t *vt, rec_t *r) { (void)vt; return last_event(r) == TUNNEL_EV_FAILED; }
static int pred_rx2(vt_t *vt, rec_t *r) { (void)vt; return r->nrx >= 2; }
static int pred_ups2(vt_t *vt, rec_t *r)
{
    int i, n = 0;

    (void)vt;
    for (i = 0; i < r->nev; i++)
        n += r->events[i] == TUNNEL_EV_UP;
    return n >= 2;
}

/* ---------- scenarios ---------- */

static void script_happy(int lfd)
{
    int fd = accept(lfd, NULL, NULL), req = 0, rep = 0;
    uint8_t buf[64], big[3000];
    size_t n;

    srv_handshake(fd, "OK FLAGS: <TeK>\n");
    srv_flag(fd, VT_ECHO_REQ);
    /* data frame split across two writes */
    srv_write(fd, "\x00\x05", 2);
    sleep_ms(30);
    srv_write(fd, "hello", 5);
    /* oversized frame (3000 > 2148) must be skipped, framing preserved */
    memset(big, 0xab, sizeof(big));
    srv_write(fd, "\x0b\xb8", 2);
    srv_write(fd, big, sizeof(big));
    srv_write(fd, "\x00\x05" "after", 7);
    /* client data frame after the data delay */
    n = srv_read_data(fd, buf, sizeof(buf), &req, &rep);
    if (n != 4 || memcmp(buf, "ping", 4) != 0)
        die("bad client frame");
    if (req < 1 || rep < 1)
        die("missing ECHO_REQ/ECHO_REP from client");
    srv_flag(fd, VT_CONN_CLOSE);
    sleep_ms(100);
    close(fd);
}

static void test_happy_path(void)
{
    uint16_t port;
    pid_t pid = start_server(script_happy, &port);
    rec_t r;
    vt_t *vt = make_client(port, &r, 1);
    tunnel_t t = vt_tunnel(vt);
    uint64_t t0;

    t0 = nc_now_ms();
    vt_start(vt);
    CHECK_EQ_INT(t.ops->send(vt, (const uint8_t *)"early", 5), -1);   /* not up */
    /* frames are delivered while UP is still held back by the data delay */
    CHECK(run_until(vt, pred_rx2, &r, 2000));
    CHECK(!vt_is_up(vt));
    CHECK_EQ_INT(t.ops->send(vt, (const uint8_t *)"early", 5), -1);
    CHECK_EQ_INT(r.nrx, 2);
    CHECK(r.rxlen[0] == 5 && memcmp(r.rx[0], "hello", 5) == 0);
    CHECK(r.rxlen[1] == 5 && memcmp(r.rx[1], "after", 5) == 0);

    CHECK(run_until(vt, pred_up, &r, 2000));
    CHECK(nc_now_ms() - t0 >= 190);                                    /* data delay */
    CHECK_EQ_INT(t.ops->link_type(vt), TUNNEL_LINK_L2);
    CHECK_EQ_STR(vt_greeting(vt), "VTUN server ver 3.X 10/13/2026");
    CHECK_EQ_INT(vt_session_flags(vt)->type, 'e');
    CHECK_EQ_INT(t.ops->send(vt, (const uint8_t *)"ping", 4), 0);
    CHECK(run_until(vt, pred_failed, &r, 2000));
    CHECK_EQ_INT(r.errs[r.nev - 1], TUNNEL_ERR_PEER_CLOSED);
    CHECK_EQ_INT(r.events[0], TUNNEL_EV_CONNECTING);
    CHECK_EQ_INT(r.events[1], TUNNEL_EV_UP);
    CHECK_EQ_INT(server_status(pid), 0);
    vt_destroy(vt);
}

static void script_err(int lfd)
{
    int fd = accept(lfd, NULL, NULL);

    srv_handshake(fd, "ERR\n");
    sleep_ms(100);
    close(fd);
}

static void script_compress(int lfd)
{
    int fd = accept(lfd, NULL, NULL);

    srv_handshake(fd, "OK FLAGS: <TeC9K>\n");
    sleep_ms(100);
    close(fd);
}

static void script_bad_greeting(int lfd)
{
    int fd = accept(lfd, NULL, NULL);

    srv_mesg(fd, "SSH-2.0-OpenSSH\n");
    sleep_ms(100);
    close(fd);
}

static void script_silent(int lfd)
{
    int fd = accept(lfd, NULL, NULL);

    sleep_ms(1500);
    close(fd);
}

static void expect_failure(script_fn script, int want_err, int no_reconnect)
{
    uint16_t port;
    pid_t pid = start_server(script, &port);
    rec_t r;
    vt_t *vt = make_client(port, &r, no_reconnect);

    vt_start(vt);
    CHECK(run_until(vt, pred_failed, &r, 3000));
    CHECK_EQ_INT(r.errs[r.nev - 1], want_err);
    vt_destroy(vt);
    kill(pid, SIGTERM);
    server_status(pid);
}

static void test_auth_error_is_final(void)
{
    /* ERR must not be retried even with reconnect enabled */
    expect_failure(script_err, TUNNEL_ERR_AUTH, 0);
}

static void test_compression_refused(void)
{
    expect_failure(script_compress, TUNNEL_ERR_COMPRESSION, 0);
}

static void test_bad_greeting(void)
{
    expect_failure(script_bad_greeting, TUNNEL_ERR_PROTOCOL, 0);
}

static void test_handshake_timeout(void)
{
    expect_failure(script_silent, TUNNEL_ERR_TIMEOUT, 1);
}

static void script_keepalive_dead(int lfd)
{
    int fd = accept(lfd, NULL, NULL);

    srv_handshake(fd, "OK FLAGS: <TuK>\n");
    sleep_ms(2000);     /* never answer */
    close(fd);
}

static void test_keepalive_timeout(void)
{
    uint16_t port;
    pid_t pid = start_server(script_keepalive_dead, &port);
    rec_t r;
    vt_t *vt = make_client(port, &r, 1);
    uint64_t t0;

    t0 = nc_now_ms();
    vt_start(vt);
    CHECK(run_until(vt, pred_up, &r, 2000));
    CHECK_EQ_INT(vt_tunnel(vt).ops->link_type(vt), TUNNEL_LINK_L3);
    CHECK(run_until(vt, pred_failed, &r, 3000));
    CHECK_EQ_INT(r.errs[r.nev - 1], TUNNEL_ERR_KEEPALIVE);
    CHECK(nc_now_ms() - t0 >= 550);    /* 3 x 200 ms of silence after OK FLAGS */
    vt_destroy(vt);
    kill(pid, SIGTERM);
    server_status(pid);
}

static void script_reconnect(int lfd)
{
    int fd = accept(lfd, NULL, NULL);

    close(fd);                          /* first attempt: dropped at once */
    fd = accept(lfd, NULL, NULL);
    srv_handshake(fd, "OK FLAGS: <TeK>\n");
    sleep_ms(300);                      /* client reports UP after the data delay */
    srv_flag(fd, VT_CONN_CLOSE);        /* e.g. taken over by killold */
    fd = accept(lfd, NULL, NULL);
    srv_handshake(fd, "OK FLAGS: <TeK>\n");
    sleep_ms(1000);
    close(fd);
}

static void test_reconnect(void)
{
    uint16_t port;
    pid_t pid = start_server(script_reconnect, &port);
    rec_t r;
    vt_t *vt = make_client(port, &r, 0);
    int i, downs = 0;

    vt_start(vt);
    CHECK(run_until(vt, pred_ups2, &r, 4000));
    for (i = 0; i < r.nev; i++) {
        if (r.events[i] == TUNNEL_EV_DOWN) {
            downs++;
            CHECK_EQ_INT(r.errs[i], TUNNEL_ERR_PEER_CLOSED);
        }
        CHECK(r.events[i] != TUNNEL_EV_FAILED);
    }
    CHECK_EQ_INT(downs, 2);
    vt_tunnel(vt).ops->close(vt);
    vt_destroy(vt);
    kill(pid, SIGTERM);
    server_status(pid);
}

static void test_config_validation(void)
{
    vt_cfg_t cfg;
    int err;

    memset(&cfg, 0, sizeof(cfg));
    cfg.host = "127.0.0.1";
    cfg.port = 1;
    cfg.session = "bad name";
    cfg.password = "x";
    CHECK(vt_create(&cfg, &err) == NULL);
    CHECK_EQ_INT(err, TUNNEL_ERR_CONFIG);
    cfg.session = SESSION;
    cfg.password = "";
    CHECK(vt_create(&cfg, &err) == NULL);
    CHECK_EQ_INT(err, TUNNEL_ERR_CONFIG);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    RUN(test_config_validation);
    RUN(test_happy_path);
    RUN(test_auth_error_is_final);
    RUN(test_compression_refused);
    RUN(test_bad_greeting);
    RUN(test_handshake_timeout);
    RUN(test_keepalive_timeout);
    RUN(test_reconnect);
    return UNIT_SUMMARY();
}
