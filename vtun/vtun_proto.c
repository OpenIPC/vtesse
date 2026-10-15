#include "vtesse/vtun_proto.h"

#include <stdio.h>
#include <string.h>

#include "netcore/tunnel.h"

static const char chal_chars[] = "abcdefghijklmnop";

int vt_session_name_valid(const char *name)
{
    size_t len;
    const char *p;

    if (!name)
        return 0;
    len = strlen(name);
    if (len == 0 || len > VT_SESSION_MAX)
        return 0;
    for (p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= ' ' || c >= 0x7f || c == ':')
            return 0;
    }
    return 1;
}

int vt_mesg_build(uint8_t out[VT_MESG_SIZE], const char *prefix, const char *arg)
{
    size_t pl = strlen(prefix), al = arg ? strlen(arg) : 0;

    if (pl + al + 1 > VT_MESG_SIZE - 1)
        return -1;
    memset(out, 0, VT_MESG_SIZE);
    memcpy(out, prefix, pl);
    if (al)
        memcpy(out + pl, arg, al);
    out[pl + al] = '\n';
    return 0;
}

void vt_chal_encode(const uint8_t chal[VT_CHAL_SIZE], char out[VT_CHAL_TEXT_SIZE])
{
    int i;

    out[0] = '<';
    for (i = 0; i < VT_CHAL_SIZE; i++) {
        out[1 + i * 2] = chal_chars[chal[i] >> 4];
        out[2 + i * 2] = chal_chars[chal[i] & 0x0f];
    }
    out[1 + VT_CHAL_SIZE * 2] = '>';
    out[2 + VT_CHAL_SIZE * 2] = '\0';
}

int vt_chal_decode(const char *msg, uint8_t chal[VT_CHAL_SIZE])
{
    const char *p = strchr(msg, '<');
    int i;

    if (!p)
        return -1;
    p++;
    for (i = 0; i < VT_CHAL_SIZE * 2; i++)
        if (p[i] < 'a' || p[i] > 'p')
            return -1;
    if (p[VT_CHAL_SIZE * 2] != '>')
        return -1;
    for (i = 0; i < VT_CHAL_SIZE; i++)
        chal[i] = (uint8_t)(((p[i * 2] - 'a') << 4) | (p[i * 2 + 1] - 'a'));
    return 0;
}

void vt_chal_xor(uint8_t chal[VT_CHAL_SIZE], const char *pwd)
{
    size_t len = strlen(pwd);
    int i;

    for (i = 0; i < VT_CHAL_SIZE; i++)
        chal[i] ^= (uint8_t)pwd[i % len];
}

/* Decimal number at *pp (optional when 'optional'); advances *pp. */
static int parse_num(const char **pp, int *out, int optional)
{
    const char *p = *pp;
    long v = 0;
    int digits = 0;

    while (*p >= '0' && *p <= '9') {
        if (v < 100000000)
            v = v * 10 + (*p - '0');
        p++;
        digits++;
    }
    if (!digits && !optional)
        return -1;
    *out = (int)v;
    *pp = p;
    return 0;
}

int vt_flags_parse(const char *msg, vt_flags_t *f)
{
    const char *p = strchr(msg, '<');

    memset(f, 0, sizeof(*f));
    if (!p)
        return -1;
    p++;
    while (*p) {
        char c = *p++;
        int n;

        switch (c) {
        case 't': case 'p': case 'e': case 'u':
            f->type = c;
            break;
        case 'T': case 'U':
            f->transport = c;
            break;
        case 'K':
            f->keepalive = 1;
            break;
        case 'C':
            if (parse_num(&p, &n, 0))
                return -1;
            f->zlib = 1;
            f->zlevel = n;
            break;
        case 'L':
            if (parse_num(&p, &n, 0))
                return -1;
            f->lzo = 1;
            f->zlevel = n;
            break;
        case 'E':
            parse_num(&p, &n, 1);
            f->encrypt = 1;
            f->cipher = n;
            break;
        case 'S':
            if (parse_num(&p, &n, 0))
                return -1;
            f->shaper = n;
            break;
        case 'F':   /* reserved */
            break;
        case '>':
            return 0;
        default:
            return -1;
        }
    }
    return -1;  /* no closing '>' */
}

int vt_flags_check(const vt_flags_t *f)
{
    if (f->zlib || f->lzo)
        return TUNNEL_ERR_COMPRESSION;
    if (f->encrypt)
        return TUNNEL_ERR_ENCRYPTION;
    if (f->transport == 'U')
        return TUNNEL_ERR_UNSUPPORTED_TRANSPORT;
    if (f->type != 'u' && f->type != 'e')
        return TUNNEL_ERR_UNSUPPORTED_TYPE;
    return TUNNEL_OK;
}

void vt_flags_format(const vt_flags_t *f, char *out, size_t size)
{
    char tmp[64];
    size_t n = 0;

    tmp[n++] = '<';
    if (f->transport)
        tmp[n++] = f->transport;
    if (f->type)
        tmp[n++] = f->type;
    if (f->shaper)
        n += (size_t)sprintf(tmp + n, "S%d", f->shaper);
    if (f->zlib)
        n += (size_t)sprintf(tmp + n, "C%d", f->zlevel);
    if (f->lzo)
        n += (size_t)sprintf(tmp + n, "L%d", f->zlevel);
    if (f->keepalive)
        tmp[n++] = 'K';
    if (f->encrypt)
        n += (size_t)sprintf(tmp + n, "E%d", f->cipher);
    tmp[n++] = '>';
    tmp[n] = '\0';
    snprintf(out, size, "%s", tmp);
}
