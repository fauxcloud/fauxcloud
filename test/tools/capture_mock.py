"""Laptop mock of the admin face for "Listen to a new console": the admin page from this checkout (built as
tools/gen_web_assets.py builds it for the firmware, rebuilt on every page load) and a synthetic board behind it, signed
in, with one station and a capture that plays a scripted session once started: a phone joins and probes, then the
console joins, asks DNS and NTP, tries TLS on 443, posts JSON on port 80, repeats, connects to MQTT and to port 5000.
Nothing leaves this machine; no board is needed.

Usage: python -I test/tools/capture_mock.py [--port 8399] [--node PATH]   (else FAUXCLOUD_NODE)
Then open http://localhost:8399/admin, "fauxcloud Wi-Fi for your station", "Listen to a new console".
Scenarios (from this machine): /__scn?s=default | ota | restart | heap | notime | nothing | lost | broker | busy | shot
  shot: the whole session has played by the first poll after Start (for the docs screenshots, with
  /admin?shot=wifi | start | listening, which opens the sheets by itself).
  ota / restart: a start is refused (409) and the GET says why; heap: the start ends "off", ended "heap";
  notime: the bridge has no clock and NTP goes unanswered; nothing: nobody ever joins; lost: 5 records left the board
  unread; broker: MQTT configured, forwarding and airport weather on (every consequence line); busy: a second
  connection on 443 while one is read.
Safety: listens on 127.0.0.1 only and answers only Host localhost:<port> / 127.0.0.1:<port>.
"""
import argparse
import http.server
import json
import os
import sys
import time
import urllib.parse

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_web_assets as G  # noqa: E402

PAGES = {"/admin": "admin.html", "/": "admin.html"}

# The docs screenshots: /admin?shot=home | wifi | start | listening appends this script, which opens the sheets as a
# reader would (home: Settings at the fauxcloud Wi-Fi tile; then the fauxcloud Wi-Fi sheet, then "Listen to a new
# console", then Start); with the scenario "shot" the whole session has played by the first poll. A headless browser at
# 375 px, 2x, takes the picture (site/README.md, "Screenshots of the admin face").
SHOT_JS = """<script>(function () {
  var want = %s, wait = function (ms) { return new Promise(function (r) { setTimeout(r, ms); }); };
  var find = function (re) { return [].slice.call(document.querySelectorAll('button')).find(function (b) { return re.test(b.textContent); }); };
  (async function () {
    for (var i = 0; i < 50 && !find(/fauxcloud Wi-Fi for your station/); i++) await wait(200);
    if (want === 'home') { find(/fauxcloud Wi-Fi for your station/).scrollIntoView({ block: 'center' }); return; }
    find(/fauxcloud Wi-Fi for your station/).click(); await wait(800);
    var o = document.querySelector('#capOpen');
    if (want === 'wifi') { o.scrollIntoView({ block: 'center' }); return; }
    o.click(); await wait(1500);
    if (want === 'start') return;
    document.querySelector('#capStart').click(); await wait(4000);
    var r = document.querySelector('#capOut'); if (r) r.scrollTop = 0;
  })();
})();</script>"""
S = {"port": 0, "built": None, "scn": "default", "cap": None, "last_end": "", "elapsed": 0, "port_hits": 0}
PHONE_IP, CONSOLE_IP = "192.168.77.24", "192.168.77.23"
# the console's maker prefix: the IANA documentation block (RFC 7042), so no picture shows a real maker's prefix
# the scenario "shot": a release version and a fixed start time on the pictures
SHOT_UTC = 1796117400  # 2026-12-01 09:30Z


def VERSION():
    return "1.1.0" if S["scn"] == "shot" else "1.1.0-dev.2"

