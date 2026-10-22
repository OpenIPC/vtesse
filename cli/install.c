/*
 * Self-install: "vtesse --install" and "vtesse --uninstall" (#50, #51, #52).
 *
 * --install:
 *  1. detects the system (#51): OpenWrt (/etc/openwrt_release), OpenIPC
 *     (OPENIPC_VERSION in /etc/os-release), a Linux PC with systemd
 *     (/run/systemd/system), or another BusyBox system with /etc/init.d;
 *  2. builds /etc/vtesse.conf from the command-line keys: an existing file keeps
 *     its other lines, a new one gets port 5000 and the device ID from the MAC of
 *     the interface with the default route unless they are given;
 *  3. copies the running binary to /usr/bin/vtesse (/usr/local/bin on a PC);
 *  4. writes the init script of the system from the built-in copy: S99vtesse,
 *     the procd vtesse.init with its rc.d link and /lib/upgrade/keep.d/vtesse
 *     (OpenWrt keeps only /etc across sysupgrade), or the systemd unit;
 *  5. a file-based service gets accept=80, 22 and 554 when it has no accept line
 *     (#67): the camera's web, ssh and RTSP from the server, as the TAP backend of
 *     1.x gave them; the hook scripts of 1.x are removed (#72);
 *  6. (re)starts the service, unless --no-start.
 * --uninstall stops the service and removes everything --install may have
 * written, /etc/vtesse.conf included, for any of the layouts.
 * Both can run again: --install updates the binary, the init script and the given
 * keys, --uninstall on a clean system is not an error.
 *
 * --root DIR does all of it below DIR and skips the service (tests, staging).
 */
#define _XOPEN_SOURCE 700       /* realpath() */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "env.h"
#include "install.h"
#include "netcore/route.h"
#include "vtesse/vtun_proto.h"
#include "vtesse_scripts.h"     /* generated from scripts/ (tools/embed-scripts.sh) */

#define CONF_PATH "/etc/vtesse.conf"
#define CONF_MAX  (64 * 1024)

typedef enum { SYS_NONE, SYS_OPENIPC, SYS_OPENWRT, SYS_SYSTEMD, SYS_BUSYBOX } sys_t;

typedef struct layout {
    const char *name;
    const char *bin;        /* the binary */
    const char *init;       /* init script or unit */
    const char *init_src;   /* its built-in copy (scripts/) */
    mode_t init_mode;
} layout_t;

static const layout_t layouts[] = {
    [SYS_OPENIPC] = { "OpenIPC", "/usr/bin/vtesse", "/etc/init.d/S99vtesse", "S99vtesse", 0755 },
    [SYS_OPENWRT] = { "OpenWrt", "/usr/bin/vtesse", "/etc/init.d/vtesse", "vtesse.init", 0755 },
    [SYS_SYSTEMD] = { "Linux with systemd", "/usr/local/bin/vtesse", "/etc/systemd/system/vtesse.service",
                      "vtesse.service", 0644 },
    [SYS_BUSYBOX] = { "BusyBox init (/etc/init.d)", "/usr/bin/vtesse", "/etc/init.d/S99vtesse", "S99vtesse",
                      0755 },
};

#define OPENWRT_RC   "/etc/rc.d/S99vtesse"
#define OPENWRT_KEEP "/lib/upgrade/keep.d/vtesse"
#define LEGACY_DIR   "/usr/share/vtesse"     /* the hook scripts up to 1.13 */

/* the TAP backend's hook scripts of 1.x (removed in 2.0.0, #72): only cleaned up */
static const char *hook_scripts[] = { "vtesse-hook.sh", "vtesse-udhcpc.script" };

/* everything --uninstall removes, whatever the layout */
static const char *all_files[] = {
    "/usr/bin/vtesse", "/usr/local/bin/vtesse",
    "/etc/init.d/S99vtesse", "/etc/init.d/vtesse", OPENWRT_RC, OPENWRT_KEEP,
    "/etc/systemd/system/vtesse.service", "/etc/default/vtesse", CONF_PATH,
};
static const char *all_dirs[] = { "/usr/libexec/vtesse", "/usr/local/libexec/vtesse", LEGACY_DIR };

static const char *root = "";

const char *vi_script(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(embedded_scripts) / sizeof(embedded_scripts[0]); i++)
        if (!strcmp(embedded_scripts[i].name, name))
            return embedded_scripts[i].text;
    return NULL;
}

const char *vi_self_path(void)
{
    static char path[512];
    static int done;
    ssize_t n;

    if (done)
        return path;
    done = 1;
    n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) {
        path[0] = '\0';
        return path;
    }
    path[n] = '\0';
    /* the binary was replaced while running (an update): its path is still right */
    if (n > 10 && !strcmp(path + n - 10, " (deleted)"))
        path[n - 10] = '\0';
    return path;
}

int vi_wanted(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--install") || !strcmp(argv[i], "--uninstall"))
            return 1;
    return 0;
}

/* ---- files ---- */

static const char *rp(const char *path)     /* path below --root */
{
    static char buf[4][1024];
    static int k;

    k = (k + 1) % 4;
    snprintf(buf[k], sizeof(buf[k]), "%s%s", root, path);
    return buf[k];
}

static int exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

