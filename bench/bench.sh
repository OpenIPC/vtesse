#!/bin/sh
# VTesse bench control.
#
#   bench.sh build           build the image
#   bench.sh up              start the server container (published on 127.0.0.1:$BENCH_PORT)
#   bench.sh down            stop and remove bench containers and network
#   bench.sh logs            follow server logs
#   bench.sh stock-test      reference check with the stock vtund client (tap + dhclient)
#   bench.sh probe [args]    run tools/vtun_probe.py against the bench
#   bench.sh selftest        protocol self-test against the running bench
#   bench.sh cli-test        stage 1 acceptance: build/vtesse-cli (lwIP, DHCP) on the bench
#   bench.sh proxy-test      stage 2 acceptance: SOCKS5 and port forwards of build/vtesse-cli
#   bench.sh tap-test        TAP backend acceptance: vtesse-cli --tap (LWIP=0 build) in a container
#   bench.sh tun-test        type tun session (#8): manual IPv4, lwIP and --tap (TUN interface)
set -eu

IMAGE=${BENCH_IMAGE:-vtesse-bench}
NET=${BENCH_NET:-vtesse-bench}
SERVER=vtesse-bench-server
CLIENT=vtesse-bench-client
BENCH_PORT=${BENCH_PORT:-15000}
HERE=$(cd "$(dirname "$0")" && pwd)

# Bench-only test sessions (see vtund.conf).
SESSION=02BE0C000001
PASSWORD=c79e01d2e49d80eafd192c043c5142bd5a65b5a9
TUN_SESSION=02BE0C000006            # type tun: server 10.76.0.1 <-> client 10.76.0.2
TUN_PASSWORD=3158ed23d81fba0909c1a18d263ca8f4cef1b6a0

# context: the repository root, for server/ (vtund build script and patches)
cmd_build() { docker build -t "$IMAGE" -f "$HERE/Dockerfile" "$HERE/.."; }

cmd_up() {
    docker network inspect "$NET" >/dev/null 2>&1 || docker network create "$NET" >/dev/null
    docker rm -f "$SERVER" >/dev/null 2>&1 || true
    docker run -d --name "$SERVER" --hostname bench-server --privileged \
        --network "$NET" -p "127.0.0.1:$BENCH_PORT:5000" "$IMAGE" server >/dev/null
    echo "server: $SERVER, 127.0.0.1:$BENCH_PORT"
}

cmd_down() {
    docker rm -f "$CLIENT" "$SERVER" >/dev/null 2>&1 || true
    docker network rm "$NET" >/dev/null 2>&1 || true
}

cmd_logs() { docker logs -f "$SERVER"; }

cmd_stock_test() {
    docker rm -f "$CLIENT" >/dev/null 2>&1 || true
    docker run -d --name "$CLIENT" --privileged --network "$NET" "$IMAGE" \
        client "$SESSION" "$PASSWORD" "$SERVER" >/dev/null
    echo "waiting for DHCP on the stock client..."
    i=0
    until docker exec "$CLIENT" sh -c 'ip -4 addr show | grep -q "10\.77\.0\."' 2>/dev/null; do
        i=$((i + 1))
        if [ $i -gt 30 ]; then
            echo "FAIL: no DHCP address"; docker logs "$CLIENT"; exit 1
        fi
        sleep 1
    done
    docker exec "$CLIENT" sh -c '
        set -e
        echo "--- addresses";  ip -4 -br addr show
        echo "--- routes";     ip -4 route show
        echo "--- ping device"; ping -c 2 -W 2 10.78.0.10
        echo "--- http";       curl -fsS --max-time 5 http://10.78.0.10/
        echo "--- rtsp";       printf "OPTIONS rtsp://10.78.0.10/ RTSP/1.0\r\nCSeq: 1\r\n\r\n" \
                                 | timeout 3 python3 -c "import socket,sys;s=socket.create_connection((\"10.78.0.10\",554));s.sendall(sys.stdin.buffer.read());print(s.recv(200).decode().splitlines()[0])"
    '
    echo "PASS: stock client reaches the device through the tunnel"
}

