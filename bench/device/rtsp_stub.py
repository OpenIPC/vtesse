#!/usr/bin/env python3
"""Tiny RTSP responder for connectivity tests (OPTIONS and DESCRIBE only).

It does not stream media; it lets port-forward / SOCKS tests verify that an
RTSP request reaches the device and a well-formed reply comes back.
"""
import socketserver
import sys

SDP = (
    "v=0\r\n"
    "o=- 0 0 IN IP4 0.0.0.0\r\n"
    "s=vtesse bench\r\n"
    "t=0 0\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=control:trackID=0\r\n"
)


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        while True:
            lines = []
            while True:
                line = self.rfile.readline()
                if not line:
                    return
                line = line.decode("latin1").rstrip("\r\n")
                if not line:
                    break
                lines.append(line)
            method = lines[0].split(" ", 1)[0] if lines else ""
            cseq = next((l.split(":", 1)[1].strip() for l in lines if l.lower().startswith("cseq:")), "0")
            if method == "OPTIONS":
                reply = "RTSP/1.0 200 OK\r\nCSeq: %s\r\nPublic: OPTIONS, DESCRIBE\r\n\r\n" % cseq
            elif method == "DESCRIBE":
                reply = ("RTSP/1.0 200 OK\r\nCSeq: %s\r\nContent-Type: application/sdp\r\n"
                         "Content-Length: %d\r\n\r\n%s" % (cseq, len(SDP), SDP))
            else:
                reply = "RTSP/1.0 501 Not Implemented\r\nCSeq: %s\r\n\r\n" % cseq
            self.wfile.write(reply.encode())


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    Server((sys.argv[1], int(sys.argv[2])), Handler).serve_forever()