# The scripted session: (second after "on", kind, client octet, text, group offset). A request's records share a group.
SCRIPT = [
    (2, "join", 24, "joined vendor:random"), (2, "lease", 24, "lease vendor:random"),
    (3, "http", 24, "GET /generate_204"), (3, "hdr", 24, "host=connectivitycheck.gstatic.com ua=Dalvik/2.1.0 conn=Keep-Alive", 1),
    (6, "join", 23, "joined vendor:00:00:5e"), (6, "lease", 23, "lease vendor:00:00:5e"),
    (9, "ntp", 23, "asked, answered"),
    (11, "tcp", 23, ":443"),
    (11, "tls", 23, ":443 verdict=ecdhe-ecdsa-gcm v=1.2 sni=api.example.net suites=c02b,c02c,c02f,c030,cca9,cca8,c009,c013,c00a,c014,009c,009d +6 groups=x25519,p256,p384 sigalg=yes mfl=- alpn=-"),
    (14, "http", 23, "POST /v2/station/upload?stationKey=***&fw=2.1.7"),
    (14, "hdr", 23, "host=api.example.net ua=ESP32HTTPClient ct=application/json cl=187 conn=close closed-before-reply", 1),
    (14, "body", 23, 'json 187B: {"device_id":***,"temp":21.4,"hum":56,"press":1013.2,"wind":{"spd":3.1,"gust":5.4,"dir":212},"rain":{"rate":0,"day":1.2},"uv":1,"lux":5300', 2),
    (20, "tcp", 23, ":1883"), (20, "mqtt", 23, ":1883 MQTT 3.1.1 flags=user,pass,clean keepalive=60 clientid=17B"),
    (26, "tcp", 23, ":5000"), (26, "raw", 23, ":5000 24B: 5a a5 01 10 00 18 01 02 03 04 05 06 07 08 09 0a"),
]
DNS = [(8, "api.example.net", 23), (8, "time.example.net", 23), (4, "connectivitycheck.gstatic.com", 24)]


def now():
    return time.monotonic()


