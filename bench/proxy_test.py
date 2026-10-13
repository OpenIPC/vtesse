#!/usr/bin/env python3
"""Stage 2 acceptance: SOCKS5 and port forwarding of build/vtesse-cli on the bench.

Usage: proxy_test.py <vtesse-cli> <bench-port> <server-container>
The bench must be up (bench.sh up). Uses the main bench session.
"""
import hashlib
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

CLI, BENCH_PORT, SERVER = sys.argv[1], int(sys.argv[2]), sys.argv[3]
SESSION = "02BE0C000001"
PASSWORD = "c79e01d2e49d80eafd192c043c5142bd5a65b5a9"
DEVICE = "10.78.0.10"
SOCKS = 11080
FWD_HTTP, FWD_RTSP, FWD_ECHO, FWD_CLOSED = 18080, 18554, 18007, 18009

failures = 0


def check(name, cond, detail=""):
    global failures
    print("%s  %s%s" % ("PASS" if cond else "FAIL", name, ("  (%s)" % detail) if detail else ""), flush=True)
    if not cond:
        failures += 1


class Cli:
    def __init__(self, extra, conf_extra=""):
        fd, self.conf = tempfile.mkstemp()
        with os.fdopen(fd, "w") as f:
            f.write("host=127.0.0.1\nport=%d\nsession=%s\npassword=%s\n%s"
                    % (BENCH_PORT, SESSION, PASSWORD, conf_extra))
        self.lines = []
        self.p = subprocess.Popen([CLI, "--config", self.conf] + extra,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(line.rstrip())

    def wait_for(self, text, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            if any(text in l for l in self.lines):
                return True
            time.sleep(0.1)
        return False

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(10)
        except subprocess.TimeoutExpired:
            self.p.kill()
        time.sleep(0.3)
        os.unlink(self.conf)


def recv_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError("EOF after %d bytes" % len(b))
        b += c
    return b


def socks_connect(atyp, addr, port, auth=None, timeout=20):
    """Returns (socket or None, reply code)."""
    s = socket.create_connection(("127.0.0.1", SOCKS), timeout=timeout)
    s.sendall(b"\x05\x01" + (b"\x02" if auth else b"\x00"))
    m = recv_exact(s, 2)
    if m[1] == 0xFF:
        s.close()
        return None, 0xFF
    if auth:
        u, p = auth
        s.sendall(b"\x01" + bytes([len(u)]) + u.encode() + bytes([len(p)]) + p.encode())
        if recv_exact(s, 2)[1] != 0:
            s.close()
            return None, -1
    if atyp == 1:
        a = socket.inet_aton(addr)
    elif atyp == 3:
        a = bytes([len(addr)]) + addr.encode()
    else:
        a = socket.inet_pton(socket.AF_INET6, addr)
    s.sendall(b"\x05\x01\x00" + bytes([atyp]) + a + struct.pack(">H", port))
    r = recv_exact(s, 10)
    if r[1] != 0:
        s.close()
        return None, r[1]
    return s, 0


def http_get(s, path):
    s.sendall(("GET %s HTTP/1.0\r\nHost: %s\r\n\r\n" % (path, DEVICE)).encode())
    data = b""
    while True:
        c = s.recv(65536)
        if not c:
            break
        data += c
    s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    return head.split(b"\r\n")[0].decode(), body


def big_sha():
    return subprocess.check_output(["docker", "exec", SERVER, "cat", "/opt/device/www/big.sha256"],
                                   text=True).strip()


def main():
    want_sha = big_sha()
    cli = Cli(["--socks", str(SOCKS),
               "--forward", "%d:%s:80" % (FWD_HTTP, DEVICE),
               "--forward", "%d:%s:554" % (FWD_RTSP, DEVICE),
               "--forward", "%d:%s:7" % (FWD_ECHO, DEVICE),
               "--forward", "%d:%s:9" % (FWD_CLOSED, DEVICE)])
    try:
        check("tunnel up and DHCP address bound", cli.wait_for("] address "))

        s, rep = socks_connect(1, DEVICE, 80)
        status, body = http_get(s, "/") if s else ("", b"")
        check("socks5 CONNECT (IPv4) -> HTTP 200", status.endswith("200 OK") and b"vtesse bench device" in body, status)

        s, rep = socks_connect(3, DEVICE, 80)
        check("socks5 domain field with an IPv4 literal accepted", rep == 0)
        if s:
            s.close()

        _, rep = socks_connect(3, "device.example", 80)
        check("socks5 host name rejected: address type not supported (8)", rep == 8, "rep=%d" % rep)
        _, rep = socks_connect(4, "2001:db8::1", 80)
        check("socks5 IPv6 rejected (8)", rep == 8, "rep=%d" % rep)
        _, rep = socks_connect(1, "8.8.8.8", 80)
        check("destination without a route: network unreachable (3)", rep == 3, "rep=%d" % rep)
        _, rep = socks_connect(1, DEVICE, 9)
        check("closed port behind the tunnel: connection refused (5)", rep == 5, "rep=%d" % rep)

        r = subprocess.run(["curl", "-sS", "--max-time", "10", "--socks5", "127.0.0.1:%d" % SOCKS,
                            "http://%s/" % DEVICE], capture_output=True, text=True)
        check("curl --socks5", r.returncode == 0 and "vtesse bench device" in r.stdout, r.stderr.strip())

        t0 = time.time()
        r = subprocess.run(["curl", "-sS", "--max-time", "60", "http://127.0.0.1:%d/big.bin" % FWD_HTTP],
                           capture_output=True)
        dt = time.time() - t0
        got = hashlib.sha256(r.stdout).hexdigest()
        check("port forward: 20 MiB download, sha256 matches", got == want_sha and len(r.stdout) == 20 << 20,
              "%.1f MiB/s" % (20 / dt) if dt else "")

        s = socket.create_connection(("127.0.0.1", FWD_RTSP), timeout=10)
        s.sendall(b"OPTIONS rtsp://%s/ RTSP/1.0\r\nCSeq: 1\r\n\r\n" % DEVICE.encode())
        line = s.recv(200).split(b"\r\n")[0]
        s.close()
        check("port forward: RTSP OPTIONS", line == b"RTSP/1.0 200 OK", line.decode())

        payload = os.urandom(8 << 20)
        s = socket.create_connection(("127.0.0.1", FWD_ECHO), timeout=30)
        echoed = []

        def reader():
            n = 0
            while n < len(payload):
                c = s.recv(65536)
                if not c:
                    break
                echoed.append(c)
                n += len(c)
        th = threading.Thread(target=reader)
        th.start()
        t0 = time.time()
        s.sendall(payload)
        th.join(60)
        dt = time.time() - t0
        s.close()
        check("port forward: 8 MiB TCP echo (both directions), data intact", b"".join(echoed) == payload,
              "%.1f MiB/s each way" % (8 / dt) if dt else "")

        s = socket.create_connection(("127.0.0.1", FWD_CLOSED), timeout=10)
        try:
            closed = s.recv(10) == b""
        except ConnectionResetError:
            closed = True
        s.close()
        check("port forward to a closed port: local connection closed", closed)

        results = []

        def worker():
            try:
                s2, rep2 = socks_connect(1, DEVICE, 80, timeout=60)
                st, body2 = http_get(s2, "/big.bin")
                results.append(hashlib.sha256(body2).hexdigest() == want_sha)
            except Exception as e:  # noqa: BLE001
                results.append(False)
                print("   worker:", e)
        ths = [threading.Thread(target=worker) for _ in range(8)]
        t0 = time.time()
        for t in ths:
            t.start()
        for t in ths:
            t.join(120)
        dt = time.time() - t0
        check("8 parallel 20 MiB downloads via socks5, all intact", results == [True] * 8,
              "%.1f MiB/s total" % (160 / dt) if dt else "")
    finally:
        cli.stop()
    stats = [l for l in cli.lines if "proxy: accepted" in l]
    print(stats[-1] if stats else "\n".join(cli.lines[-15:]))

    # SOCKS5 username/password (RFC 1929)
    cli = Cli(["--socks", str(SOCKS)], "socks_user=bench\nsocks_password=s3cret\n")
    try:
        cli.wait_for("] address ")
        _, rep = socks_connect(1, DEVICE, 80)
        check("socks5 auth required: no-auth client refused (0xFF)", rep == 0xFF, "rep=%d" % rep)
        _, rep = socks_connect(1, DEVICE, 80, auth=("bench", "wrong"))
        check("socks5 wrong password refused", rep == -1, "rep=%d" % rep)
        s, rep = socks_connect(1, DEVICE, 80, auth=("bench", "s3cret"))
        status = http_get(s, "/")[0] if s else ""
        check("socks5 correct user/password -> HTTP 200", status.endswith("200 OK"), status)
    finally:
        cli.stop()

    print("\n%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
