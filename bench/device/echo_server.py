#!/usr/bin/env python3
"""TCP echo server for proxy tests (both directions, flow control)."""
import socketserver
import sys


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(65536)
            if not data:
                return
            self.request.sendall(data)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    Server((sys.argv[1], int(sys.argv[2])), Handler).serve_forever()