cmd_probe() {
    conf=$(mktemp)
    printf 'host=127.0.0.1\nport=%s\nsession=%s\npassword=%s\nauth=xor\n' \
        "$BENCH_PORT" "$SESSION" "$PASSWORD" >"$conf"
    python3 "$HERE/../tools/vtun_probe.py" --config "$conf" "$@" || rc=$?
    rm -f "$conf"
    return ${rc:-0}
}

cmd_cli_test() {
    cli="$HERE/../build/vtesse-cli"
    [ -x "$cli" ] || { echo "build the CLI first: make"; exit 1; }
    conf=$(mktemp) log=$(mktemp)
    printf 'host=127.0.0.1\nport=%s\nsession=%s\npassword=%s\n' "$BENCH_PORT" "$SESSION" "$PASSWORD" >"$conf"
    fails=0
    check() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }

    "$cli" --config "$conf" --seconds 30 --fetch http://10.78.0.10/ >"$log" 2>&1 &
    pid=$!
    i=0
    until grep -q "^\[.*\] address " "$log" || [ $i -gt 15 ]; do sleep 1; i=$((i + 1)); done
    addr=$(sed -n 's/^\[.*\] address \([0-9.]*\) .*/\1/p' "$log" | head -1)
    check "DHCP address bound" '[ -n "$addr" ]'
    check "option 121 routes applied" 'grep -q "route 10.78.0.0/24 via 10.77.0.1" "$log"'
    check "option 3 ignored with classless routes" 'grep -q "ignored: classless routes present" "$log"'
    sleep 1
    check "HTTP GET to the device through lwIP" 'grep -q "fetch 10.78.0.10:80/: ok.*HTTP/1.0 200 OK" "$log"'
    check "device pings the client" 'docker exec "$SERVER" ip netns exec device ping -c 2 -W 2 "$addr" >/dev/null'
    cmd_probe --seconds 1 >/dev/null 2>&1      # take the session over (multi killold)
    i=0
    until [ "$(grep -c '\] up: ' "$log")" -ge 2 ] || [ $i -gt 15 ]; do sleep 1; i=$((i + 1)); done
    check "reconnect after take-over" '[ "$(grep -c "\] up: " "$log")" -ge 2 ]'
    sleep 2
    check "same address after reconnect, device pings it" \
        'docker exec "$SERVER" ip netns exec device ping -c 2 -W 2 "$addr" >/dev/null'
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    echo "--- client log"; cat "$log"
    rm -f "$conf" "$log"
    echo "$fails failure(s)"
    [ "$fails" -eq 0 ]
}

