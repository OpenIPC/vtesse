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
set -eu

IMAGE=${BENCH_IMAGE:-vtesse-bench}
NET=${BENCH_NET:-vtesse-bench}
SERVER=vtesse-bench-server
CLIENT=vtesse-bench-client
BENCH_PORT=${BENCH_PORT:-15000}
HERE=$(cd "$(dirname "$0")" && pwd)

# Bench-only test session (see vtund.conf).
SESSION=02BE0C000001
PASSWORD=c79e01d2e49d80eafd192c043c5142bd5a65b5a9

cmd_build() { docker build -t "$IMAGE" "$HERE"; }

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

case "${1:-}" in
    build) cmd_build ;;
    up) cmd_up ;;
    down) cmd_down ;;
    logs) cmd_logs ;;
    stock-test) cmd_stock_test ;;
    probe) shift; cmd_probe "$@" ;;
    cli-test) cmd_cli_test ;;
    proxy-test) python3 "$HERE/proxy_test.py" "$HERE/../build/vtesse-cli" "$BENCH_PORT" "$SERVER" ;;
    selftest) python3 "$HERE/selftest.py" 127.0.0.1 "$BENCH_PORT" ;;
    *) sed -n '2,12p' "$0"; exit 1 ;;
esac
