/*
 * Settings in the U-Boot environment (#64), the way OpenIPC keeps the VTun server
 * there (fw_printenv -n vtun): helpdesk access with one command,
 *   fw_setenv vtesse <server>
 * vtesse = host; optional vtesse_port, vtesse_session, vtesse_id_from,
 * vtesse_password, vtesse_ip, vtesse_gw. With only the server, the VTun defaults
 * of OpenIPC's "tunnel" apply: port 5000, session = MAC of the interface with the
 * default route without colons, password = SHA-1 of "<MAC with colons>\n".
 */
#ifndef VTESSE_CLI_ENV_H
#define VTESSE_CLI_ENV_H

#include <stddef.h>

typedef struct ve {
    char host[256];
    char port[16];
    char session[64];
    char id_from[32];
    char password[128];
    char ip[32], gw[32];
    int ok;                 /* fw_printenv ran and succeeded: the environment is usable */
} ve_t;

/* the variables and the configuration keys they hold */
typedef struct ve_var {
    const char *name;       /* U-Boot variable */
    const char *key;        /* /etc/vtesse.conf key */
} ve_var_t;
extern const ve_var_t ve_vars[];
extern const size_t ve_nvars;

/* Reads the vtesse variables with fw_printenv; all empty (ok = 0) without fw_printenv
 * or when it fails. Returns 1 when "vtesse" (the server) is set. */
int ve_load(ve_t *e);

/* 1 when fw_setenv is in PATH */
int ve_can_write(void);

/* fw_setenv -s -: one batch of "NAME VALUE" lines ("NAME" alone deletes it) on stdin,
 * so the values stay out of the process list and are written together; 0 on success */
int ve_set_script(const char *script);

/* The interface of the default route if it has a MAC; -1: none (yet) */
int ve_route_iface(char *out, size_t size);

/* 1 when there is a default route, through any interface */
int ve_has_default_route(void);

/* The interface of the default route (/proc/net/route), as OpenIPC's VTun
 * client picks it, if it has a MAC (PPPoE, WireGuard and tun have none); else
 * eth0 if that has one. -1: none of them. */
int ve_default_iface(char *out, size_t size);

/* The MAC of an interface in upper case with colons ("02:BE:0C:00:00:01"). */
int ve_iface_mac(const char *ifname, char out[18]);

/* SHA-1 of data as 40 lower-case hex digits */
void ve_sha1_hex(const void *data, size_t len, char out[41]);

/* The VTun password of OpenIPC's "tunnel": echo "$MAC" | sha1sum */
void ve_vtun_password(const char mac[18], char out[41]);

#endif
