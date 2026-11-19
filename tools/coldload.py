"""tools/coldload.py -- the two-device cold-load replay: the acceptance tool for every
change to the web budgets.

Two simulated devices load the dashboard at the same moment, each like a browser with an empty cache: the shell, then
its stylesheet and script in parallel, then /api/live, /api/bridge/status, /api/hourly, /api/recent and the 90-day
flash view /api/daily?days=90 (what the history sheet asks for) one at a time. Meanwhile a
console upload is replayed every 0.5 s until both devices finish (at least --uploads of them) and its reply time is
measured, so "max upload" also sees the flash view (the console's path must stay fast while pages load). Before and after the rounds it prints the board's own counters from /api/live (diag):
http_busy_ms_max, heap_min_10m, heap_largest.

Usage (LAN side, Python 3, stdlib only):
  python -I tools/coldload.py <board-address> --station <ID> --upload-id <ID> [--rounds 3] [--lang pl] [--page admin]

  --station    an existing station id: its /api/* views are what the page loads
  --upload-id  the id the replayed uploads use (required: pick a throwaway one). The board accepts LAN uploads only
               with "Accept uploads from my home network" on; then this id becomes a real station (and an HA device
               with auto-publish) -- forget it in the admin afterwards. With LAN uploads off the replies are still
               timed (the catch-all answers them).

  --lang       a first visit in that language: after the scripts, each device fetches the page's initial language
               packs one at a time before its first API call, as lang.js queues them (the pack paths are read from
               the manifest in the page's main script, the language button's choice is not needed)
  --page       admin: the admin's cold load up to the sign-in check (/admin, its assets, the packs with --lang, then
               /api/admin/settings, answered 401 without a session); --station is not used there

Nothing here is specific to one board: the asset paths are read from the shell the board serves.
"""
import argparse
import http.client
import json
import re
import threading
import time


def main():
    ap = argparse.ArgumentParser(description="fauxcloud two-device cold-load replay")
    ap.add_argument("host")
    ap.add_argument("--station", default="")
    ap.add_argument("--upload-id", required=True)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--uploads", type=int, default=4, help="uploads replayed per round, at least (one every 0.5 s)")
    ap.add_argument("--lang", default="", help="a first visit in this language (its initial packs before the first API call)")
    ap.add_argument("--page", choices=("dash", "admin"), default="dash")
    a = ap.parse_args()
    if a.page == "dash" and not a.station:
        ap.error("--station is required for the dashboard")

    def fetch(path, timeout=10):
        c = http.client.HTTPConnection(a.host, 80, timeout=timeout)
        c.request("GET", path, headers={"Accept-Encoding": "gzip"})
        r = c.getresponse()
        body = r.read()
        c.close()
        return r.status, body

    def diag():
        d = json.loads(fetch("/api/live")[1])["diag"]
        return {k: d.get(k) for k in ("http_busy_ms_max", "heap_min_10m", "heap_largest", "heap_free")}  # byte-addressable heap figures

    # the shell's own asset references (hashed paths), as a browser would find them
    import gzip
    root = "/admin" if a.page == "admin" else "/"
    st, shell = fetch(root)
    if shell[:2] == b"\x1f\x8b":
        shell = gzip.decompress(shell)
    assets = sorted(set(re.findall(rb'/a/[0-9a-f]{8}/[A-Za-z0-9_.-]+\.(?:css|js)', shell)))
    assets = [x.decode() for x in assets]
    print("shell %d, %d B; assets: %s" % (st, len(shell), ", ".join(assets)))
    packs = []
    if a.lang:
        # lang.js's manifest in the page's scripts: "<script>":"/a/<hash>/<name>.<lang>.json" and _i:[the initial scripts]
        # (the minifier writes the key bare, _i:[...]; the generator's JSON quotes it, "_i":[...]: either)
        for p in (x for x in assets if x.endswith(".js")):
            body = fetch(p)[1]
            body = gzip.decompress(body) if body[:2] == b"\x1f\x8b" else body
            m = re.search(rb'"?_i"?:\[([^\]]*)\]', body)
            if m:
                where = dict(re.findall(rb'"([\w.-]+\.js)":"(/a/[0-9a-f]{8}/[\w.-]+\.' + re.escape(a.lang.encode()) + rb'\.json)"', body))
                packs = [where[s].decode() for s in re.findall(rb'"([\w.-]+\.js)"', m.group(1)) if s in where]
        if not packs:
            raise SystemExit("no %s packs in the page's manifest: is this an image with language packs?" % a.lang)
        print("first visit in %s: packs %s" % (a.lang, ", ".join("%s %d B" % (p, len(fetch(p)[1])) for p in packs)))
    sid = a.station
    api = (("/api/admin/settings",) if a.page == "admin" else
           ("/api/live?station=%s" % sid, "/api/bridge/status", "/api/hourly?station=%s" % sid,
            "/api/recent?station=%s" % sid, "/api/daily?station=%s&days=90" % sid))
    up = ("/weatherstation/updateweatherstation.php?ID=%s&PASSWORD=coldload&dateutc=now&tempf=51.0&humidity=60"
          "&winddir=240&windspeedmph=5&windgustmph=9&baromin=29.92&rainin=0&dailyrainin=0.12" % a.upload_id)

    print("diag before:", diag())
    summary = []
    for rnd in range(1, a.rounds + 1):
        log, lock = [], threading.Lock()

        def get(path, tag):
            t = time.time()
            try:
                st, body = fetch(path)
            except OSError as e:  # a timeout or a reset counts as an error of the round, not a crash of the tool
                st, body = "err:%s" % type(e).__name__, b""
            with lock:
                log.append((t, time.time(), tag, path.split("?")[0], st, len(body)))

        def device(tag):
            get("/", tag)
            ts = [threading.Thread(target=get, args=(p, tag)) for p in assets]
            [t.start() for t in ts]
            [t.join() for t in ts]
            for p in packs:  # lang.js: the initial packs first in the one-request queue
                get(p, tag)
            for p in api:
                get(p, tag)

        time.sleep(3)  # the board settles between rounds
        t0 = time.time()
        ds = [threading.Thread(target=device, args=(d,)) for d in ("A", "B")]
        [d.start() for d in ds]
        ups = []
        while len(ups) < a.uploads or any(d.is_alive() for d in ds):  # spread over the whole round
            time.sleep(0.05 if not ups else 0.5)
            t = time.time()
            get(up, "upload")
            ups.append((time.time() - t) * 1e3)
        [d.join() for d in ds]
        ev = sorted([(x[0], 1) for x in log] + [(x[1], -1) for x in log])
        cur = mx = 0
        for _, d in ev:
            cur += d
            mx = max(mx, cur)
        total = (max(x[1] for x in log) - t0) * 1e3
        bad = [x for x in log if x[4] not in ((200, 304, 401) if a.page == "admin" else (200, 304))]
        errs = [x for x in bad if isinstance(x[4], str)]
        print("round %d: uploads %s ms (max %.0f); whole load %.0f ms; max open %d; non-200: %d (timeouts/resets %d)" %
              (rnd, ", ".join("%.0f" % u for u in ups), max(ups), total, mx, len(bad), len(errs)))
        summary.append((max(ups), total))
    print("diag after:", diag())
    print("max upload over the rounds %.0f ms; whole load %s ms" %
          (max(s[0] for s in summary), ", ".join("%.0f" % s[1] for s in summary)))
    print("forget station %s in the admin afterwards (Admin -> the station -> Forget) if LAN uploads were on"
          % a.upload_id)


if __name__ == "__main__":
    main()
