#include "netcore/route.h"
#include "unit.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static void test_parse_121(void)
{
    /* 10.78.0.0/24 via 10.77.0.1, 10.79.0.0/24 via 10.77.0.1 (bench) */
    static const uint8_t opt[] = {
        24, 10, 78, 0, 10, 77, 0, 1,
        24, 10, 79, 0, 10, 77, 0, 1
    };
    nc_routes_t rt;
    char s[48];

    CHECK_EQ_INT(nc_routes_parse_classless(opt, sizeof(opt), &rt), 0);
    CHECK_EQ_INT(rt.count, 2);
    CHECK_EQ_INT(rt.r[0].net, IP(10, 78, 0, 0));
    CHECK_EQ_INT(rt.r[0].prefix, 24);
    CHECK_EQ_INT(rt.r[0].gw, IP(10, 77, 0, 1));
    nc_route_format(&rt.r[1], s, sizeof(s));
    CHECK_EQ_STR(s, "10.79.0.0/24 via 10.77.0.1");
}

static void test_parse_widths(void)
{
    static const uint8_t opt[] = {
        0, 192, 168, 1, 1,                  /* default via 192.168.1.1 */
        8, 10, 192, 168, 1, 2,              /* 10.0.0.0/8 */
        32, 1, 2, 3, 4, 0, 0, 0, 0,         /* host route, on-link */
        20, 172, 16, 16, 192, 168, 1, 3     /* 172.16.16.0/20, 3 significant octets */
    };
    nc_routes_t rt;
    char s[48];

    CHECK_EQ_INT(nc_routes_parse_classless(opt, sizeof(opt), &rt), 0);
    CHECK_EQ_INT(rt.count, 4);
    CHECK_EQ_INT(rt.r[0].prefix, 0);
    CHECK_EQ_INT(rt.r[0].mask, 0);
    CHECK_EQ_INT(rt.r[1].net, IP(10, 0, 0, 0));
    CHECK_EQ_INT(rt.r[2].net, IP(1, 2, 3, 4));
    CHECK_EQ_INT(rt.r[2].gw, 0);
    nc_route_format(&rt.r[2], s, sizeof(s));
    CHECK_EQ_STR(s, "1.2.3.4/32 on-link");
    CHECK_EQ_INT(rt.r[3].net, IP(172, 16, 16, 0));
    CHECK_EQ_INT(rt.r[3].mask, 0xfffff000u);

    /* longest prefix wins, default catches the rest */
    CHECK(nc_routes_lookup(&rt, IP(1, 2, 3, 4)) == &rt.r[2]);
    CHECK(nc_routes_lookup(&rt, IP(10, 1, 1, 1)) == &rt.r[1]);
    CHECK(nc_routes_lookup(&rt, IP(172, 16, 31, 255)) == &rt.r[3]);
    CHECK(nc_routes_lookup(&rt, IP(172, 16, 32, 0)) == &rt.r[0]);
}

static void test_parse_malformed(void)
{
    static const uint8_t truncated[] = { 24, 10, 78, 0, 10, 77, 0 };
    static const uint8_t width33[] = { 33, 1, 2, 3, 4, 5, 1, 2, 3, 4 };
    static const uint8_t trailing[] = { 24, 10, 78, 0, 10, 77, 0, 1, 24 };
    nc_routes_t rt;

    CHECK_EQ_INT(nc_routes_parse_classless(truncated, sizeof(truncated), &rt), -1);
    CHECK_EQ_INT(rt.count, 0);
    CHECK_EQ_INT(nc_routes_parse_classless(width33, sizeof(width33), &rt), -1);
    CHECK_EQ_INT(nc_routes_parse_classless(trailing, sizeof(trailing), &rt), -1);
    CHECK_EQ_INT(nc_routes_parse_classless(truncated, 0, &rt), 0);    /* empty is valid */
    CHECK_EQ_INT(rt.count, 0);
}

static void test_lookup_none(void)
{
    nc_routes_t rt;

    memset(&rt, 0, sizeof(rt));
    nc_routes_add(&rt, IP(10, 78, 0, 77), 24, IP(10, 77, 0, 1));   /* host bits masked */
    CHECK_EQ_INT(rt.r[0].net, IP(10, 78, 0, 0));
    CHECK(nc_routes_lookup(&rt, IP(10, 78, 0, 10)) != NULL);
    CHECK(nc_routes_lookup(&rt, IP(8, 8, 8, 8)) == NULL);
}

static void test_devid(void)
{
    uint8_t mac[6];

    CHECK_EQ_INT(nc_devid_to_mac("02BE0C000001", mac), 0);
    CHECK(mac[0] == 0x02 && mac[1] == 0xbe && mac[5] == 0x01);
    CHECK_EQ_INT(nc_devid_to_mac("02be0c000001", mac), -1);     /* lower case */
    CHECK_EQ_INT(nc_devid_to_mac("02:BE:0C:00:00:01", mac), -1);
    CHECK_EQ_INT(nc_devid_to_mac("02BE0C00000", mac), -1);      /* 11 chars */
    CHECK_EQ_INT(nc_devid_to_mac("02BE0C0000011", mac), -1);    /* 13 chars */
    CHECK_EQ_INT(nc_devid_to_mac("01BE0C000001", mac), -1);     /* multicast */
    CHECK_EQ_INT(nc_devid_to_mac("02BE0C00000G", mac), -1);
    CHECK_EQ_INT(nc_devid_to_mac(NULL, mac), -1);
}

int main(void)
{
    RUN(test_parse_121);
    RUN(test_parse_widths);
    RUN(test_parse_malformed);
    RUN(test_lookup_none);
    RUN(test_devid);
    return UNIT_SUMMARY();
}
