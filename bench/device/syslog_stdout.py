#!/usr/bin/env python3
"""Minimal syslog sink: receive datagrams on /dev/log and print them to stdout."""
import os
import socket
import sys

PATH = "/dev/log"

if os.path.exists(PATH):
    os.unlink(PATH)
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(PATH)
os.chmod(PATH, 0o666)
while True:
    data = s.recv(4096).decode("utf-8", "replace").rstrip("\n\0")
    # strip the "<PRI>" prefix
    if data.startswith("<") and ">" in data[:5]:
        data = data[data.index(">") + 1:]
    sys.stdout.write("[syslog] %s\n" % data)
    sys.stdout.flush()
