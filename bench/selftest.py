#!/usr/bin/env python3
"""Bench self-test: protocol behaviour of the VTun 3.0.2 server that VTesse relies on.

Usage: selftest.py [host] [port]      (default 127.0.0.1 15000, i.e. `bench.sh up`)
"""
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from vtun_probe import (MESG_SIZE, CONN_CLOSE, ECHO_REQ, ECHO_REP, msg, read_exact, read_mesg,  # noqa: E402
                        cl2cs, cs2cl, xor_chal, dhcp_discover)

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 15000

# Bench-only test sessions (bench/vtund.conf).
S = {
    "main": ("02BE0C000001", "c79e01d2e49d80eafd192c043c5142bd5a65b5a9"),
    "shaper": ("02BE0C000003", "a61ced02fb7e8191c77e74fd3e6ac5e80efce635"),
    "compress": ("02BE0C000004", "3adac4cea43b5e0b6402463cfb62569249d6daf3"),
    "deny": ("02BE0C000005", "5b3b17af423b1ea1c49dd3a1bc56a9baa6d33f1d"),
    "tun": ("02BE0C000006", "3158ed23d81fba0909c1a18d263ca8f4cef1b6a0"),
}

failures = 0


def check(name, cond, detail=""):
    global failures
    print("%s  %s%s" % ("PASS" if cond else "FAIL", name, ("  (%s)" % detail) if detail else ""))
    if not cond:
        failures += 1


def login(session, password, greeting=None):
    """Returns (socket, final reply, seconds from CHAL to reply)."""
    s = socket.create_connection((HOST, PORT), timeout=15)
    raw = read_exact(s, MESG_SIZE)
    if greeting is not None:
        greeting.append(raw)
    s.sendall(msg("HOST: %s\n" % session))
    chal = read_mesg(s)
    if not chal.startswith("OK CHAL"):
        return s, chal, 0
    t0 = time.time()
    s.sendall(msg("CHAL: %s\n" % cl2cs(xor_chal(cs2cl(chal), password))))
    try:
        reply = read_mesg(s)
    except EOFError:
        reply = "<EOF>"
    return s, reply, time.time() - t0


def close(s):
    try:
        s.sendall(struct.pack(">H", CONN_CLOSE))
    except OSError:
        pass
    s.close()


def wait_flag(s, want, seconds):
    """Read frames for up to `seconds`; return True when a flag frame `want` is seen
    (or EOF when want is None)."""
    s.settimeout(0.5)
    end, buf = time.time() + seconds, b""
    while time.time() < end:
        try:
            c = s.recv(4096)
        except socket.timeout:
            continue
        if not c:
            return want is None
        buf += c
        while len(buf) >= 2:
            hdr = struct.unpack(">H", buf[:2])[0]
            fl, ln = hdr & 0xF000, hdr & 0x0FFF
            if fl:
                buf = buf[2:]
                if fl == ECHO_REQ:
                    s.sendall(struct.pack(">H", ECHO_REP))
                if fl == want:
                    return True
                continue
            if len(buf) < 2 + ln:
                break
            buf = buf[2 + ln:]
    return False


def dhcp_offer(s, mac, seconds):
    """Send DHCPDISCOVER, return (seconds until OFFER, raw frame) or (None, None)."""
    t0 = time.time()
    frame = dhcp_discover(mac)
    s.sendall(struct.pack(">H", len(frame)) + frame)
    s.settimeout(0.5)
    buf, end = b"", t0 + seconds
    while time.time() < end:
        try:
            c = s.recv(65536)
        except socket.timeout:
            continue
        if not c:
            return None, None
        buf += c
        while len(buf) >= 2:
            hdr = struct.unpack(">H", buf[:2])[0]
            fl, ln = hdr & 0xF000, hdr & 0x0FFF
            if fl:
                buf = buf[2:]
                if fl == ECHO_REQ:
                    s.sendall(struct.pack(">H", ECHO_REP))
                continue
            if len(buf) < 2 + ln:
                break
            fr, buf = buf[2:2 + ln], buf[2 + ln:]
            if fr[12:14] == b"\x08\x00" and fr[23] == 17 and fr[36:38] == b"\x00\x44":
                return time.time() - t0, fr
    return None, None


def main():
    # Greeting and the main session
    g = []
    s, reply, _ = login(*S["main"], greeting=g)
    check("greeting is exactly 50 bytes, NUL padded",
          len(g[0]) == 50 and g[0].startswith(b"VTUN server ver ") and g[0].rstrip(b"\0").endswith(b"\n"))
    check("main session flags <TeK>", reply == "OK FLAGS: <TeK>\n", repr(reply))
    check("ECHO_REQ is the first frame after OK FLAGS", wait_flag(s, ECHO_REQ, 2))
    time.sleep(0.5)  # the server brings the tap up asynchronously
    dt, fr = dhcp_offer(s, bytes.fromhex(S["main"][0]), 10)
    check("DHCP OFFER through the tunnel", fr is not None, "after %.1fs" % dt if dt else "")
    check("DHCP OFFER delayed by the ping check (>= 2 s)", dt is not None and dt >= 2.0)
    check("DHCP OFFER carries option 121", fr is not None and b"\x79\x10\x18\x0a\x4e\x00" in fr)

    # killold: a second login takes the session over, the first one is closed
    s2, reply2, dt2 = login(*S["main"])
    check("killold: second login succeeds", reply2.startswith("OK FLAGS"), "%.2fs" % dt2)
    check("killold: first connection gets CONN_CLOSE or EOF",
          wait_flag(s, CONN_CLOSE, 6) or wait_flag(s, None, 1))
    close(s2)
    s.close()

    # Flags of other sessions
    for key, want in (("shaper", "<TeS512K>"), ("compress", "<TeC9K>"), ("tun", "<TuK>")):
        s, reply, _ = login(*S[key])
        check("%s session flags %s" % (key, want), reply == "OK FLAGS: %s\n" % want, repr(reply))
        close(s)

    # Errors
    s, reply, _ = login(S["main"][0], "wrong-password")
    check("wrong password -> ERR", reply == "ERR\n", repr(reply))
    s.close()
    s, reply, _ = login("02BE0C0000FF", "whatever")
    check("unknown session -> OK CHAL, then ERR", reply == "ERR\n", repr(reply))
    s.close()

    # multi no: the second parallel login is refused
    s1, r1, _ = login(*S["deny"])
    s2, r2, _ = login(*S["deny"])
    check("multi no: first login OK", r1.startswith("OK FLAGS"), repr(r1))
    check("multi no: second login -> ERR", r2 == "ERR\n", repr(r2))
    close(s1)
    s2.close()

    print("\n%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
