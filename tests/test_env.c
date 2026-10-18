/* SHA-1 and the VTun password of OpenIPC's tunnel (cli/env.c, #64). */
#include <stdlib.h>

#include "../cli/env.h"
#include "unit.h"

static void sha1(const char *data, size_t len, char out[41])
{
    ve_sha1_hex(data, len, out);
}

static void test_sha1_vectors(void)
{
    static char big[1000000];
    char h[41];

    sha1("", 0, h);
    CHECK_EQ_STR(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    sha1("abc", 3, h);
    CHECK_EQ_STR(h, "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    CHECK_EQ_STR(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    memset(big, 'a', sizeof(big));
    sha1(big, sizeof(big), h);
    CHECK_EQ_STR(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

/* the padding boundaries: 55 bytes fit one block, 56 and 64 need two */
static void test_sha1_padding(void)
{
    char x[64], h[41];

    memset(x, 'x', sizeof(x));
    sha1(x, 55, h);
    CHECK_EQ_STR(h, "cef734ba81a024479e09eb5a75b6ddae62e6abf1");
    sha1(x, 56, h);
    CHECK_EQ_STR(h, "901305367c259952f4e7af8323f480d59f81335b");
    sha1(x, 64, h);
    CHECK_EQ_STR(h, "bb2fa3ee7afb9f54c6dfb5d021f14b1ffe40c163");
}

/* printf '02:BE:0C:00:00:01\n' | sha1sum, as OpenIPC's tunnel computes it */
static void test_vtun_password(void)
{
    char h[41];

    ve_vtun_password("02:BE:0C:00:00:01", h);
    CHECK_EQ_STR(h, "4cfa9bc4a53bbccbc4e09829f6067d42488e66b2");
}

int main(void)
{
    RUN(test_sha1_vectors);
    RUN(test_sha1_padding);
    RUN(test_vtun_password);
    return UNIT_SUMMARY();
}
