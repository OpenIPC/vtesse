#!/bin/sh
# vtesse-cli --tap --hook scripts/vtesse-hook.sh
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
# dhclient is a fallback: its stock script may rewrite /etc/resolv.conf with
# the tunnel DNS and install a default route through the tunnel.
# For type tun sessions configure addresses here instead (no DHCP over L3).

IF=$VTESSE_IFNAME
PIDFILE=/var/run/vtesse-dhcp.$IF.pid
UDHCPC_SCRIPT=$(dirname "$0")/vtesse-udhcpc.script

dhcp_start() {
    if command -v udhcpc >/dev/null 2>&1; then
        udhcpc -i "$IF" -b -p "$PIDFILE" -s "$UDHCPC_SCRIPT" -O staticroutes -O msstaticroutes
    elif command -v dhclient >/dev/null 2>&1; then
        dhclient -nw -pf "$PIDFILE" -lf "/var/lib/dhcp/vtesse.$IF.leases" "$IF"
    else
        echo "vtesse-hook: no DHCP client (udhcpc or dhclient) found" >&2
        exit 1
    fi
}

dhcp_pid() {
    [ -f "$PIDFILE" ] && cat "$PIDFILE"
}

case "$1" in
    up)
        [ "$VTESSE_LINK" = ether ] && dhcp_start
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
