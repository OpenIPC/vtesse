#include <stdint.h>

#include "netcore/tunnel.h"
#include "unit.h"
#include "vtesse/vtun_proto.h"

/* Captured from the stock VTun 3.0.2 client on the bench (bench/vectors). */
#define REF_PASSWORD "c79e01d2e49d80eafd192c043c5142bd5a65b5a9"
#define REF_CHAL     "OK CHAL: <djhdglocepfoaejahdbnealodcbnjgbl>\n"
#define REF_RESP     "<fkeefcihhpgpgakcbgcjhjnkakcnpdhk>"

static void test_mesg_build(void)
{
    uint8_t m[VT_MESG_SIZE];
    char name43[44];
    int i;

    CHECK_EQ_INT(vt_mesg_build(m, "HOST: ", "02BE0C000001"), 0);
    CHECK(memcmp(m, "HOST: 02BE0C000001\n", 19) == 0);
    for (i = 19; i < VT_MESG_SIZE; i++)
        CHECK_EQ_INT(m[i], 0);

    memset(name43, 'A', 43);
    name43[43] = '\0';
    CHECK_EQ_INT(vt_mesg_build(m, "HOST: ", name43), -1);   /* 6 + 43 + 1 > 49 */
    name43[42] = '\0';
    CHECK_EQ_INT(vt_mesg_build(m, "HOST: ", name43), 0);    /* exactly 49 */
    CHECK_EQ_INT(m[48], '\n');
    CHECK_EQ_INT(m[49], 0);
}

static void test_session_name(void)
{
    char n[44];

    CHECK(vt_session_name_valid("02BE0C000001"));
    CHECK(!vt_session_name_valid(""));
    CHECK(!vt_session_name_valid("a b"));
    CHECK(!vt_session_name_valid("a:b"));
    CHECK(!vt_session_name_valid("a\nb"));
    memset(n, 'x', 43);
    n[43] = '\0';
    CHECK(!vt_session_name_valid(n));
    n[42] = '\0';
    CHECK(vt_session_name_valid(n));
}

static void test_chal_codec(void)
{
    uint8_t c[VT_CHAL_SIZE], d[VT_CHAL_SIZE];
    char text[VT_CHAL_TEXT_SIZE];
    int i;

    for (i = 0; i < VT_CHAL_SIZE; i++)
        c[i] = (uint8_t)(i * 17 + 3);
    c[0] = 0x00;
    c[1] = 0xff;
    vt_chal_encode(c, text);
    CHECK_EQ_INT(strlen(text), 34);
    CHECK(strncmp(text, "<aapp", 5) == 0);
    CHECK_EQ_INT(vt_chal_decode(text, d), 0);
    CHECK(memcmp(c, d, sizeof(c)) == 0);

    CHECK_EQ_INT(vt_chal_decode("OK CHAL: <abc>", d), -1);
    CHECK_EQ_INT(vt_chal_decode("OK CHAL: no bracket", d), -1);
    CHECK_EQ_INT(vt_chal_decode("OK CHAL: <djhdglocepfoaejahdbnealodcbnjgbq>", d), -1); /* 'q' */
    CHECK_EQ_INT(vt_chal_decode("OK CHAL: <djhdglocepfoaejahdbnealodcbnjgbll>", d), -1); /* 33 chars */
}

static void test_xor_reference(void)
{
    uint8_t c[VT_CHAL_SIZE];
    char text[VT_CHAL_TEXT_SIZE];

    CHECK_EQ_INT(vt_chal_decode(REF_CHAL, c), 0);
    vt_chal_xor(c, REF_PASSWORD);
    vt_chal_encode(c, text);
    CHECK_EQ_STR(text, REF_RESP);
}

