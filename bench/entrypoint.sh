#!/bin/sh
# VTesse bench entrypoint.
#
#   server                         VTun server + bridge segment + DHCP + remote device
#   client <session> <password> <server-host> [port]
#                                  stock vtund client (reference), tap + dhclient
#
# Bench addressing (test values only):
#   bridge segment  10.77.0.0/24  bridge/router/DHCP 10.77.0.1, leases .100-.199 (infinite)
#   device subnet   10.78.0.0/24  router 10.78.0.1, device 10.78.0.10 (HTTP :80, RTSP :554)
#   option 121      10.78.0.0/24 and 10.79.0.0/24 (unreachable) via 10.77.0.1
set -eu

log() { echo "[bench] $*"; }

ensure_tun() {
    mkdir -p /dev/net
    [ -c /dev/net/tun ] || mknod /dev/net/tun c 10 200
}

# vtund logs only to syslog; forward /dev/log to stdout.
start_syslog() {
    python3 /opt/device/syslog_stdout.py &
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -S /dev/log ] && return 0; sleep 0.2; done
    log "warning: /dev/log not ready"
}

setup_segment() {
    sysctl -qw net.ipv4.ip_forward=1

    ip link add br-vtesse type bridge
    ip addr add 10.77.0.1/24 dev br-vtesse
    ip link set br-vtesse up

    ip netns add device
    ip link add rt0 type veth peer name dev0
    ip link set dev0 netns device
    ip addr add 10.78.0.1/24 dev rt0
    ip link set rt0 up
    ip -n device addr add 10.78.0.10/24 dev dev0
    ip -n device link set lo up
    ip -n device link set dev0 up
    ip -n device route add default via 10.78.0.1

    mkdir -p /opt/device/www
    echo "vtesse bench device" > /opt/device/www/index.html
    ip netns exec device python3 -m http.server 80 --bind 10.78.0.10 --directory /opt/device/www \
        >/var/log/device-http.log 2>&1 &
    ip netns exec device python3 /opt/device/rtsp_stub.py 10.78.0.10 554 \
        >/var/log/device-rtsp.log 2>&1 &

    # Default dnsmasq behaviour pings an address before offering it, which
    # delays the OFFER by ~3 s (three ARP probes), like the target server.
    dnsmasq --port=0 --interface=br-vtesse --bind-interfaces \
        --dhcp-range=10.77.0.100,10.77.0.199,255.255.255.0,infinite \
        --dhcp-option=3,10.77.0.1 \
        --dhcp-option=6,10.77.0.1 \
        --dhcp-option=121,10.78.0.0/24,10.77.0.1,10.79.0.0/24,10.77.0.1 \
        --dhcp-leasefile=/var/lib/misc/dnsmasq.leases \
        --log-dhcp --log-facility=- &
}

run_server() {
    ensure_tun
    start_syslog
    setup_segment
    # The upstream build script skips `make install`; the lock directory must exist
    # or lock_host() fails and every login gets ERR.
    mkdir -p /var/lock/vtund /var/log/vtund
    log "starting vtund server"
    exec /opt/sbin/vtund -n -s -f /etc/vtund.conf
}

run_client() {
    session=$1 password=$2 server=$3 port=${4:-5000}
    ensure_tun
    start_syslog
    mkdir -p /var/lock/vtund /var/log/vtund
    cat > /etc/vtund-client.conf <<EOF
options { port $port; timeout 60; ip /sbin/ip; }
$session {
  password $password;
  persist no;
  up {
    ip "link set %% up mtu 1500";
    program "/opt/client/up.sh %%";
  };
}
EOF
    log "starting stock vtund client for $session -> $server:$port"
    exec /opt/sbin/vtund -n -f /etc/vtund-client.conf "$session" "$server"
}

case "${1:-server}" in
    server) run_server ;;
    client) shift; run_client "$@" ;;
    *) exec "$@" ;;
esac
