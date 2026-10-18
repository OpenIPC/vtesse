/*
 * Settings in the U-Boot environment (#64), see env.h.
 */
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "env.h"
#include "netcore/route.h"

const ve_var_t ve_vars[] = {
    { "vtesse", "host" },
    { "vtesse_port", "port" },
    { "vtesse_session", "session" },
    { "vtesse_id_from", "id_from" },
    { "vtesse_password", "password" },
    { "vtesse_ip", "ip" },
    { "vtesse_gw", "gw" },
};
const size_t ve_nvars = sizeof(ve_vars) / sizeof(ve_vars[0]);

static char *field(ve_t *e, const char *key, size_t *size)
{
    static const struct { const char *key; size_t off, size; } map[] = {
        { "host", offsetof(ve_t, host), sizeof(((ve_t *)0)->host) },
        { "port", offsetof(ve_t, port), sizeof(((ve_t *)0)->port) },
        { "session", offsetof(ve_t, session), sizeof(((ve_t *)0)->session) },
        { "id_from", offsetof(ve_t, id_from), sizeof(((ve_t *)0)->id_from) },
        { "password", offsetof(ve_t, password), sizeof(((ve_t *)0)->password) },
        { "ip", offsetof(ve_t, ip), sizeof(((ve_t *)0)->ip) },
        { "gw", offsetof(ve_t, gw), sizeof(((ve_t *)0)->gw) },
    };
    size_t i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(map[i].key, key)) {
            *size = map[i].size;
            return (char *)e + map[i].off;
        }
    return NULL;
}

int ve_load(ve_t *e)
{
    char line[512];
    FILE *f;

    memset(e, 0, sizeof(*e));
    /* all variables at once: one fw_printenv instead of one per name */
    f = popen("fw_printenv 2>/dev/null", "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '=');
        size_t i, n;

        if (!v)
            continue;
        *v++ = '\0';
        n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r'))
            v[--n] = '\0';
        for (i = 0; i < ve_nvars; i++)
            if (!strcmp(line, ve_vars[i].name)) {
                size_t size;
                char *dst = field(e, ve_vars[i].key, &size);

                /* a value that would not fit is ignored rather than cut */
                if (dst && n < size)
                    memcpy(dst, v, n + 1);
                break;
            }
    }
    pclose(f);
    return e->host[0] != '\0';
}

int ve_can_write(void)
{
    const char *path = getenv("PATH");
    char file[600];

    while (path && *path) {
        size_t n = strcspn(path, ":");

        if (n && n < 512) {
            snprintf(file, sizeof(file), "%.*s/fw_setenv", (int)n, path);
            if (access(file, X_OK) == 0)
                return 1;
        }
        path += n + (path[n] == ':');
    }
    return 0;
}

int ve_set(const char *name, const char *value)
{
    pid_t pid = fork();
    int st;

    if (pid == 0) {
        /* no value: fw_setenv NAME deletes the variable */
        if (value)
            execlp("fw_setenv", "fw_setenv", name, value, (char *)NULL);
        else
            execlp("fw_setenv", "fw_setenv", name, (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* 1 when the interface has a MAC usable as the device ID: PPPoE, WireGuard and tun
 * have none */
static int has_mac(const char *ifname)
{
    char mac[18], id[13];

    /* the same parser as id_from at runtime: unicast, not all zeros */
    return ve_iface_mac(ifname, mac) == 0 && nc_devid_from_mac_str(mac, id) == 0;
}

int ve_default_iface(char *out, size_t size)
{
    FILE *f = fopen("/proc/net/route", "r");
    char line[256], ifname[64];
    unsigned long dst;

    if (f) {
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "%63s %lx", ifname, &dst) == 2 && dst == 0 && has_mac(ifname)) {
                snprintf(out, size, "%s", ifname);
                fclose(f);
                return 0;
            }
        fclose(f);
    }
    snprintf(out, size, "eth0");
    return has_mac(out) ? 0 : -1;
}

int ve_iface_mac(const char *ifname, char out[18])
{
    char path[96], line[64];
    FILE *f;
    int i;

    if (!ifname[0] || strchr(ifname, '/') || strlen(ifname) > 31)
        return -1;
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    for (i = 0; i < 17; i++) {
        unsigned char c = (unsigned char)line[i];

        if (i % 3 == 2 ? c != ':' : !isxdigit(c))
            return -1;
        out[i] = (char)toupper(c);
    }
    out[17] = '\0';
    return 0;
}

/* ---- SHA-1 (FIPS 180-4), for the VTun password only ---- */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(uint32_t h[5], const unsigned char *p)
{
    uint32_t w[80], a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;

        if (i < 20)
            f = (b & c) | (~b & d), k = 0x5a827999;
        else if (i < 40)
            f = b ^ c ^ d, k = 0x6ed9eba1;
        else if (i < 60)
            f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
        else
            f = b ^ c ^ d, k = 0xca62c1d6;
        t = ROL(a, 5) + f + e + k + w[i];
        e = d, d = c, c = ROL(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

void ve_sha1_hex(const void *data, size_t len, char out[41])
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    const unsigned char *p = data;
    unsigned char block[64];
    uint64_t bits = (uint64_t)len * 8;
    size_t rest;
    int i;

    for (; len >= 64; p += 64, len -= 64)
        sha1_block(h, p);
    rest = len;
    memset(block, 0, sizeof(block));
    memcpy(block, p, rest);
    block[rest] = 0x80;
    if (rest >= 56) {
        sha1_block(h, block);
        memset(block, 0, sizeof(block));
    }
    for (i = 0; i < 8; i++)
        block[63 - i] = (unsigned char)(bits >> (8 * i));
    sha1_block(h, block);
    for (i = 0; i < 20; i++)
        snprintf(out + 2 * i, 3, "%02x", (unsigned)(h[i / 4] >> (24 - 8 * (i % 4))) & 0xff);
}

void ve_vtun_password(const char mac[18], char out[41])
{
    char line[19];

    /* "echo "$default_mac" | sha1sum" in OpenIPC's tunnel: the newline is hashed too */
    snprintf(line, sizeof(line), "%s\n", mac);
    ve_sha1_hex(line, strlen(line), out);
}
