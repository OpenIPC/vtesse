/*
 * VTun wire format helpers (verified against VTun 3.0.2).
 * Pure functions, no I/O, explicit byte order.
 */
#ifndef VTESSE_VTUN_PROTO_H
#define VTESSE_VTUN_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define VT_MESG_SIZE        50      /* every control message, NUL padded */
#define VT_CHAL_SIZE        16
#define VT_CHAL_TEXT_SIZE   (VT_CHAL_SIZE * 2 + 3)  /* "<" + 32 + ">" + NUL */
#define VT_FRAME_SIZE       2048    /* max payload we send */
#define VT_FRAME_OVERHEAD   100     /* receivers accept up to SIZE + OVERHEAD */
#define VT_FRAME_MAX_RX     (VT_FRAME_SIZE + VT_FRAME_OVERHEAD)
#define VT_FSIZE_MASK       0x0fff
#define VT_FLAGS_MASK       0xf000
#define VT_CONN_CLOSE       0x1000
#define VT_ECHO_REQ         0x2000
#define VT_ECHO_REP         0x4000
#define VT_BAD_FRAME        0x8000  /* never on the wire in 3.0.2 */
#define VT_SESSION_MAX      42      /* "HOST: " + name + "\n" must fit 49 bytes */

/* Session names: 1..42 printable characters, no space, ':', control chars. */
int vt_session_name_valid(const char *name);

/* Build "<prefix><arg>\n" padded with NUL to 50 bytes. -1 if longer than 49. */
int vt_mesg_build(uint8_t out[VT_MESG_SIZE], const char *prefix, const char *arg);

/* Challenge text "<" + 32 chars 'a'..'p' + ">" <-> 16 bytes. */
void vt_chal_encode(const uint8_t chal[VT_CHAL_SIZE], char out[VT_CHAL_TEXT_SIZE]);
/* Finds '<' in msg; requires exactly 32 chars 'a'..'p' followed by '>'. 0 on success. */
int  vt_chal_decode(const char *msg, uint8_t chal[VT_CHAL_SIZE]);

/* XOR challenge response (servers built without OpenSSL). pwd must be non-empty. */
void vt_chal_xor(uint8_t chal[VT_CHAL_SIZE], const char *pwd);

typedef struct vt_flags {
    char transport;     /* 'T' or 'U', 0 if absent */
    char type;          /* 'u' tun, 'e' ether, 't' tty, 'p' pipe, 0 if absent */
    int  keepalive;
    int  shaper;        /* S<n>, 0 if absent */
    int  zlib;          /* C<n> present */
    int  lzo;           /* L<n> present */
    int  zlevel;
    int  encrypt;       /* E or E<n> present */
    int  cipher;        /* 0 = legacy */
} vt_flags_t;

/* Parse "OK FLAGS: <...>". Same grammar as VTun cf2bf: unknown characters,
 * a missing number after C/L/S or a missing '>' are errors. 0 on success. */
int vt_flags_parse(const char *msg, vt_flags_t *f);

/* Policy check: TCP, type tun or ether, no compression, no encryption.
 * Returns TUNNEL_OK or a TUNNEL_ERR_* code. */
int vt_flags_check(const vt_flags_t *f);

/* Format flags back as "<...>" in VTun server order (for logs/tests). */
void vt_flags_format(const vt_flags_t *f, char *out, size_t size);

static inline void vt_hdr_put(uint8_t b[2], uint16_t v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

static inline uint16_t vt_hdr_get(const uint8_t b[2])
{
    return (uint16_t)((b[0] << 8) | b[1]);
}

#endif