static void test_xor_password_lengths(void)
{
    uint8_t c[VT_CHAL_SIZE], z[VT_CHAL_SIZE];
    int i;

    /* short password repeats */
    memset(c, 0, sizeof(c));
    vt_chal_xor(c, "ab");
    for (i = 0; i < VT_CHAL_SIZE; i++)
        CHECK_EQ_INT(c[i], i % 2 ? 'b' : 'a');

    /* only the first 16 characters matter */
    memset(c, 0x55, sizeof(c));
    memset(z, 0x55, sizeof(z));
    vt_chal_xor(c, "0123456789abcdefXYZ");
    vt_chal_xor(z, "0123456789abcdef");
    CHECK(memcmp(c, z, sizeof(c)) == 0);
}

static void test_flags(void)
{
    vt_flags_t f;
    char out[64];

    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TeK>\n", &f), 0);
    CHECK_EQ_INT(f.transport, 'T');
    CHECK_EQ_INT(f.type, 'e');
    CHECK_EQ_INT(f.keepalive, 1);
    CHECK_EQ_INT(vt_flags_check(&f), TUNNEL_OK);
    vt_flags_format(&f, out, sizeof(out));
    CHECK_EQ_STR(out, "<TeK>");

    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TuK>\n", &f), 0);
    CHECK_EQ_INT(vt_flags_check(&f), TUNNEL_OK);

    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TeS512K>\n", &f), 0);
    CHECK_EQ_INT(f.shaper, 512);
    CHECK_EQ_INT(vt_flags_check(&f), TUNNEL_OK);
    vt_flags_format(&f, out, sizeof(out));
    CHECK_EQ_STR(out, "<TeS512K>");

    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TeC9K>\n", &f), 0);
    CHECK_EQ_INT(vt_flags_check(&f), TUNNEL_ERR_COMPRESSION);
    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TuL1K>\n", &f), 0);
    CHECK_EQ_INT(vt_flags_check(&f), TUNNEL_ERR_COMPRESSION);

    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TuKE1>\n", &f), 0);
    CHECK_EQ_INT(f.cipher, 1);
    CHECK_EQ_INT(vt_flags_check(&f), TUNNEL_ERR_ENCRYPTION);
    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TuKE>\n", &f), 0);   /* legacy form */
    CHECK_EQ_INT(f.cipher, 0);
    CHECK_EQ_INT(vt_flags_check(&f), TUNNEL_ERR_ENCRYPTION);

    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <UuK>\n", &f), 0);
    CHECK_EQ_INT(vt_flags_check(&f), TUNNEL_ERR_UNSUPPORTED_TRANSPORT);
    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <Tt>\n", &f), 0);
    CHECK_EQ_INT(vt_flags_check(&f), TUNNEL_ERR_UNSUPPORTED_TYPE);
    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TpK>\n", &f), 0);
    CHECK_EQ_INT(vt_flags_check(&f), TUNNEL_ERR_UNSUPPORTED_TYPE);
    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TFeK>\n", &f), 0);   /* F reserved */

    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TeX>\n", &f), -1);   /* unknown */
    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TeK\n", &f), -1);    /* no '>' */
    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: TeK\n", &f), -1);     /* no '<' */
    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TeCK>\n", &f), -1);  /* C without level */
    CHECK_EQ_INT(vt_flags_parse("OK FLAGS: <TeSK>\n", &f), -1);  /* S without speed */
}

static void test_header(void)
{
    uint8_t b[2];

    vt_hdr_put(b, 1514);
    CHECK_EQ_INT(b[0], 0x05);
    CHECK_EQ_INT(b[1], 0xea);
    CHECK_EQ_INT(vt_hdr_get(b), 1514);
    vt_hdr_put(b, VT_ECHO_REQ);
    CHECK_EQ_INT(b[0], 0x20);
    CHECK_EQ_INT(b[1], 0x00);
    CHECK_EQ_INT(vt_hdr_get(b) & VT_FSIZE_MASK, 0);
}

int main(void)
{
    RUN(test_mesg_build);
    RUN(test_session_name);
    RUN(test_chal_codec);
    RUN(test_xor_reference);
    RUN(test_xor_password_lengths);
    RUN(test_flags);
    RUN(test_header);
    return UNIT_SUMMARY();
}