cmd_tap_test() {
    repo=$(cd "$HERE/.." && pwd)
    tapc=vtesse-bench-tapclient
    fails=0
    check() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
    inc() { docker exec "$tapc" sh -c "$1"; }
    dev() { docker exec "$SERVER" ip netns exec device sh -c "$1"; }

    # build the TAP-only CLI for the bench image (Debian bookworm)
    docker build -q -t vtesse-ci-native -f "$repo/ci/Dockerfile.native" "$repo/ci" >/dev/null
    docker run --rm --user "$(id -u):$(id -g)" -v "$repo:/src" -w /src vtesse-ci-native \
        make -s BUILD=build-bench-tap LWIP=0 all >/dev/null

    docker rm -f "$tapc" >/dev/null 2>&1 || true
    docker run -d --name "$tapc" --privileged --network "$NET" -v "$repo:/src:ro" "$IMAGE" sh -c "
        mkdir -p /var/lib/dhcp
        # device ID from an interface MAC (id_from, #6): a dummy NIC with the session's MAC
        ip link add idsrc0 type dummy
        ip link set idsrc0 address $(echo $SESSION | sed 's/../&:/g; s/:$//')
        printf 'host=$SERVER\nport=5000\nid_from=idsrc0\npassword=$PASSWORD\n' > /tmp/vtesse.conf
        python3 -m http.server 8080 --bind 0.0.0.0 --directory /etc >/dev/null 2>&1 &
        exec /src/build-bench-tap/vtesse-cli --tap --ifname vtesse0 --hook /src/scripts/vtesse-hook.sh --config /tmp/vtesse.conf
    " >/dev/null

    i=0
    until inc 'ip -4 addr show vtesse0 2>/dev/null | grep -q "inet 10\.77\.0\."' || [ $i -gt 20 ]; do sleep 1; i=$((i + 1)); done
    addr=$(inc "ip -4 -o addr show vtesse0" 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p')
    check "device ID taken from the MAC of idsrc0 (id_from)" 'docker logs "$tapc" 2>&1 | grep -q "device ID $SESSION (MAC of idsrc0)"'
    check "tap interface vtesse0 with DHCP address" '[ -n "$addr" ]'
    check "MAC of vtesse0 is the device ID" 'inc "cat /sys/class/net/vtesse0/address" | grep -qi "^02:be:0c:00:00:01$"'
    check "option 121 route via the tunnel, no default route through it" \
        'inc "ip route" | grep -q "10.78.0.0/24 via 10.77.0.1 dev vtesse0" && ! inc "ip route" | grep -q "default.*vtesse0"'
    check "ping the device from the client" 'inc "ping -c 2 -W 2 10.78.0.10" >/dev/null'
    check "HTTP to the device from any process (kernel stack)" 'inc "curl -fsS --max-time 5 http://10.78.0.10/" | grep -q "vtesse bench device"'
    check "inbound: the device pings the client" 'dev "ping -c 2 -W 2 $addr" >/dev/null'
    check "inbound: the device opens TCP to the client (HTTP :8080)" 'dev "curl -fsS --max-time 5 http://$addr:8080/hostname" >/dev/null'
    sha=$(docker exec "$SERVER" cat /opt/device/www/big.sha256)
    check "20 MiB download through the tap, sha256 matches" \
        '[ "$(inc "curl -fsS --max-time 60 http://10.78.0.10/big.bin | sha256sum | cut -d\" \" -f1")" = "$sha" ]'

    cmd_probe --seconds 1 >/dev/null 2>&1     # take the session over (multi killold)
    i=0
    until [ "$(docker logs "$tapc" 2>&1 | grep -c 'carrier on')" -ge 1 ] || [ $i -gt 15 ]; do sleep 1; i=$((i + 1)); done
    check "reconnect after take-over keeps the interface" 'docker logs "$tapc" 2>&1 | grep -q "carrier on"'
    sleep 2
    check "same address after reconnect" 'inc "ip -4 -o addr show vtesse0" | grep -q "inet $addr/"'
    check "traffic works after reconnect (both directions)" \
        'inc "ping -c 2 -W 2 10.78.0.10" >/dev/null && dev "ping -c 2 -W 2 $addr" >/dev/null'

    docker stop -t 5 "$tapc" >/dev/null
    check "clean exit on SIGTERM" 'docker logs "$tapc" 2>&1 | grep -q "frames in"'
    echo "--- client log"; docker logs "$tapc" 2>&1 | grep -v "^\(Internet\|Listening\|Sending\|Copyright\|For info\|DHCP\|bound\)" | tail -20
    docker rm -f "$tapc" >/dev/null 2>&1
    echo "$fails failure(s)"
    [ "$fails" -eq 0 ]
}

# type tun (#8): no DHCP over L3, the client address comes from ip=/gw=/route=.
# lwIP: HTTP to the device through the tunnel and inbound ping of the client;
# --tap (LWIP=0 build, as in tap-test): a TUN interface configured by the hook.
cmd_tun_test() {
    repo=$(cd "$HERE/.." && pwd)
    cli="$repo/build/vtesse-cli"
    tunc=vtesse-bench-tunclient
    [ -x "$cli" ] || { echo "build the CLI first: make"; exit 1; }
    fails=0
    check() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
    srv() { docker exec "$SERVER" sh -c "$1"; }
    dev() { docker exec "$SERVER" ip netns exec device sh -c "$1"; }
    tunconf="ip=10.76.0.2/32\ngw=10.76.0.1\nroute=10.78.0.0/24\n"

    # --- lwIP backend
    conf=$(mktemp) log=$(mktemp)
    printf "host=127.0.0.1\nport=%s\nsession=%s\npassword=%s\n$tunconf" \
        "$BENCH_PORT" "$TUN_SESSION" "$TUN_PASSWORD" >"$conf"
    "$cli" --config "$conf" --seconds 15 --fetch http://10.78.0.10/ >"$log" 2>&1 &
    pid=$!
    i=0
    until grep -q "fetch 10.78.0.10" "$log" || [ $i -gt 10 ]; do sleep 1; i=$((i + 1)); done
    check "lwIP: session flags <TuK>" 'grep -q "flags <TuK>" "$log"'
    check "lwIP: manual address 10.76.0.2, route 10.78.0.0/24, no default route" \
        'grep -q "address 10.76.0.2 " "$log" && grep -q "route 10.78.0.0/24" "$log" && ! grep -q "(default route)" "$log"'
    check "lwIP: HTTP GET to the device through the tun session" 'grep -q "fetch 10.78.0.10:80/: ok.*200 OK" "$log"'
    check "lwIP: the server pings the client (10.76.0.2)" 'srv "ping -c 2 -W 2 10.76.0.2" >/dev/null'
    check "lwIP: the device pings the client" 'dev "ping -c 2 -W 2 10.76.0.2" >/dev/null'
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    echo "--- lwIP client log"; cat "$log"
    rm -f "$conf" "$log"
    sleep 1

    # --- TAP backend: TUN interface, addressing by the hook
    docker build -q -t vtesse-ci-native -f "$repo/ci/Dockerfile.native" "$repo/ci" >/dev/null
    docker run --rm --user "$(id -u):$(id -g)" -v "$repo:/src" -w /src vtesse-ci-native \
        make -s BUILD=build-bench-tap LWIP=0 all >/dev/null
    docker rm -f "$tunc" >/dev/null 2>&1 || true
    docker run -d --name "$tunc" --privileged --network "$NET" -v "$repo:/src:ro" "$IMAGE" sh -c "
        printf 'host=$SERVER\nport=5000\nsession=$TUN_SESSION\npassword=$TUN_PASSWORD\n$tunconf' > /tmp/vtesse.conf
        exec /src/build-bench-tap/vtesse-cli --tap --ifname vtun0 --hook /src/scripts/vtesse-hook.sh --config /tmp/vtesse.conf
    " >/dev/null
    inc() { docker exec "$tunc" sh -c "$1"; }
    i=0
    until inc 'ip -4 addr show vtun0 2>/dev/null | grep -q "inet 10\.76\.0\.2"' || [ $i -gt 15 ]; do sleep 1; i=$((i + 1)); done
    check "tap: vtun0 is a TUN (L3) interface" 'inc "ip -d link show vtun0" | grep -q "tun type tun"'
    check "tap: hook set 10.76.0.2 and the route to 10.78.0.0/24" \
        'inc "ip -4 addr show vtun0" | grep -q "inet 10.76.0.2/32" && inc "ip route" | grep -q "10.78.0.0/24 dev vtun0"'
    check "tap: no default route through the tunnel" '! inc "ip route" | grep -q "default.*vtun0"'
    check "tap: ping the device from the client" 'inc "ping -c 2 -W 2 10.78.0.10" >/dev/null'
    check "tap: HTTP to the device from any process" 'inc "curl -fsS --max-time 5 http://10.78.0.10/" | grep -q "vtesse bench device"'
    check "tap: inbound: the device pings the client" 'dev "ping -c 2 -W 2 10.76.0.2" >/dev/null'
    docker stop -t 5 "$tunc" >/dev/null
    echo "--- tap client log"; docker logs "$tunc" 2>&1 | tail -8
    docker rm -f "$tunc" >/dev/null 2>&1
    echo "$fails failure(s)"
    [ "$fails" -eq 0 ]
}

case "${1:-}" in
    build) cmd_build ;;
    up) cmd_up ;;
    down) cmd_down ;;
    logs) cmd_logs ;;
    stock-test) cmd_stock_test ;;
    probe) shift; cmd_probe "$@" ;;
    cli-test) cmd_cli_test ;;
    tap-test) cmd_tap_test ;;
    tun-test) cmd_tun_test ;;
    proxy-test) python3 "$HERE/proxy_test.py" "$HERE/../build/vtesse-cli" "$BENCH_PORT" "$SERVER" ;;
    selftest) python3 "$HERE/selftest.py" 127.0.0.1 "$BENCH_PORT" ;;
    *) sed -n '2,14p' "$0"; exit 1 ;;
esac
