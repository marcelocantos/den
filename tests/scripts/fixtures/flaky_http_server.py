#!/usr/bin/env python3
"""Local HTTP fixture for tests/scripts/test_vendor_fetch_retry.sh.

Serves one payload file with scripted failures, keyed by URL path, and logs
every request path (one per line) so the test can count attempts:

  /ok/<f>                  200 with the payload
  /e504/<n>/<f>            504 for the first n requests, then 200
  /e429/<n>/<f>            429 for the first n requests, then 200
  /redir504/<n>/<f>        302 -> /e504/<n>/<f> (final status is the 504)
  /trunc/<n>/<f>           first n: Content-Length promises more, then close (curl 18)
  /reset/<n>/<f>           first n: partial body, then TCP reset (curl 56)
  /e404/<f>                404 always
  /bad/<f>                 200 with the wrong bytes (hash mismatch)

Usage: flaky_http_server.py <payload> <port-file> <request-log>
"""
import http.server
import socket
import struct
import sys
import threading

payload_path, port_file, log_path = sys.argv[1:4]
with open(payload_path, "rb") as f:
    PAYLOAD = f.read()
counts = {}
lock = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _send(self, code, body=b"", headers=None):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_GET(self):
        path = self.path
        with lock:
            counts[path] = counts.get(path, 0) + 1
            n_seen = counts[path]
            with open(log_path, "a") as log:
                log.write(path + "\n")
        parts = path.strip("/").split("/")
        kind = parts[0]
        if kind == "ok":
            return self._send(200, PAYLOAD)
        if kind == "e404":
            return self._send(404, b"not found\n")
        if kind == "bad":
            return self._send(200, PAYLOAD[::-1] + b"x")
        fails = int(parts[1])
        failing = n_seen <= fails
        if kind == "redir504":
            target = "/e504/" + "/".join(parts[1:])
            return self._send(302, b"", {"Location": target})
        if kind in ("e504", "e429"):
            if failing:
                return self._send(int(kind[1:]), b"busy\n")
            return self._send(200, PAYLOAD)
        if kind in ("trunc", "reset"):
            if not failing:
                return self._send(200, PAYLOAD)
            self.send_response(200)
            self.send_header("Content-Length", str(len(PAYLOAD) + 4096))
            self.end_headers()
            self.wfile.write(PAYLOAD[: max(1, len(PAYLOAD) // 2)])
            self.wfile.flush()
            if kind == "reset":
                # SO_LINGER 0 + close() sends RST instead of FIN.
                self.connection.setsockopt(
                    socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                self.connection.close()
            self.close_connection = True
            return
        self._send(400, b"unknown fixture path\n")


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
with open(port_file + ".tmp", "w") as f:
    f.write(str(server.server_address[1]))
import os
os.replace(port_file + ".tmp", port_file)
server.serve_forever()
