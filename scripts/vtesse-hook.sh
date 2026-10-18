#!/bin/sh
# The TAP backend's hook. vtesse carries a copy and runs it when --hook is not
# given (the built-in hook); this file is for --hook FILE, e.g. a copy edited by
# hand ("vtesse --install --scripts" writes one to <prefix>/libexec/vtesse):
#   vtesse-cli --tap --hook scripts/vtesse-hook.sh
#
# Called as "$0 <event>" with VTESSE_IFNAME, VTESSE_LINK (ether|tun),
# VTESSE_SESSION and VTESSE_EVENT in the environment:
#   up         interface created (first tunnel UP): start the DHCP client
#   reconnect  tunnel up again: forget neighbours (the remote bridge MAC may
#              have changed), ask the DHCP client to renew
#   down       tunnel lost (the interface stays, the lease is kept)
#   stop       vtesse-cli exits: stop the DHCP client
#
# DHCP: BusyBox udhcpc with vtesse-udhcpc.script (address and option 121/249
# routes only; DNS and the default route are left alone, see the script).
# dhclient is a fallback for a hook file only (not for the built-in hook): its
# stock script may rewrite /etc/resolv.conf with the tunnel DNS and install a
# default route through the tunnel.
# Manual IPv4 (ip=, gw=, route= in the vtesse-cli config; required for type tun,
# which has no DHCP over L3) arrives as VTESSE_IP (A.B.C.D/P), VTESSE_GW and
# VTESSE_ROUTES ("NET/P NET/P ..."): the address and routes are set here instead of
# DHCP. Without routes and with VTESSE_GW a default route through the tunnel is
# installed only with VTESSE_DEFAULT_ROUTE=1 (as in vtesse-udhcpc.script).

IF=$VTESSE_IFNAME
PIDFILE=/var/run/vtesse-dhcp.$IF.pid
UDHCPC_SCRIPT=$(dirname "$0")/vtesse-udhcpc.script
# The built-in hook gets the path of the binary in VTESSE_SELF: the binary runs
# its built-in copy of vtesse-udhcpc.script when called with VTESSE_RUN set.
if [ -n "$VTESSE_SELF" ]; then
    UDHCPC_SCRIPT=$VTESSE_SELF
    export VTESSE_RUN=udhcpc-script
fi

dhcp_start() {
    if command -v udhcpc >/dev/null 2>&1; then
        udhcpc -i "$IF" -b -p "$PIDFILE" -s "$UDHCPC_SCRIPT" -O staticroutes -O msstaticroutes
    elif [ -n "$VTESSE_SELF" ]; then
        # the built-in hook does not fall back to dhclient: its stock script may
        # replace /etc/resolv.conf and add a default route through the tunnel
        echo "vtesse-hook: no udhcpc found (BusyBox udhcpc, e.g. the udhcpc package);" \
             "install it, or run with --hook FILE / --hook none" >&2
        exit 1
    elif command -v dhclient >/dev/null 2>&1; then
        dhclient -nw -pf "$PIDFILE" -lf "/var/lib/dhcp/vtesse.$IF.leases" "$IF"
    else
        echo "vtesse-hook: no DHCP client (udhcpc or dhclient) found" >&2
        exit 1
    fi
}

static_up() {
    ip addr replace "$VTESSE_IP" dev "$IF"
    for r in $VTESSE_ROUTES; do
        if [ "$VTESSE_LINK" = tun ] || [ -z "$VTESSE_GW" ]; then
            ip route replace "$r" dev "$IF"
        else
            ip route replace "$r" via "$VTESSE_GW" dev "$IF"
        fi
    done
    if [ -z "$VTESSE_ROUTES" ] && [ -n "$VTESSE_GW" ] && [ "${VTESSE_DEFAULT_ROUTE:-0}" = 1 ]; then
        ip route replace default via "$VTESSE_GW" dev "$IF"
    fi
}

dhcp_pid() {
    [ -f "$PIDFILE" ] && cat "$PIDFILE"
}

case "$1" in
    up)
        if [ -n "$VTESSE_IP" ]; then
            static_up
        elif [ "$VTESSE_LINK" = ether ]; then
            dhcp_start
        else
            echo "vtesse-hook: type tun needs ip= (and route=) in the config" >&2
        fi
        ;;
    reconnect)
        ip neigh flush dev "$IF" 2>/dev/null
        pid=$(dhcp_pid) && [ -n "$pid" ] && command -v udhcpc >/dev/null 2>&1 && kill -USR1 "$pid" 2>/dev/null
        ;;
    down)
        ;;
    stop)
        pid=$(dhcp_pid) && [ -n "$pid" ] && kill "$pid" 2>/dev/null
        rm -f "$PIDFILE"
        ;;
esac
exit 0
