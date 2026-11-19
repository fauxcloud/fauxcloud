"""fwd_fake_endpoint.py -- a laptop-side fake weather service for forwarding.

Serves canned HTTP answers on the LAN so the bridge's client can be driven through every response case without a real
service: point a "WU-compatible server" or a custom template at http://<laptop>:<port>/<case>/... (plain HTTP). Each
request is logged as one line with PASSWORD / key values replaced by *** (nothing else from the query is secret: the
fixture ids are invented, and the board's test sends a real key only to the real services).

The CASES table is also the source of the host parser vectors: test/tools/gen_fwd_vectors.py writes each case's bytes
and expected result into test/host/fwd_vectors.inc, and test/host/test_fwd.inc feeds them to FwdResp (whole and one
byte at a time). test/tools/test_fwd_fake_endpoint.py drives this server over a real socket.

    python -I tools/fwd_fake_endpoint.py --port 8089            # serve until Ctrl+C
    python -I tools/fwd_fake_endpoint.py --list                 # the cases
stdlib only.
"""
import argparse
import re
import socket
import socketserver
import sys
import threading
import time

def sized(head, body):
    """A response with its Content-Length."""
    return head + b"\r\nContent-Length: %d\r\n\r\n" % len(body) + body


# name -> (parse as type, success text, response bytes, expected result, expected status, retry_after)
# Behaviour cases (no bytes) are below in BEHAVIOUR.
CASES = [
    ("wu-ok", "wu", "", b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 8\r\n\r\nsuccess\n", "ok", 200, 0),
    ("wu-badkey", "wu", "", b"HTTP/1.1 200 OK\r\nContent-Length: 51\r\n\r\nINVALIDPASSWORDID|Password and/or id are incorrect\n",
     "refused", 200, 0),
    ("wu-401", "wu", "", b"HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n\r\nunauthorized", "refused", 401, 0),
    ("wu-400", "wu", "", b"HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\n\r\nbad request", "reject", 400, 0),
    ("pws-ok", "pwsweather", "", b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 29\r\n\r\n"
     b"{\"error\":null,\"success\":true}", "ok", 200, 0),
    ("pws-fail", "pwsweather", "", sized(b"HTTP/1.1 200 OK",
     b"{\"error\":{\"code\":\"invalid_location\",\"description\":\"x\"},\"success\":false}"), "reject", 200, 0),
    ("windy-ok", "windy", "", b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", "ok", 200, 0),
    ("windy-409", "windy", "", b"HTTP/1.1 409 Conflict\r\nContent-Length: 0\r\n\r\n", "dup", 409, 0),
    ("windy-429", "windy", "", b"HTTP/1.1 429 Too Many Requests\r\nRetry-After: 240\r\nContent-Length: 19\r\n\r\n"
     b"{\"retry_after\":240}", "limited", 429, 240),
    ("wowbe-ok", "wowbe", "", sized(b"HTTP/1.1 200 OK\r\nContent-Type: application/geo+json",
     b"{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[0,0]}}"), "ok", 200, 0),
    ("chunked", "template", "accepted", b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nacce\r\n4\r\npted\r\n"
     b"0\r\n\r\n", "ok", 200, 0),
    ("compat-success", "wu_compat", "success", b"HTTP/1.0 200 OK\r\n\r\nsuccess\n", "ok", 200, 0),
    ("compat-other", "wu_compat", "success", b"HTTP/1.0 200 OK\r\n\r\nstored, thanks", "reject", 200, 0),
    ("server-500", "wu", "", b"HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n", "fail", 500, 0),
    ("redirect", "wu", "", b"HTTP/1.1 301 Moved Permanently\r\nLocation: http://example.org/\r\n\r\n", "reject", 301, 0),
    ("garbage", "wu", "", b"SSH-2.0-OpenSSH_9.6\r\n", "reject", -1, 0),
    ("big-headers", "wu", "", b"HTTP/1.1 200 OK\r\n" + b"X-Pad: " + b"0" * 1100 + b"\r\n\r\nsuccess", "reject", 200, 0),
    ("big-body", "wu", "", b"HTTP/1.1 200 OK\r\n\r\nsuccess" + b"x" * 600, "ok", 200, 0),
]
# Cases with timing / a broken connection (the board's failure injection); not byte vectors
BEHAVIOUR = {
    "stall15": "read the request, then say nothing for 15 s (the bridge cuts at 10 s)",
    "close": "close right after the request (no status line: fail)",
    "split": "the wu-ok answer one byte every 50 ms (a status line across reads)",
}


def redact(line):
    return re.sub(r"(?i)((?:password|key|pass)=)[^&\s]*", r"\1***", line)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request
        s.settimeout(5)
        data = b""
        try:
            while b"\r\n\r\n" not in data and len(data) < 4096:
                chunk = s.recv(1024)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
        line = data.split(b"\r\n", 1)[0].decode("latin-1")
        m = re.match(r"GET /([A-Za-z0-9-]+)", line)
        case = m.group(1) if m else ""
        if not self.server.quiet:
            print(time.strftime("%H:%M:%S"), self.client_address[0], case or "?", redact(line)[:300], flush=True)
        by = {c[0]: c for c in CASES}
        if case in by:
            s.sendall(by[case][3])
        elif case == "stall15":
            time.sleep(15)
        elif case == "close":
            pass
        elif case == "split":
            for b in by["wu-ok"][3]:
                s.sendall(bytes([b]))
                time.sleep(0.05)
        else:
            s.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    quiet = False


def start(port=0, quiet=True):
    """For tests: a server on 127.0.0.1 in a thread; returns (server, port)."""
    srv = Server(("127.0.0.1", port), Handler)
    srv.quiet = quiet
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8089)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        for c in CASES:
            print(f"/{c[0]:16} as {c[1]:10} -> {c[4]:8} status {c[5]}")
        for k, v in BEHAVIOUR.items():
            print(f"/{k:16} {v}")
        return
    srv = Server((a.bind, a.port), Handler)
    print(f"fwd_fake_endpoint on {a.bind}:{a.port} -- /<case>/... ; --list for the cases", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