static int is_link(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

static int mkdir_p(const char *dir)
{
    char tmp[1024];
    char *p;

    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return mkdir(tmp, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

static int mkdir_parent(const char *path)
{
    char dir[1024];
    char *slash;

    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return 0;
    *slash = '\0';
    return mkdir_p(dir);
}

/* Writes a file through PATH.new and rename(): a running binary or a script being
 * read is replaced, not rewritten in place ("Text file busy"). */
static int write_file(const char *path, const void *data, size_t len, mode_t mode)
{
    char tmp[1100];
    const char *p = data;
    int fd;

    if (mkdir_parent(path) < 0) {
        fprintf(stderr, "vtesse: cannot create the directory of %s: %s\n", path, strerror(errno));
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    /* a fresh file: a left-over PATH.new, or a symlink in its place, is not followed */
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
    if (fd < 0) {
        fprintf(stderr, "vtesse: cannot write %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            fprintf(stderr, "vtesse: cannot write %s: %s\n", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    if (fchmod(fd, mode) < 0 || fsync(fd) < 0 || close(fd) < 0 || rename(tmp, path) < 0) {
        fprintf(stderr, "vtesse: cannot write %s: %s\n", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t n = 0, cap = 64 * 1024;

    if (!f)
        return NULL;
    buf = malloc(cap + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    for (;;) {
        size_t r = fread(buf + n, 1, cap - n, f);
        n += r;
        if (n < cap) {
            if (ferror(f)) {        /* a short read on an error is not the whole file */
                free(buf);
                fclose(f);
                errno = EIO;
                return NULL;
            }
            break;
        }
        cap *= 2;
        if (cap > 64 * 1024 * 1024) {
            free(buf);
            fclose(f);
            errno = EFBIG;
            return NULL;
        }
        {
            char *nb = realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
        }
    }
    fclose(f);
    buf[n] = '\0';
    *len = n;
    return buf;
}

static int remove_file(const char *path)
{
    if (unlink(path) == 0) {
        printf("removed %s\n", path);
        return 0;
    }
    if (errno == ENOENT)
        return 0;
    fprintf(stderr, "vtesse: cannot remove %s: %s\n", path, strerror(errno));
    return -1;
}

/* ---- running commands (service start and stop) ---- */

static int run(const char *const argv[])
{
    pid_t pid;
    int st, i;

    printf("$");
    for (i = 0; argv[i]; i++)
        printf(" %s", argv[i]);
    printf("\n");
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "vtesse: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (pid < 0)
        return -1;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ---- system detection (#51) ---- */

/* 1 when a line of the file starts with prefix (a "KEY=" assignment of os-release;
 * comments and other keys do not count) */
static int file_has_line(const char *path, const char *prefix)
{
    size_t len, n = strlen(prefix);
    char *s = read_file(path, &len), *p;
    int found = 0;

    for (p = s; p && *p; ) {
        char *e = strchr(p, '\n');

        if (!strncmp(p, prefix, n)) {
            found = 1;
            break;
        }
        p = e ? e + 1 : p + strlen(p);
    }
    free(s);
    return found;
}

static int file_has(const char *path, const char *needle)
{
    size_t len;
    char *s = read_file(path, &len);
    int found = s && strstr(s, needle);

    free(s);
    return found;
}

static int init_is_busybox(void)
{
    char *t = realpath(rp("/sbin/init"), NULL);
    int bb = t && strstr(t, "busybox");

    free(t);
    return bb;
}

static sys_t detect(void)
{
    struct stat st;

    if (exists(rp("/etc/openwrt_release")))
        return SYS_OPENWRT;
    if (file_has_line(rp("/etc/os-release"), "OPENIPC_VERSION="))
        return SYS_OPENIPC;
    if (stat(rp("/run/systemd/system"), &st) == 0 && S_ISDIR(st.st_mode))
        return SYS_SYSTEMD;
    /* BusyBox init only: its rcS runs /etc/init.d/S??*; OpenRC and sysvinit do not,
     * and S99vtesse would be gone after the first reboot */
    if (stat(rp("/etc/init.d"), &st) == 0 && S_ISDIR(st.st_mode) && !exists(rp("/run/openrc")) &&
        !exists(rp("/sbin/openrc-run")) && (file_has(rp("/etc/init.d/rcS"), "S??") || init_is_busybox()))
        return SYS_BUSYBOX;
    return SYS_NONE;
}

static sys_t parse_system(const char *s)
{
    if (!strcmp(s, "openipc"))
        return SYS_OPENIPC;
    if (!strcmp(s, "openwrt"))
        return SYS_OPENWRT;
    if (!strcmp(s, "systemd"))
        return SYS_SYSTEMD;
    if (!strcmp(s, "busybox"))
        return SYS_BUSYBOX;
    return SYS_NONE;
}

/* ---- /etc/vtesse.conf ---- */

typedef struct kv {
    const char *key;
    const char *val;
    int done;
} kv_t;

#define MAX_KEYS 8

/* The key of a "key=value" line of len bytes, or 0 for a comment, a blank line and
 * anything else. */
static int key_of(const char *line, size_t len, char *key, size_t size)
{
    const char *end = line + len, *eq;
    size_t n;

    while (line < end && (*line == ' ' || *line == '\t'))
        line++;
    if (line == end || *line == '#')
        return 0;
    eq = memchr(line, '=', (size_t)(end - line));
    if (!eq)
        return 0;
    n = (size_t)(eq - line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t'))
        n--;
    if (!n || n >= size)
        return 0;
    memcpy(key, line, n);
    key[n] = '\0';
    return 1;
}

/* 1 when the value of a "key=value" line of len bytes is not empty */
static int has_value(const char *line, size_t len)
{
    const char *end = line + len, *v = memchr(line, '=', len);

    if (!v)
        return 0;
    for (v++; v < end; v++)
        if (*v != ' ' && *v != '\t' && *v != '\r')
            return 1;
    return 0;
}

static kv_t *find(kv_t *set, int n, const char *key)
{
    int i;

    for (i = 0; i < n; i++)
        if (!strcmp(set[i].key, key))
            return &set[i];
    return NULL;
}

typedef struct outbuf {
    char *p;
    size_t len;
} outbuf_t;

/* Appends len bytes and a newline; -1 when the result would not fit in CONF_MAX. */
static int out_line(outbuf_t *o, const char *line, size_t len)
{
    if (o->len + len + 2 > CONF_MAX) {
        fprintf(stderr, "vtesse: %s would be larger than %d bytes\n", CONF_PATH, CONF_MAX);
        return -1;
    }
    memcpy(o->p + o->len, line, len);
    o->len += len;
    o->p[o->len++] = '\n';
    o->p[o->len] = '\0';
    return 0;
}

static int out_kv(outbuf_t *o, const char *key, const char *val)
{
    static char line[CONF_MAX];
    int n = snprintf(line, sizeof(line), "%s=%s", key, val);

    if (n < 0 || (size_t)n >= sizeof(line))
        return out_line(o, line, CONF_MAX);    /* reports "too large" */
    return out_line(o, line, (size_t)n);
}

/* Builds the new /etc/vtesse.conf in out (CONF_MAX + 1 bytes) from the existing text
 * (or NULL) and the given keys. env: the U-Boot environment when it holds the server
 * (#64), else NULL; the client then takes the server, the defaults and the derived
 * password from there, so the file needs none of them and gets no defaults.
 * Returns 0, or -1 with the reason printed. */
static int build_config(const char *old, kv_t *set, int n, char *out, const ve_t *env)
{
    static char iface[64];
    int has_host = 0, has_port = 0, bad_port = 0, has_pass = 0, has_session = 0, has_idfrom = 0, has_ip = 0,
        has_gw = 0, has_accept = 0, i;
    outbuf_t o = { out, 0 };
    const char *p = old;
    char key[64];

    out[0] = '\0';
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *line = p;
        kv_t *k;

        p = e ? e + 1 : p + len;
        if (key_of(line, len, key, sizeof(key))) {
            /* session and id_from exclude each other: a given one replaces the other */
            if ((!strcmp(key, "session") && find(set, n, "id_from")) ||
                (!strcmp(key, "id_from") && find(set, n, "session")))
                continue;
            k = find(set, n, key);
            if (k) {
                if (!k->done && out_kv(&o, k->key, k->val) < 0)
                    return -1;
                k->done = 1;
                continue;
            }
        }
        if (out_line(&o, line, len) < 0)
            return -1;
    }
    for (i = 0; i < n; i++)
        if (!set[i].done && out_kv(&o, set[i].key, set[i].val) < 0)
            return -1;

    for (p = out; *p; ) {
        const char *e = strchr(p, '\n');     /* every line written above ends with one */
        size_t len = (size_t)(e - p);

        /* the last line of a key counts, as in the client's parser */
        if (key_of(p, len, key, sizeof(key))) {
            int v = has_value(p, len);

            if (!strcmp(key, "host"))
                has_host = v;
            else if (!strcmp(key, "port")) {
                const char *eq = memchr(p, '=', len);
                char num[16];
                size_t k = (size_t)(p + len - (eq + 1));
                long port;
                char *end;

                while (k && (eq[1] == ' ' || eq[1] == '\t'))
                    eq++, k--;
                snprintf(num, sizeof(num), "%.*s", (int)(k < 15 ? k : 15), eq + 1);
                port = strtol(num, &end, 10);
                while (*end == ' ' || *end == '\t' || *end == '\r')
                    end++;
                has_port = v;
                bad_port = v && (end == num || *end || port < 1 || port > 65535);
            }
            else if (!strcmp(key, "password"))
                has_pass = v;
            else if (!strcmp(key, "session"))
                has_session = v;
            else if (!strcmp(key, "id_from"))
                has_idfrom = v;
            else if (!strcmp(key, "ip"))
                has_ip = v;
            else if (!strcmp(key, "gw") || !strcmp(key, "route"))
                has_gw |= v;
            else if (!strcmp(key, "accept"))
                has_accept |= v;
        }
        p = e + 1;
    }
    if (env && env->ip[0])
        has_ip = 1;
    if (!env && (!has_host || !has_pass)) {
        fprintf(stderr,
                "vtesse: %s%s needs at least the server and the password:\n"
                "  vtesse --install --host SERVER --password PASSWORD\n"
                "        [--port 5000] [--session ID | --id-from IFACE] [--ip A.B.C.D/P [--gw A.B.C.D]]\n"
                "  (--password - reads it from standard input)\n",
                root, CONF_PATH);
        return -1;
    }
    /* a valid vtesse_port overrides the file's port at runtime */
    if (bad_port && !(env && env->port[0])) {
        fprintf(stderr, "vtesse: %s%s: port must be 1..65535: give --port\n", root, CONF_PATH);
        return -1;
    }
    /* vtesse_session or vtesse_id_from replaces both keys at runtime */
    if (has_session && has_idfrom && !(env && (env->session[0] || env->id_from[0]))) {
        fprintf(stderr, "vtesse: %s%s has both session and id_from: give --session or --id-from\n", root, CONF_PATH);
        return -1;
    }
    if (has_gw && !has_ip) {
        fprintf(stderr, "vtesse: gw and route need ip (--ip A.B.C.D/P)\n");
        return -1;
    }
    /* inbound access for the server (#67), as the TAP backend of 1.x gave it: the
     * camera's web, ssh and RTSP on its LAN address; with the U-Boot environment and
     * no accept line the client opens the same itself */
    if (!has_accept && (!env || old)) {
        if (out_kv(&o, "accept", "80") < 0 || out_kv(&o, "accept", "22") < 0 || out_kv(&o, "accept", "554") < 0)
            return -1;
        printf("accept=80, 22, 554: the server reaches the camera's web, ssh and RTSP (#67)\n");
    }
    if (env)
        return 0;
    if (!has_port && out_kv(&o, "port", "5000") < 0)
        return -1;
    if (!has_session && !has_idfrom) {
        /* the interfaces of this machine say nothing about a staged image */
        if (root[0]) {
            fprintf(stderr, "vtesse: with --root, give --session ID or --id-from IFACE\n");
            return -1;
        }
        if (ve_default_iface(iface, sizeof(iface)) < 0) {
            fprintf(stderr, "vtesse: no interface with a MAC for the device ID (the default route goes\n"
                            "  through PPPoE, WireGuard or tun?): give --session ID or --id-from IFACE\n");
            return -1;
        }
        if (out_kv(&o, "id_from", iface) < 0)
            return -1;
    }
    return 0;
}

/* The value of a configuration key in the U-Boot environment ("" when unset) */
static const char *env_value(const ve_t *e, const char *key)
{
    return !strcmp(key, "host") ? e->host : !strcmp(key, "port") ? e->port :
           !strcmp(key, "session") ? e->session : !strcmp(key, "id_from") ? e->id_from :
           !strcmp(key, "password") ? e->password : !strcmp(key, "ip") ? e->ip :
           !strcmp(key, "gw") ? e->gw : "";
}

/* Without fw_setenv the given keys go only to the file, and a set vtesse_* variable
 * overrides its key there at runtime (#64): such a key is refused, nothing could
 * change it. Returns 0, or -1 with the reason printed. */
static int env_conflict(const ve_t *e, kv_t *set, int n)
{
    size_t v;
    int i;

    for (i = 0; i < n; i++)
        for (v = 0; v < ve_nvars; v++) {
            const char *key = ve_vars[v].key, *var = ve_vars[v].name;

            if (strcmp(key, set[i].key) != 0)
                continue;
            /* session and id_from: either variable replaces both keys at runtime */
            if (!strcmp(key, "session") || !strcmp(key, "id_from")) {
                if (!e->session[0] && !e->id_from[0])
                    continue;
                var = e->session[0] ? "vtesse_session" : "vtesse_id_from";
            } else if (!env_value(e, key)[0])
                continue;
            fprintf(stderr, "vtesse: %s is set in the U-Boot environment and overrides --%s,\n"
                            "  and there is no fw_setenv to change it\n",
                    var, !strcmp(key, "id_from") ? "id-from" : key);
            return -1;
        }
    return 0;
}

/* Appends "NAME VALUE\n" (or "NAME\n", which deletes it, for a NULL value) to the
 * fw_setenv -s script; -1 when it would not fit */
static int script_add(char *buf, size_t size, size_t *len, const char *name, const char *value)
{
    int r = value ? snprintf(buf + *len, size - *len, "%s %s\n", name, value)
                  : snprintf(buf + *len, size - *len, "%s\n", name);

    if (r < 0 || (size_t)r >= size - *len)
        return -1;
    *len += (size_t)r;
    return 0;
}

/* ---- --install / --uninstall ---- */

static void usage(void)
{
    fprintf(stderr,
            "usage: vtesse --install [--host SERVER] [--port N] [--password PASSWORD|-]\n"
            "                        [--session ID | --id-from IFACE] [--ip A.B.C.D/P] [--gw A.B.C.D]\n"
            "                        [--no-start] [--system SYSTEM] [--root DIR]\n"
            "       vtesse --uninstall [--system SYSTEM] [--root DIR]\n"
            "  SYSTEM: openipc, openwrt, systemd or busybox (default: detected)\n"
            "  the keys go to /etc/vtesse.conf; an existing file keeps its other lines\n");
}

static int copy_self(const char *dst)
{
    const char *src = vi_self_path();
    char *rsrc, *rdst;
    size_t len;
    char *data;
    int rc, same;

    if (!src[0]) {
        fprintf(stderr, "vtesse: cannot find the running binary (/proc/self/exe)\n");
        return -1;
    }
    /* realpath() allocates: a fixed buffer must be PATH_MAX (glibc's fortify aborts) */
    rsrc = realpath(src, NULL);
    rdst = realpath(dst, NULL);
    same = rsrc && rdst && !strcmp(rsrc, rdst);
    free(rsrc);
    free(rdst);
    if (same) {
        printf("binary %s (already in place)\n", dst);
        return 0;
    }
    /* the running binary itself: its path may be gone (a removed or replaced file) */
    data = read_file("/proc/self/exe", &len);
    if (!data) {
        fprintf(stderr, "vtesse: cannot read %s: %s\n", src, strerror(errno));
        return -1;
    }
    rc = write_file(dst, data, len, 0755);
    free(data);
    if (rc == 0)
        printf("binary %s\n", dst);
    return rc;
}

static int write_script(const char *path, const char *name, mode_t mode)
{
    const char *text = vi_script(name);

    if (!text || write_file(path, text, strlen(text), mode) < 0)
        return -1;
    printf("wrote %s\n", path);
    return 0;
}

/* The hook scripts of 1.x (libexec, and /usr/share/vtesse up to 1.13) belonged to
 * the TAP backend, removed in 2.0.0 (#72): an update removes them. Never through a
 * symlink: only a real directory of ours is emptied. Returns 0, or -1 when a file
 * could not be removed. */
static int remove_old_scripts(void)
{
    static const char *dirs[] = { "/usr/libexec/vtesse", "/usr/local/libexec/vtesse", LEGACY_DIR };
    size_t i, j;
    int rc = 0;

    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        struct stat st;
        char path[1024];

        if (lstat(rp(dirs[i]), &st) < 0)
            continue;
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "vtesse: %s is not a directory, left in place\n", rp(dirs[i]));
            continue;
        }
        for (j = 0; j < sizeof(hook_scripts) / sizeof(hook_scripts[0]); j++) {
            snprintf(path, sizeof(path), "%s%s/%s", root, dirs[i], hook_scripts[j]);
            if (remove_file(path) < 0)
                rc = -1;
            snprintf(path, sizeof(path), "%s%s/%s.1.13", root, dirs[i], hook_scripts[j]);
            if (remove_file(path) < 0)
                rc = -1;
        }
        if (rmdir(rp(dirs[i])) == 0)
            printf("removed %s (hook scripts of the TAP backend, gone in 2.0.0)\n", rp(dirs[i]));
        else if (errno != ENOENT)
            fprintf(stderr, "vtesse: %s is not empty, left in place\n", rp(dirs[i]));
    }
    return rc;
}

/* /etc/default/vtesse of 1.x may still pass TAP options (ARGS="--tap ..."): the client
 * refuses them, so the service would not start. Said loudly, the file is not changed. */
static void check_default_args(void)
{
    size_t len;
    char *def = read_file(rp("/etc/default/vtesse"), &len);

    if (def && (strstr(def, "--tap") || strstr(def, "--hook") || strstr(def, "--ifname")))
        fprintf(stderr, "vtesse: warning: %s passes --tap/--hook/--ifname: the TAP backend was removed in "
                        "2.0.0 (#72) and the service will not start; remove those options\n",
                rp("/etc/default/vtesse"));
    free(def);
}

/* 1 when a process named vtesse other than this installer runs (BusyBox pidof of
 * OpenWrt 15.05 has no -o to leave ourselves out) */
static int daemon_running(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int found = 0;

    if (!d)
        return 0;
    while (!found && (e = readdir(d))) {
        char path[64], comm[32];
        long pid = strtol(e->d_name, NULL, 10);
        FILE *f;

        if (pid <= 0 || pid == (long)getpid())
            continue;
        snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
        f = fopen(path, "r");
        if (!f)
            continue;
        found = fgets(comm, sizeof(comm), f) && !strcmp(comm, "vtesse\n");
        fclose(f);
    }
    closedir(d);
    return found;
}

static int service(sys_t sys, const layout_t *l, int start)
{
    if (sys == SYS_SYSTEMD) {
        const char *reload[] = { "systemctl", "daemon-reload", NULL };
        const char *enable[] = { "systemctl", "enable", "vtesse", NULL };
        const char *restart[] = { "systemctl", "restart", "vtesse", NULL };
        const char *active[] = { "systemctl", "is-active", "vtesse", NULL };

        if (run(reload) != 0 || run(enable) != 0)
            return -1;
        if (!start)
            return 0;
        if (run(restart) != 0)
            return -1;
        sleep(1);
        return run(active) == 0 ? 0 : -1;
    }
    if (start) {
        const char *restart[] = { l->init, "restart", NULL };
        const char *status[] = { l->init, "status", NULL };

        if (run(restart) != 0)
            return -1;
        sleep(2);
        /* procd's vtesse.init has no status in older OpenWrt: look for the process */
        if (sys == SYS_OPENWRT)
            return daemon_running() ? 0 : -1;
        return run(status) == 0 ? 0 : -1;
    }
    return 0;
}

static int do_install(sys_t sys, kv_t *set, int n, int start)
{
    char env_host[256] = "";
    ve_t env;
    const layout_t *l = &layouts[sys];
    static char conf[CONF_MAX + 1];
    size_t len;
    char *old;
    int env_write;

    printf("system: %s%s\n", l->name, root[0] ? " (below --root)" : "");
    /* this device's environment: under --root it says nothing about the image */
    memset(&env, 0, sizeof(env));
    if (!root[0] && ve_load(&env))
        snprintf(env_host, sizeof(env_host), "%s", env.host);
    /* fw_setenv alone is not enough: u-boot-tools on a PC without fw_env.config */
    env_write = !root[0] && env.ok && ve_can_write();

    /* the configuration first: nothing is written when it is incomplete */
    old = read_file(rp(CONF_PATH), &len);
    if (!old && errno != ENOENT) {
        fprintf(stderr, "vtesse: cannot read %s: %s\n", rp(CONF_PATH), strerror(errno));
        return 1;
    }
    /* the server is in the U-Boot environment and there is no file: given keys go
     * there too (below), so no file is needed */
    if (!old && env_host[0] && (n == 0 || env_write)) {
        const kv_t *gw = find(set, n, "gw"), *ip = find(set, n, "ip");

        /* the same rule as for a file: a gateway needs an address */
        if (gw && !ip && !env.ip[0]) {
            fprintf(stderr, "vtesse: --gw needs --ip (or vtesse_ip)\n");
            free(old);
            return 1;
        }
        /* helpdesk set-up (#64): the settings come from the U-Boot environment */
        conf[0] = '\0';
    } else if ((n && !env_write && env_conflict(&env, set, n) < 0) ||
               build_config(old, set, n, conf, env_host[0] ? &env : NULL) < 0) {
        free(old);
        return 1;
    }

    if (copy_self(rp(l->bin)) < 0) {
        free(old);
        return 1;
    }
    if (!conf[0]) {
        const kv_t *host = find(set, n, "host");

        /* a given server replaces the variable below */
        printf("no %s: the settings come from the U-Boot environment (vtesse=%s)\n", rp(CONF_PATH),
               host ? host->val : env_host);
    } else if (!old || strcmp(old, conf) != 0) {
        if (write_file(rp(CONF_PATH), conf, strlen(conf), 0600) < 0) {
            free(old);
            return 1;
        }
        printf("%s %s\n", old ? "updated" : "wrote", rp(CONF_PATH));
        if (env_host[0] && n && !env_write)
            printf("no fw_setenv: the given keys are kept in %s, the server comes from the U-Boot "
                   "environment (vtesse=%s)\n", rp(CONF_PATH), env_host);
    } else if (is_link(rp(CONF_PATH))) {
        /* a symlink, maybe to a file someone else owns: replaced by our own file */
        if (write_file(rp(CONF_PATH), conf, strlen(conf), 0600) < 0) {
            free(old);
            return 1;
        }
        printf("replaced the symlink %s by a file\n", rp(CONF_PATH));
    } else {
        /* it holds the password: also an unchanged file is made private */
        if (chmod(rp(CONF_PATH), 0600) < 0) {
            fprintf(stderr, "vtesse: cannot chmod %s: %s\n", rp(CONF_PATH), strerror(errno));
            free(old);
            return 1;
        }
        printf("kept %s\n", rp(CONF_PATH));
    }
    free(old);

    if (write_script(rp(l->init), l->init_src, l->init_mode) < 0)
        return 1;
    /* the given keys also go to the U-Boot environment, when the device has one (#64) */
    if (n && env_write) {
        /* one batch through stdin: the password stays out of the process list, and
         * the variables are written together or not at all */
        static char script[CONF_MAX];
        size_t v, slen = 0;
        int k;

        for (k = 0; k < n; k++)
            for (v = 0; v < ve_nvars; v++) {
                const char *other = !strcmp(set[k].key, "session") ? "vtesse_id_from" :
                                    !strcmp(set[k].key, "id_from") ? "vtesse_session" : NULL;

                if (strcmp(ve_vars[v].key, set[k].key) != 0)
                    continue;
                /* the other identity variable would override the new one at runtime */
                if (script_add(script, sizeof(script), &slen, ve_vars[v].name, set[k].val) < 0 ||
                    (other && script_add(script, sizeof(script), &slen, other, NULL) < 0)) {
                    fprintf(stderr, "vtesse: the settings are too long for the U-Boot environment\n");
                    return 1;
                }
            }
        if (slen) {
            if (ve_set_script(script) != 0) {
                fprintf(stderr, "vtesse: fw_setenv failed: nothing changed in the U-Boot environment, "
                                "the service is not started\n");
                return 1;
            }
            for (k = 0; k < n; k++)
                for (v = 0; v < ve_nvars; v++)
                    if (!strcmp(ve_vars[v].key, set[k].key))
                        printf("U-Boot environment: %s set\n", ve_vars[v].name);
        }
    }
    if (sys == SYS_OPENWRT) {
        const char *keep = "/usr/bin/vtesse\n/etc/init.d/vtesse\n"
                           OPENWRT_RC "\n" CONF_PATH "\n" OPENWRT_KEEP "\n";

        unlink(rp(OPENWRT_RC));
        if (mkdir_parent(rp(OPENWRT_RC)) < 0 || symlink("../init.d/vtesse", rp(OPENWRT_RC)) < 0) {
            fprintf(stderr, "vtesse: cannot link %s: %s\n", rp(OPENWRT_RC), strerror(errno));
            return 1;
        }
        printf("enabled %s\n", rp(OPENWRT_RC));
        /* sysupgrade keeps only the changed files under /etc without this list */
        if (write_file(rp(OPENWRT_KEEP), keep, strlen(keep), 0644) < 0)
            return 1;
        printf("wrote %s (kept by sysupgrade)\n", rp(OPENWRT_KEEP));
    }

    if (remove_old_scripts() < 0)
        return 1;
    check_default_args();
    if (root[0]) {
        printf("--root: the service is not started\n");
        return 0;
    }
    if (service(sys, l, start) < 0) {
        fprintf(stderr, "vtesse: the service did not start; check %s and the log\n", CONF_PATH);
        return 1;
    }
    printf(start ? "vtesse is installed and running\n" : "vtesse is installed (not started: --no-start)\n");
    return 0;
}

/* The PID of S99vtesse's daemon (/var/run/vtesse.pid), 0 if none. */
static pid_t service_pid(void)
{
    FILE *f = fopen("/var/run/vtesse.pid", "r");
    long pid = 0;

    if (f) {
        if (fscanf(f, "%ld", &pid) != 1)
            pid = 0;
        fclose(f);
    }
    if (pid > 1) {
        char path[64], comm[32] = "";

        /* a stale file whose PID now belongs to another program: no service */
        snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
        f = fopen(path, "r");
        if (f) {
            if (!fgets(comm, sizeof(comm), f))
                comm[0] = '\0';
            fclose(f);
        }
        if (strcmp(comm, "vtesse\n") != 0)
            pid = 0;
    }
    return pid > 1 ? (pid_t)pid : 0;
}

/* 1 when the process has exited within ms milliseconds */
static int gone(pid_t pid, int ms)
{
    for (; ms > 0; ms -= 100) {
        struct timespec ts = { 0, 100 * 1000000L };

        if (kill(pid, 0) < 0 && errno == ESRCH)
            return 1;
        nanosleep(&ts, NULL);
    }
    return kill(pid, 0) < 0 && errno == ESRCH;
}

static int do_uninstall(sys_t sys)
{
    size_t i;
    int rc = 0;

    /* every installed service must be stopped before its files go, whatever the
     * detected system: an install of another layout may be there */
    (void)sys;
    if (!root[0]) {
        static const char *inits[] = { "/etc/init.d/S99vtesse", "/etc/init.d/vtesse" };

        /* a unit file left on a system without a running systemd has no service */
        if (exists(layouts[SYS_SYSTEMD].init) && access("/run/systemd/system", F_OK) == 0) {
            const char *disable[] = { "systemctl", "disable", "--now", "vtesse", NULL };

            if (run(disable) != 0) {
                fprintf(stderr, "vtesse: systemctl could not stop the service; nothing removed\n");
                return 1;
            }
        }
        for (i = 0; i < sizeof(inits) / sizeof(inits[0]); i++) {
            const char *stop[] = { inits[i], "stop", NULL };
            /* S99vtesse removes its PID file even when the process survives: the PID
             * is read first and the process watched for up to 5 s */
            pid_t pid = i == 0 ? service_pid() : 0;

            if (!exists(inits[i]))
                continue;
            if (run(stop) != 0 || (pid > 0 && !gone(pid, 5000))) {
                fprintf(stderr, "vtesse: the service is still running; nothing removed\n");
                return 1;
            }
        }
    }
    for (i = 0; i < sizeof(all_files) / sizeof(all_files[0]); i++)
        if (remove_file(rp(all_files[i])) < 0)
            rc = 1;
    for (i = 0; i < sizeof(all_dirs) / sizeof(all_dirs[0]); i++) {
        struct stat st;
        size_t j;
        char path[1024];

        /* never through a symlink: only a real directory of ours is emptied */
        if (lstat(rp(all_dirs[i]), &st) < 0)
            continue;
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "vtesse: %s is not a directory, left in place\n", rp(all_dirs[i]));
            continue;
        }

        for (j = 0; j < sizeof(hook_scripts) / sizeof(hook_scripts[0]); j++) {
            snprintf(path, sizeof(path), "%s%s/%s", root, all_dirs[i], hook_scripts[j]);
            if (remove_file(path) < 0)
                rc = 1;
            snprintf(path, sizeof(path), "%s%s/%s.1.13", root, all_dirs[i], hook_scripts[j]);
            if (remove_file(path) < 0)
                rc = 1;
        }
        if (rmdir(rp(all_dirs[i])) == 0)
            printf("removed %s\n", rp(all_dirs[i]));
        else if (errno != ENOENT)
            fprintf(stderr, "vtesse: %s is not empty, left in place\n", rp(all_dirs[i]));
    }
    if (!root[0] && access("/run/systemd/system", F_OK) == 0) {
        const char *reload[] = { "systemctl", "daemon-reload", NULL };
        run(reload);
    }
    {
        ve_t env;

        if (!root[0] && ve_load(&env))
            printf("the U-Boot environment keeps vtesse=%s (fw_setenv vtesse to clear it)\n", env.host);
    }
    printf(rc ? "vtesse: uninstalled with errors\n" : "vtesse is uninstalled\n");
    return rc;
}

/* A.B.C.D, or A.B.C.D/P with prefix: the forms the client parses */
static int ip4_ok(const char *v, int prefix)
{
    unsigned a, b, c, d, p;
    char tail;

    if (prefix)
        return sscanf(v, "%u.%u.%u.%u/%u%c", &a, &b, &c, &d, &p, &tail) == 5 &&
               a < 256 && b < 256 && c < 256 && d < 256 && p <= 32;
    return sscanf(v, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256;
}

/* The limits of the runtime parser (load_config in vtesse_cli.c): longer values
 * would be cut there, and it trims spaces around a value. */
static int check_value(const char *key, const char *v)
{
    static const struct { const char *key; size_t max; } limits[] = {
        { "host", 255 }, { "session", VT_SESSION_MAX }, { "id_from", 31 },
        { "ip", 31 }, { "gw", 31 }, { "password", 127 },
    };
    size_t i, len = strlen(v);

    if (v[0] == ' ' || v[0] == '\t' || v[len - 1] == ' ' || v[len - 1] == '\t' || v[len - 1] == '\r') {
        fprintf(stderr, "vtesse: the %s value must not start or end with a space\n", key);
        return -1;
    }
    /* control characters: a newline is refused with the arguments already, the rest
     * (\r, escapes) has no place in the file or in the fw_setenv script either */
    for (i = 0; i < len; i++)
        if ((unsigned char)v[i] < 0x20 || v[i] == 0x7f) {
            fprintf(stderr, "vtesse: the %s value must not contain control characters\n", key);
            return -1;
        }
    for (i = 0; i < sizeof(limits) / sizeof(limits[0]); i++)
        if (!strcmp(limits[i].key, key) && len > limits[i].max) {
            fprintf(stderr, "vtesse: the %s value is longer than %u characters\n", key, (unsigned)limits[i].max);
            return -1;
        }
    if (!strcmp(key, "id_from") && (strchr(v, '/') || !strcmp(v, ".") || !strcmp(v, ".."))) {
        fprintf(stderr, "vtesse: --id-from: an interface name, e.g. eth0\n");
        return -1;
    }
    if ((!strcmp(key, "ip") && !ip4_ok(v, 1)) || (!strcmp(key, "gw") && !ip4_ok(v, 0))) {
        fprintf(stderr, "vtesse: --%s: expected %s, got '%s'\n", key, !strcmp(key, "ip") ? "A.B.C.D/P" : "A.B.C.D", v);
        return -1;
    }
    if (!strcmp(key, "session") && !vt_session_name_valid(v)) {
        fprintf(stderr, "vtesse: --session: no spaces, colons or control characters (a device ID: 12 x 0-9A-F)\n");
        return -1;
    }
    return 0;
}

static int read_password(char *buf, size_t size)
{
    size_t n;

    if (!fgets(buf, (int)size, stdin))
        return -1;
    n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return n ? 0 : -1;
}

int vi_main(int argc, char **argv)
{
    static char password[256];
    kv_t set[MAX_KEYS];
    const char *sys_name = NULL;
    int i, n = 0, install = 0, uninstall = 0, start = 1;
    sys_t sys;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *key = NULL;

        if (!strcmp(a, "--install"))
            install = 1;
        else if (!strcmp(a, "--uninstall"))
            uninstall = 1;
        else if (!strcmp(a, "--scripts")) {
            fprintf(stderr, "vtesse: --scripts: the hook scripts went with the TAP backend in 2.0.0 (#72)\n");
            return 2;
        }
        else if (!strcmp(a, "--no-start"))
            start = 0;
        else if (!strcmp(a, "--root") && i + 1 < argc) {
            root = argv[++i];
            /* "" (an unset variable) or "/" would mean the live system */
            if (!root[0] || !strcmp(root, "/")) {
                fprintf(stderr, "vtesse: --root needs a staging directory (not \"\" or /)\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--system") && i + 1 < argc)
            sys_name = argv[++i];
        else if (!strcmp(a, "--host"))
            key = "host";
        else if (!strcmp(a, "--port"))
            key = "port";
        else if (!strcmp(a, "--password"))
            key = "password";
        else if (!strcmp(a, "--session"))
            key = "session";
        else if (!strcmp(a, "--id-from"))
            key = "id_from";
        else if (!strcmp(a, "--ip"))
            key = "ip";
        else if (!strcmp(a, "--gw"))
            key = "gw";
        else {
            usage();
            return 2;
        }
        if (!key)
            continue;
        if (i + 1 >= argc || !argv[i + 1][0] || strchr(argv[i + 1], '\n') || n >= MAX_KEYS) {
            fprintf(stderr, "vtesse: %s needs a value\n", a);
            return 2;
        }
        if (find(set, n, key)) {
            fprintf(stderr, "vtesse: %s is given twice\n", a);
            return 2;
        }
        set[n].key = key;
        set[n].val = argv[++i];
        set[n].done = 0;
        if (!strcmp(key, "password") && !strcmp(set[n].val, "-")) {
            if (read_password(password, sizeof(password)) < 0) {
                fprintf(stderr, "vtesse: --password -: no password on standard input\n");
                return 2;
            }
            set[n].val = password;
        }
        if (check_value(key, set[n].val) < 0)
            return 2;
        if (!strcmp(key, "port")) {
            char *end;
            long p = strtol(set[n].val, &end, 10);
            if (*end || p < 1 || p > 65535) {
                fprintf(stderr, "vtesse: --port: expected 1..65535, got '%s'\n", set[n].val);
                return 2;
            }
        }
        n++;
    }
    if (install == uninstall) {
        usage();
        return 2;
    }
    if (find(set, n, "session") && find(set, n, "id_from")) {
        fprintf(stderr, "vtesse: --session and --id-from exclude each other\n");
        return 2;
    }
    if (uninstall && (n || !start)) {
        fprintf(stderr, "vtesse: --uninstall takes only --system and --root\n");
        return 2;
    }
    if (!root[0] && geteuid() != 0) {
        fprintf(stderr, "vtesse: --%s needs root\n", install ? "install" : "uninstall");
        return 1;
    }
    if (sys_name) {
        sys = parse_system(sys_name);
        if (sys == SYS_NONE) {
            fprintf(stderr, "vtesse: --system: openipc, openwrt, systemd or busybox, got '%s'\n", sys_name);
            return 2;
        }
    } else {
        sys = detect();
    }
    if (uninstall)
        return do_uninstall(sys);
    if (sys == SYS_NONE) {
        fprintf(stderr, "vtesse: unknown system (no systemd, no BusyBox init): name one with --system,\n"
                        "  or run the binary in the foreground: vtesse --config FILE\n");
        return 1;
    }
    return do_install(sys, set, n, start);
}
