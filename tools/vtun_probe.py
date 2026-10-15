#!/usr/bin/env python3
"""VTun 3.0.2 probe: handshake (XOR auth), flags, keepalive, optional DHCPDISCOVER.

Credentials are read from ~/.config/vtesse/probe.conf (key=value):
  host, port, session, password
Usage: vtun_probe.py [--dhcp] [--seconds N] [--config PATH]

WARNING: with `multi killold` on the server, connecting with a production
session name kicks the real device off. Use a dedicated test session.
"""
import argparse
import os
import random
import select
import socket
import struct
import time

MESG_SIZE = 50
CONN_CLOSE, ECHO_REQ, ECHO_REP = 0x1000, 0x2000, 0x4000
CHARS = "abcdefghijklmnop"


def load_cfg(path):
    cfg = {}
    with open(os.path.expanduser(path)) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    return cfg


def msg(text):
    b = text.encode()
    if len(b) > MESG_SIZE - 1:
        raise ValueError("message too long: %r" % text)
    return b + b"\0" * (MESG_SIZE - len(b))


def read_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError("EOF after %d of %d bytes" % (len(b), n))
        b += c
    return b


def read_mesg(s):
    return read_exact(s, MESG_SIZE).rstrip(b"\0").decode("latin1")


def cl2cs(chal):
    return "<" + "".join(CHARS[x >> 4] + CHARS[x & 15] for x in chal) + ">"


def cs2cl(text):
    body = text[text.index("<") + 1:text.index(">")]
    if len(body) != 32:
        raise ValueError("bad challenge: %r" % text)
    return bytes(((ord(body[i]) - 97) << 4) | (ord(body[i + 1]) - 97) for i in range(0, 32, 2))


def xor_chal(chal, password):
    pw = password.encode()
    return bytes(chal[i] ^ pw[i % len(pw)] for i in range(16))


def ip_csum(b):
    if len(b) % 2:
        b += b"\0"
    t = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    t = (t >> 16) + (t & 0xFFFF)
    t += t >> 16
    return (~t) & 0xFFFF


def dhcp_discover(mac):
    xid = random.getrandbits(32)
    opts = bytes([53, 1, 1, 61, 7, 1]) + mac + bytes([55, 7, 1, 3, 6, 15, 28, 121, 249, 255])
    bootp = struct.pack("!BBBBIHH4s4s4s4s16s64s128s", 1, 1, 6, 0, xid, 0, 0x8000,
                        b"\0" * 4, b"\0" * 4, b"\0" * 4, b"\0" * 4, mac + b"\0" * 10, b"", b"")
    bootp += b"\x63\x82\x53\x63" + opts
    bootp += b"\0" * max(0, 300 - len(bootp))
    udp = struct.pack("!HHHH", 68, 67, 8 + len(bootp), 0) + bootp
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0, b"\0" * 4, b"\xff" * 4)
    ip = ip[:10] + struct.pack("!H", ip_csum(ip)) + ip[12:]
    return b"\xff" * 6 + mac + b"\x08\x00" + ip + udp


def decode_121(v):
    routes, i = [], 0
    while i < len(v):
        width = v[i]
        n = (width + 7) // 8
        net = list(v[i + 1:i + 1 + n]) + [0] * (4 - n)
        gw = v[i + 1 + n:i + 5 + n]
        routes.append("%s/%d via %s" % (".".join(map(str, net)), width, socket.inet_ntoa(gw)))
        i += 5 + n
    return routes


def print_dhcp(fr):
    if fr[12:14] != b"\x08\x00" or fr[23] != 17:
        return
    ihl = (fr[14] & 15) * 4
    u = fr[14 + ihl:]
    if struct.unpack("!H", u[2:4])[0] != 68:
        return
    b = u[8:]
    o, i, opts = b[240:], 0, {}
    while i < len(o) and o[i] != 255:
        if o[i] == 0:
            i += 1
            continue
        opts[o[i]] = o[i + 2:i + 2 + o[i + 1]]
        i += 2 + o[i + 1]
    print("DHCP type %s from %s, yiaddr %s" % (opts.get(53, b"?").hex(), socket.inet_ntoa(fr[26:30]),
                                               socket.inet_ntoa(b[16:20])))
    for k, v in sorted(opts.items()):
        if k in (1, 3, 6, 28, 54):
            print("  opt %d: %s" % (k, ", ".join(socket.inet_ntoa(v[j:j + 4]) for j in range(0, len(v), 4))))
        elif k in (121, 249):
            print("  opt %d: %s" % (k, "; ".join(decode_121(v))))
        else:
            print("  opt %d: %s" % (k, v.hex()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="~/.config/vtesse/probe.conf")
    ap.add_argument("--dhcp", action="store_true", help="send one DHCPDISCOVER (type ether)")
    ap.add_argument("--seconds", type=float, default=12)
    ap.add_argument("--delay", type=float, default=1.0,
                    help="wait before the first data frame: the server brings the tap up "
                         "asynchronously and drops the session if a frame arrives earlier")
    a = ap.parse_args()
    cfg = load_cfg(a.config)

    s = socket.create_connection((cfg["host"], int(cfg["port"])), timeout=15)
    print("peer", s.getpeername())
    print("S:", repr(read_mesg(s)))
    s.sendall(msg("HOST: %s\n" % cfg["session"]))
    chal = read_mesg(s)
    print("S:", repr(chal))
    t0 = time.time()
    s.sendall(msg("CHAL: %s\n" % cl2cs(xor_chal(cs2cl(chal), cfg["password"]))))
    flags = read_mesg(s)
    print("S:", repr(flags), "after %.2fs" % (time.time() - t0))
    if not flags.startswith("OK"):
        raise SystemExit("authentication failed")

    if a.dhcp:
        time.sleep(a.delay)
        frame = dhcp_discover(bytes.fromhex(cfg["session"]))
        s.sendall(struct.pack(">H", len(frame)) + frame)
        print("sent DHCPDISCOVER (%d bytes)" % len(frame))

    buf, end = b"", time.time() + a.seconds
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.5)
        if not r:
            continue
        c = s.recv(65536)
        if not c:
            print("EOF from server")
            break
        buf += c
        while len(buf) >= 2:
            hdr = struct.unpack(">H", buf[:2])[0]
            fl, ln = hdr & 0xF000, hdr & 0x0FFF
            if fl:
                buf = buf[2:]
                print("%.2fs flag 0x%04x" % (time.time() - t0, fl))
                if fl == ECHO_REQ:
                    s.sendall(struct.pack(">H", ECHO_REP))
                elif fl == CONN_CLOSE:
                    print("server closed the session")
                    return
                continue
            if len(buf) < 2 + ln:
                break
            fr, buf = buf[2:2 + ln], buf[2 + ln:]
            print("%.2fs frame len=%d src=%s type=%s" % (time.time() - t0, ln, fr[6:12].hex(), fr[12:14].hex()))
            print_dhcp(fr)

    s.sendall(struct.pack(">H", CONN_CLOSE))
    s.close()
    print("sent CONN_CLOSE")


if __name__ == "__main__":
    main()
