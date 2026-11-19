"""Laptop preview of the web faces from this checkout, with live data from a board.

Serves the dashboard (/), admin (/admin) and setup (/setup) pages and their hashed /a/ assets exactly as
tools/gen_web_assets.py builds them for the firmware (rebuilt on every page load, so an edit in firmware/fauxcloud/web
shows on reload), and forwards GET /api/* to the board read-only: any other method gets 405 here and never reaches the
board. /api/setup/state is answered locally (the board's LAN face answers it with the catch-all), as a joined setup, so
/setup opens on its success card; /__sta_ip?ip=A.B.C.D[&mask=M.M.M.M] changes the address (and the subnet mask) it reports
(to check the 192.168.77.x subnet warning, a /16 home network included).

Safety: it listens on 127.0.0.1 only and answers only requests whose Host is localhost:<port> or
127.0.0.1:<port> (a page elsewhere cannot reach it through DNS rebinding); it forwards If-None-Match and X-Fauxcloud
only (never the browser's cookies); the board is a dotted IPv4 address, given with --board or by one POST to
/__board?token=<printed at start>&host=A.B.C.D from this machine, so no web page can retarget it. The address is never
written to a file. Without one, /api answers 502 and the dashboard says "No answer from fauxcloud".

Usage: python -I test/tools/preview_proxy.py [--port 8377] [--board A.B.C.D | --mock PORT] [--node PATH]   (else FAUXCLOUD_NODE)
--mock PORT forwards to a synthetic mock board on this machine (127.0.0.1:PORT) for mockups and render checks.
"""
import argparse
import http.server
import ipaddress
import json
import os
import secrets
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_web_assets as G  # noqa: E402

STATE = {"board": "", "sta_ip": "192.168.77.23", "sta_mask": "255.255.255.0", "built": None, "port": 0, "token": secrets.token_hex(8)}
PAGES = {"/": "index.html", "/admin": "admin.html", "/setup": "setup.html"}


def ipv4(text):
    """The text as a dotted IPv4 address, or '' (no names, no ports, no IPv6)."""
    try:
        return str(ipaddress.IPv4Address(text.strip()))
    except ValueError:
        return ""


def build():
    table = G.assets()
    STATE["built"] = (table, {route: G.page(G.WEB / name, table) for route, name in PAGES.items()})


class Handler(http.server.BaseHTTPRequestHandler):
    def send(self, code, body, ctype="application/json", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def local(self):
        """Only this machine's own browser, by the name it used (DNS rebinding sends another Host)."""
        if self.headers.get("Host", "") in ("localhost:%d" % STATE["port"], "127.0.0.1:%d" % STATE["port"]):
            return True
        self.send(403, b'{"error":"preview answers localhost only"}')
        return False

    def do_GET(self):
        if not self.local():
            return
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/__sta_ip":
            STATE["sta_ip"] = ipv4((q.get("ip") or [""])[0]) or STATE["sta_ip"]
            STATE["sta_mask"] = ipv4((q.get("mask") or [""])[0]) or STATE["sta_mask"]
            return self.send(200, ("setup reports sta_ip %s, sta_mask %s\n" % (STATE["sta_ip"], STATE["sta_mask"])).encode(),
                             "text/plain")
        if u.path in PAGES:
            build()
            return self.send(200, STATE["built"][1][u.path], "text/html; charset=utf-8", {"X-Fauxcloud-Face": "lan"})
        if u.path.startswith("/a/"):
            if not STATE["built"]:
                build()
            for name, g, path, ctype in STATE["built"][0]:
                if path == u.path:
                    return self.send(200, g, ctype, {"Content-Encoding": "gzip"})
            return self.send(404, b'{"error":"no such asset"}')
        if u.path == "/api/setup/state":
            return self.send(200, json.dumps({"state": "joined", "hostname": "fauxcloud", "ap_ssid": "fauxcloud-0000",
                                              "sta_ssid": "Home Wi-Fi", "keep_ap": False, "keep_admin": False, "tz": "",
                                              "tz_name": "", "sta_ip": STATE["sta_ip"],
                                              "sta_mask": STATE["sta_mask"]}).encode())
        if u.path.startswith("/api/"):
            return self.forward()
        return self.send(200, b"success", "text/plain")

    def do_POST(self):
        if not self.local():
            return
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/__board":
            if (q.get("token") or [""])[0] != STATE["token"]:
                return self.send(403, b'{"error":"token"}')
            host = ipv4((q.get("host") or [""])[0])
            if not host:
                return self.send(400, b'{"error":"a dotted IPv4 address"}')
            STATE["board"] = host
            return self.send(200, b"board set\n", "text/plain")
        self.refuse()

    def forward(self):
        if not STATE["board"]:
            return self.send(502, b'{"error":"no board address"}')
        req = urllib.request.Request("http://%s%s" % (STATE["board"], self.path), method="GET")
        for h in ("If-None-Match", "X-Fauxcloud"):
            if self.headers.get(h):
                req.add_header(h, self.headers[h])
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                code, body, hdr = r.status, r.read(), r.headers
        except urllib.error.HTTPError as e:
            code, body, hdr = e.code, e.read(), e.headers
        except OSError as e:
            return self.send(502, json.dumps({"error": str(e)}).encode())
        extra = {k: hdr[k] for k in ("ETag", "X-Fauxcloud-Face") if hdr.get(k)}
        if code == 304:
            self.send_response(304)
            for k, v in extra.items():
                self.send_header(k, v)
            self.end_headers()
            return
        self.send(code, body, hdr.get("Content-Type", "application/json"), extra)

    def refuse(self):
        self.send(405, b'{"error":"read-only preview"}')

    do_PUT = do_DELETE = do_PATCH = refuse

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.command, urllib.parse.urlsplit(self.path).path))  # no query (token)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8377)
    ap.add_argument("--board", default="", help="the board's dotted IPv4 address")
    ap.add_argument("--node", help="node.exe for the bundler (else FAUXCLOUD_NODE / PATH)")
    ap.add_argument("--mock", type=int, help="a synthetic mock board on 127.0.0.1:PORT instead of a board (mockups, never the board)")
    a = ap.parse_args()
    if a.node:
        os.environ["FAUXCLOUD_NODE"] = a.node
    if a.board and not ipv4(a.board):
        raise SystemExit("--board: a dotted IPv4 address")
    STATE["board"], STATE["port"] = ipv4(a.board), a.port
    if a.mock:
        STATE["board"] = "127.0.0.1:%d" % a.mock
    build()
    print("preview on http://localhost:%d  (/, /admin, /setup); board: POST /__board?token=%s&host=A.B.C.D"
          % (a.port, STATE["token"]), flush=True)
    http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