class Cap:
    """A capture that plays SCRIPT (and repeats the console's upload every 20 s) with the board's GET semantics."""

    def __init__(self):
        self.t0 = now()
        self.recs, self.seq, self.played, self.served, self.lost = {}, 0, set(), 0, 0
        self.state = "starting"
        self.ended = ""

    def on_s(self):
        return max(0.0, now() - self.t0 - 2)  # two seconds of "starting"

    def age(self, seconds):  # the scenario "shot": as if it had been listening that long
        self.t0 -= seconds

    def tick(self):
        if self.state == "starting" and now() - self.t0 >= 2:
            if S["scn"] == "heap":
                self.state, self.ended = "off", "heap"
                return
            self.state = "on"
        if self.state != "on" or S["scn"] == "nothing":
            return
        t = self.on_s()
        group = None
        for i, row in enumerate(SCRIPT):
            sec, kind, cl, text = row[:4]
            if i in self.played or t < sec:
                continue
            if kind == "ntp" and S["scn"] == "notime":
                text = "asked, not answered: fauxcloud has no time"
            self.played.add(i)
            self.seq += 1
            gid = self.seq if len(row) < 5 else group
            group = gid
            self.recs[self.seq] = {"id": self.seq, "seq": self.seq, "group": gid, "kind": kind, "client": cl, "n": 1,
                                   "t_first_s": int(sec), "t_last_s": int(sec), "text": text}
        # the console uploads again every 20 s: its request's three records count again (a new seq each)
        reps = int(max(0, t - 14) // 20)
        for r in self.recs.values():
            if r["text"].startswith(("POST /v2", "host=api.example.net", "json 187B")) and r["n"] < reps + 1:
                self.seq += 1
                r["n"], r["seq"], r["t_last_s"] = reps + 1, self.seq, int(t)
        if S["scn"] == "busy" and t > 12 and not any(r["text"] == ":443 busy, closed" for r in self.recs.values()):
            self.seq += 1
            self.recs[self.seq] = {"id": self.seq, "seq": self.seq, "group": self.seq, "kind": "tcp", "client": 0, "n": 1,
                                   "t_first_s": int(t), "t_last_s": int(t), "text": ":443 busy, closed"}

    def view(self, since):
        self.tick()
        self.served = max(self.served, since)
        newer = sorted((r for r in self.recs.values() if r["seq"] > since), key=lambda r: r["seq"])
        on = self.state == "on"
        t = self.on_s()
        clients = []
        if on and S["scn"] != "nothing":
            if t >= 2:
                clients.append({"ip": PHONE_IP, "vendor": "random", "joined_s": 2, "left_s": 40 if t >= 40 else None, "n": 2, "active": False})
            if t >= 6:
                clients.append({"ip": CONSOLE_IP, "vendor": "00:00:5e", "joined_s": 6, "left_s": None,
                                "n": sum(r["n"] for r in self.recs.values() if r["client"] == 23), "active": True})
        names = [{"name": n, "n": 3 + int(t // 20), "types": "A", "client": c} for s, n, c in DNS if on and t >= s and S["scn"] != "nothing"]
        ntp_asked = 1 + int(max(0, t - 9) // 64) if on and t >= 9 and S["scn"] != "nothing" else 0
        return {"state": self.state, "ended": self.ended, "blocked": "", "elapsed_s": int(t) if on else S["elapsed"],
                "idle_left_s": 3600 if on else 0, "started_utc": None if S["scn"] == "notime" else SHOT_UTC if S["scn"] == "shot" else int(time.time() - t),
                "clock": S["scn"] != "notime", "ntp": {"asked": ntp_asked, "answered": 0 if S["scn"] == "notime" else ntp_asked},
                "uploads_skipped": 0, "vendor": "00:00:5e" if on and t >= 14 else None, "seq": self.seq,
                "lost": 5 if S["scn"] == "lost" and on and t > 15 else 0, "more": len(newer) > 8, "clients": clients,
                "clients_over": 0, "dns": {"other": 0, "names": names}, "events": newer[:8]}


def blocked():
    return {"ota": "ota", "restart": "restart"}.get(S["scn"], "")


def off_view():
    last = S.get("last_view")
    if last:  # the board keeps the counters after a stop; the lists and the records are gone
        return dict(last, state="off", ended=S["last_end"], blocked=blocked(), elapsed_s=S["elapsed"], idle_left_s=0, more=False,
                    clients=[], dns={"other": last["dns"]["other"], "names": []}, events=[])
    return {"state": "off", "ended": S["last_end"], "blocked": blocked(), "elapsed_s": S["elapsed"], "idle_left_s": 0,
            "started_utc": None, "clock": S["scn"] != "notime", "ntp": {"asked": 0, "answered": 0}, "uploads_skipped": 0,
            "vendor": None, "seq": 0, "lost": 0, "more": False, "clients": [], "clients_over": 0,
            "dns": {"other": 0, "names": []}, "events": []}


def quiet():
    c = S["cap"]
    return bool(c and c.state != "off")


def live():
    broker = S["scn"] in ("broker", "shot")  # the docs screenshots show every pause line
    st = {"id": "CAPT1", "name": "Garden", "online": not quiet(), "age_s": 40, "uploads": 812, "values": {"temp_c": 6.4, "humidity": 81,
          "wind_kmh": 7.2, "pressure_hpa": 1014.2}, "known": [], "rej": {}, "last_rejected": []}
    return {"uptime_s": 86400, "time_synced": S["scn"] != "notime", "page": "mock", "stations": [st],
            "diag": {"ap_clients": 2 if quiet() else 1, "sta_up": True, "sta_rssi": -58, "mqtt_up": broker and not quiet(),
                     "heap_free": 51200, "heap_min": 31000, "heap_low": False, "capture": quiet(), "fw_version": "fauxcloud " + VERSION(),
                     "build": VERSION() + " mock", "page": "mock", "ota_state": "stable", "ota_slot": 1900544, "sta_ip": "192.168.1.50",
                     "uploads": 812, "mqtt_published": 0, "mqtt_failed": 0, "last_reset": "power_on", "restarts": 1, "last_crash": None}}


def settings():
    return {"settings": {"sta_ssid": "Home Wi-Fi", "hostname": "fauxcloud", "mqtt_host": "broker.lan" if S["scn"] in ("broker", "shot") else "",
                         "mqtt_port": 1883}, "ap_ssid": "fauxcloud-1A2B", "wifi_change": ""}


def bridge():
    b = {"tz": "CET-1CEST,M3.5.0,M10.5.0/3", "tz_name": "Europe/Warsaw", "lat": None, "lon": None, "auto_publish_new_stations": True,
         "lan_uploads": False, "max_stations": 1, "max_stations_cap": 1, "mqtt_interval_last_seen_s": 300,
         "forwarding": S["scn"] in ("broker", "shot"), "log_verbose": False}
    return {"effective": b, "overrides": {}}


class Handler(http.server.BaseHTTPRequestHandler):
    def send(self, code, body, ctype="application/json", extra=None):
        if not isinstance(body, bytes):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def local(self):
        if self.headers.get("Host", "") in ("localhost:%d" % S["port"], "127.0.0.1:%d" % S["port"]):
            return True
        self.send(403, {"error": "mock answers localhost only"})
        return False

    def do_GET(self):
        if not self.local():
            return
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/__scn":
            S["scn"] = (q.get("s") or ["default"])[0]
            return self.send(200, ("scenario %s\n" % S["scn"]).encode(), "text/plain")
        if u.path in PAGES:
            table = G.assets()
            S["built"] = table
            html = G.page(G.WEB / PAGES[u.path], table)
            shot = (q.get("shot") or [""])[0]
            if shot in ("home", "wifi", "start", "listening"):
                html = html.replace(b"</body>", (SHOT_JS % json.dumps(shot)).encode() + b"</body>", 1)
            return self.send(200, html, "text/html; charset=utf-8", {"X-Fauxcloud-Face": "lan"})
        if u.path.startswith("/a/"):
            for name, g, path, ctype in S["built"] or G.assets():
                if path == u.path:
                    return self.send(200, g, ctype, {"Content-Encoding": "gzip"})
            return self.send(404, {"error": "no such asset"})
        p = u.path
        if p == "/api/admin/settings":
            return self.send(200, settings())
        if p == "/api/live":
            return self.send(200, live(), extra={"X-Fauxcloud-Face": "lan"})
        if p == "/api/admin/bridge":
            return self.send(200, bridge())
        if p == "/api/bridge/status":
            return self.send(200, {"ids": ["CAPT1"], "no_slot_id": "", "ota": S["scn"] == "ota"})
        if p == "/api/admin/station":
            return self.send(200, {"effective": {"profile": "yt60234", "publish": True, "stale_s": 240, "disabled": [], "promote": {}},
                                   "overrides": {"name": "Garden"}, "key": "bound", "src_mac": "", "fp": "match"})
        if p == "/api/admin/sessions":
            return self.send(200, [{"id": "s1", "current": True, "ip": "192.168.1.10", "ua": "mock", "last_s": 0}])
        if p == "/api/admin/station/blocked":
            return self.send(200, {"blocked": []})
        if p == "/api/admin/rings":
            return self.send(200, {"rings": [], "parked": []})
        if p == "/api/admin/forwarding":
            return self.send(200, {"targets": []})
        if p == "/api/admin/metar":
            return self.send(200, {"available": True, "on": S["scn"] in ("broker", "shot"), "airports": []})
        if p == "/api/admin/challenge":
            return self.send(200, {"nonce": "00" * 16, "salt": "00" * 16, "iters": 1})
        if p == "/api/admin/capture":
            since = int((q.get("since") or ["0"])[0] or 0)
            c = S["cap"]
            if not c:
                return self.send(200, off_view())
            v = c.view(since)
            if c.state == "off":
                S["cap"], S["last_end"] = None, c.ended
                v["blocked"] = blocked()
            S["last_view"] = v
            return self.send(200, v)
        if p.startswith("/api/"):
            return self.send(404, {"error": "not in the mock"})
        return self.send(200, b"success", "text/plain")

    def do_POST(self):
        if not self.local():
            return
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(n)
        p = urllib.parse.urlsplit(self.path).path
        if p == "/api/admin/capture/start":
            if quiet():
                return self.send(409, {"error": "on"})
            if blocked():
                return self.send(409, {"error": blocked()})
            S["cap"], S["last_end"], S["last_view"] = Cap(), "", None
            if S["scn"] == "shot":
                S["cap"].age(150)
            return self.send(200, {"ok": True})
        if p == "/api/admin/capture/stop":
            c = S["cap"]
            if c:
                S["last_view"] = c.view(c.seq)
                S["elapsed"], S["last_end"], S["cap"] = int(c.on_s()), "stop", None
            return self.send(200, {"ok": True})
        if p == "/api/admin/ap-pass/reveal":
            return self.send(200, {"ok": True, "ap_pass": "mock-ap-pass-1234"})
        if p == "/api/admin/ota":
            return self.send(409 if quiet() else 200, {"error": "capture"} if quiet() else {"error": "not in the mock"})
        return self.send(200, {"ok": True, "errors": []})

    def do_PUT(self):
        self.do_POST()

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8399)
    ap.add_argument("--node", help="node for terser (else FAUXCLOUD_NODE or PATH)")
    a = ap.parse_args()
    if a.node:
        os.environ["FAUXCLOUD_NODE"] = a.node
    S["port"] = a.port
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print("capture mock on http://localhost:%d/admin  (scenarios: /__scn?s=...)" % a.port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
