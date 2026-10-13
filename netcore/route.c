#include "netcore/route.h"

#include <stdio.h>
#include <string.h>

static uint32_t prefix_mask(uint8_t prefix)
{
    return prefix ? 0xffffffffu << (32 - prefix) : 0;
}

int nc_routes_add(nc_routes_t *rt, uint32_t net, uint8_t prefix, uint32_t gw)
{
    nc_route_t *r;

    if (prefix > 32 || rt->count >= NC_ROUTES_MAX)
        return -1;
    r = &rt->r[rt->count++];
    r->mask = prefix_mask(prefix);
    r->net = net & r->mask;
    r->gw = gw;
    r->prefix = prefix;
    return 0;
}

int nc_routes_parse_classless(const uint8_t *opt, size_t len, nc_routes_t *out)
{
    size_t i = 0;

    memset(out, 0, sizeof(*out));
    while (i < len) {
        uint8_t width = opt[i++];
        size_t n = (size_t)(width + 7) / 8, k;
        uint32_t net = 0, gw;

        if (width > 32 || i + n + 4 > len)
            goto bad;
        for (k = 0; k < n; k++)
            net |= (uint32_t)opt[i + k] << (24 - 8 * k);
        i += n;
        gw = ((uint32_t)opt[i] << 24) | ((uint32_t)opt[i + 1] << 16) |
             ((uint32_t)opt[i + 2] << 8) | opt[i + 3];
        i += 4;
        if (nc_routes_add(out, net, width, gw) < 0)
            goto bad;
    }
    return 0;
bad:
    memset(out, 0, sizeof(*out));
    return -1;
}

const nc_route_t *nc_routes_lookup(const nc_routes_t *rt, uint32_t dst)
{
    const nc_route_t *best = NULL;
    int i;

    for (i = 0; i < rt->count; i++) {
        const nc_route_t *r = &rt->r[i];
        if ((dst & r->mask) == r->net && (!best || r->prefix > best->prefix))
            best = r;
    }
    return best;
}

void nc_ip4_format(uint32_t ip, char *out, size_t size)
{
    snprintf(out, size, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 255,
             (unsigned)(ip >> 8) & 255, (unsigned)ip & 255);
}

void nc_route_format(const nc_route_t *r, char *out, size_t size)
{
    char net[16], gw[16];

    nc_ip4_format(r->net, net, sizeof(net));
    nc_ip4_format(r->gw, gw, sizeof(gw));
    if (r->gw)
        snprintf(out, size, "%s/%u via %s", net, (unsigned)r->prefix, gw);
    else
        snprintf(out, size, "%s/%u on-link", net, (unsigned)r->prefix);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int nc_devid_to_mac(const char *id, uint8_t mac[6])
{
    int i;

    if (!id || strlen(id) != 12)
        return -1;
    for (i = 0; i < 6; i++) {
        int hi = hexval(id[i * 2]), lo = hexval(id[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        mac[i] = (uint8_t)(hi << 4 | lo);
    }
    if (mac[0] & 1)
        return -1;  /* multicast */
    return 0;
}
