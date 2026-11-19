"""Forwarding reference: build one upload for one online weather service and, only with --send, send it.

The firmware's forwarding (firmware/fauxcloud/src/fwd.*) renders the same query strings byte for byte:
test/tools/gen_fwd_vectors.py renders the host test vectors with this file (the oracle), so a change here changes the
vectors. Laptop-side tool, not product code. Data comes from a synthetic fixture (fwd_reference_fixture.txt next to this
file: the console's WU query string) or from the bridge's read-only /api/live. Keys come from your own keys file outside
the repository, ~/.fauxcloud/secrets.forwarding.json (or the path in FAUXCLOUD_SECRETS), and are never printed: every
secret string is replaced by *** in all output and in the log. The file holds one block per service, e.g.
  {"wu": {"id": "<station id>", "key": "<station key>"}, "windy": {"id": ..., "key": ..., "url": "<optional template>"},
   "custom": {"<name>": {"url": "https://example.org/upload?k={key}&t={temp_c}", "key": "<secret>", "method": "GET",
                         "headers": {}, "body": null, "min_s": 300}}}
(also "pwsweather", "wow-be", "weathercloud", "awekas"); a block with an empty key is a dry run with placeholders.

Restraint rules (built in):
  * default is a dry run: print the redacted request and its size, send nothing;
  * --send sends exactly one request; a second send to the same service inside its minimum interval is refused;
  * --every MIN repeats, off by default, never faster than the service's minimum interval, at most --max-sends times;
  * nothing is sent when the station is offline or its outdoor sensor is stale (we never forward frozen data).
Never --send from the fixture: its values are invented and would land on a public page.

Usage (from the repo root, in a venv that holds `requests==2.32.5`):
  python -I tools/fwd_reference.py list
  python -I tools/fwd_reference.py wu --source fixture                 # dry run from the fixture
  python -I tools/fwd_reference.py wu --source live --send             # one real upload from the board's live values
  python -I tools/fwd_reference.py custom:mysite --source live         # a URL template from the keys file
"""
import argparse
import json
import math
import os
import re
import sys
import time
import urllib.parse
from datetime import datetime, timezone

import requests

HERE = os.path.dirname(os.path.abspath(__file__))
# Outside the repository on purpose: the keys stay out of every checkout and survive removing one.
SECRETS = os.environ.get("FAUXCLOUD_SECRETS") or os.path.join(os.path.expanduser("~"), ".fauxcloud",
                                                                "secrets.forwarding.json")
STATE_DIR = os.path.join(HERE, ".fwd_reference_state")  # gitignored: last-send times + the redacted request/response log
FIXTURE = os.path.join(HERE, "fwd_reference_fixture.txt")
UA = "fauxcloud-reference/0.1"
TIMEOUT_S = 10

# ---------------------------------------------------------------------------------------------------------------
# The canonical record: what fauxcloud holds, metric, one observation. Keys = /api/live `values` keys (docs/api.md §6.1).


def record_from_live(bridge, station_id):
    url = f"http://{bridge}/api/live"
    if station_id:
        url += "?station=" + urllib.parse.quote(station_id)
    r = requests.get(url, timeout=TIMEOUT_S, headers={"User-Agent": UA})
    r.raise_for_status()
    d = r.json()
    st = d.get("station")
    if st is None:
        sts = d.get("stations", [])
        if len(sts) != 1:
            sys.exit(f"bridge has {len(sts)} stations; pass --station")
        st = sts[0]
    rec = dict(st.get("values", {}))
    outlook_in = (st.get("outlook") or {}).get("in") or {}
    if outlook_in.get("wind10") is not None:
        rec["wind_avg10m_kmh"] = outlook_in["wind10"]  # mean wind over 10 min (the outlook input, docs/api.md §3.2.2)
    rec["_time"] = st.get("last_seen")
    rec["_online"] = bool(st.get("online"))
    rec["_stale"] = bool(st.get("sensor_stale"))
    rec["_age_s"] = st.get("age_s")
    rec["_profile"] = st.get("profile")
    rec["_derived"] = st.get("derived") or {}
    rec["_warming"] = st.get("warming") or []
    rec["_sources"] = st.get("sources") or {}
    rec["_time_synced"] = bool(d.get("time_synced"))
    return rec, None


def record_from_fixture(path, profile="yt60234"):
    raw = open(path, encoding="ascii").read().strip()
    q = dict(urllib.parse.parse_qsl(raw, keep_blank_values=True))
    lk = {k.lower(): v for k, v in q.items()}

    def num(k):
        v = lk.get(k.lower())
        try:
            return float(v) if v not in (None, "") else None
        except ValueError:
            return None

    f2c = lambda f: None if f is None else (f - 32) * 5 / 9
    rec = {
        "temp_c": f2c(num("tempf")),
        "dewpt_c": f2c(num("dewptf")),
        "humidity": num("humidity"),
        "pressure_hpa": None if num("baromin") is None else num("baromin") * 33.8638866667,
        "wind_kmh": None if num("windspeedmph") is None else num("windspeedmph") * 1.609344,
        "gust_kmh": None if num("windgustmph") is None else num("windgustmph") * 1.609344,
        "wind_dir": num("winddir"),
        "rain_daily_mm": None if num("dailyrainin") is None else num("dailyrainin") * 25.4,
        "uv": num("UV"),
        "solar_wm2": num("solarradiation"),
    }
    rin = num("rainin")
    if rin is not None:
        if profile == "yt60234":
            rec["rain_rate_mmh"] = rin * 25.4  # this console's rainin is a 10-min rate ×6 (docs/station-profiles.md)
        else:
            rec["rain_mm"] = rin * 25.4
    rec = {k: v for k, v in rec.items() if v is not None}
    # A fixture is a shape, not an observation: stamp it "now" so a service does not file it in the past.
    rec["_time"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    rec.update({"_online": True, "_stale": False, "_age_s": 0, "_profile": profile, "_derived": {}, "_warming": [],
                "_sources": {}, "_time_synced": True})
    return rec, raw


# ---------------------------------------------------------------------------------------------------------------
# Variables: the canonical record in every unit a service wants. A missing value -> the variable is absent.


def fmt(v, dec):
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return None
    s = f"{v:.{dec}f}"
    return s


def variables(rec):
    g = rec.get
    c2f = lambda c: None if c is None else c * 9 / 5 + 32
    kmh2mph = lambda k: None if k is None else k / 1.609344
    kmh2ms = lambda k: None if k is None else k / 3.6
    mm2in = lambda m: None if m is None else m / 25.4
    x10 = lambda v: None if v is None else str(int(round(v * 10)))
    first = lambda *ks: next((g(k) for k in ks if g(k) is not None), None)
    wind_mean = first("wind_avg10m_kmh", "wind_avg2m_kmh", "wind_kmh")  # the period's mean, best available
    gust_max = first("gust_10m_kmh", "gust_kmh")                        # the period's max, best available
    t = g("_time")
    dt = datetime.strptime(t, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc) if t else None
    v = {
        # WU / imperial names (the WU PWS protocol's own names)
        "tempf": fmt(c2f(g("temp_c")), 1),
        "dewptf": fmt(c2f(g("dewpt_c")), 1),
        "humidity": fmt(g("humidity"), 0),
        "baromin": fmt(None if g("pressure_hpa") is None else g("pressure_hpa") / 33.8638866667, 3),
        "windspeedmph": fmt(kmh2mph(g("wind_kmh")), 1),
        "windgustmph": fmt(kmh2mph(g("gust_kmh")), 1),
        "winddir": fmt(g("wind_dir"), 0),
        "windspdmph_avg2m": fmt(kmh2mph(g("wind_avg2m_kmh")), 1),
        "winddir_avg2m": fmt(g("wind_dir_avg2m"), 0),
        "windgustmph_10m": fmt(kmh2mph(g("gust_10m_kmh")), 1),
        "rainin": fmt(mm2in(g("rain_mm")), 3),  # WU meaning: rain over the past 60 min
        "dailyrainin": fmt(mm2in(g("rain_daily_mm")), 3),
        "rainratein": fmt(mm2in(g("rain_rate_mmh")), 3),
        "UV": fmt(g("uv"), 1),
        "solarradiation": fmt(g("solar_wm2"), 1),
        "windmean_mph": fmt(kmh2mph(wind_mean), 1),
        "gustmax_mph": fmt(kmh2mph(gust_max), 1),
        # metric
        "wind_mean_ms": fmt(kmh2ms(wind_mean), 1),
        "gust_max_ms": fmt(kmh2ms(gust_max), 1),
        "temp_c": fmt(g("temp_c"), 1),
        "dewpt_c": fmt(g("dewpt_c"), 1),
        "pressure_hpa": fmt(g("pressure_hpa"), 1),
        "wind_ms": fmt(kmh2ms(g("wind_kmh")), 1),
        "gust_ms": fmt(kmh2ms(g("gust_kmh")), 1),
        "wind_avg2m_ms": fmt(kmh2ms(g("wind_avg2m_kmh")), 1),
        "wind_avg10m_ms": fmt(kmh2ms(g("wind_avg10m_kmh")), 1),
        "gust_10m_ms": fmt(kmh2ms(g("gust_10m_kmh")), 1),
        "wind_kmh": fmt(g("wind_kmh"), 1),
        "gust_kmh": fmt(g("gust_kmh"), 1),
        "rain_1h_mm": fmt(g("rain_mm"), 2),
        "rain_daily_mm": fmt(g("rain_daily_mm"), 1),
        "rain_rate_mmh": fmt(g("rain_rate_mmh"), 1),
        # Weathercloud: metric integers ×10
        "wc_temp": x10(g("temp_c")),
        "wc_dew": x10(g("dewpt_c")),
        "wc_bar": x10(g("pressure_hpa")),
        "wc_wspd": x10(kmh2ms(g("wind_kmh"))),
        "wc_wspdavg": x10(kmh2ms(g("wind_avg10m_kmh"))),
        "wc_wspdhi": x10(kmh2ms(g("gust_10m_kmh") if g("gust_10m_kmh") is not None else g("gust_kmh"))),
        "wc_wdiravg": fmt(g("wind_dir_avg2m"), 0),
        "wc_rain": x10(g("rain_daily_mm")),
        "wc_rainrate": x10(g("rain_rate_mmh")),
        "wc_uvi": x10(g("uv")),
        "wc_solarrad": x10(g("solar_wm2")),
        # time
        "dateutc": dt.strftime("%Y-%m-%d %H:%M:%S") if dt else "now",
        "dateutc_iso": dt.strftime("%Y-%m-%dT%H:%M:%SZ") if dt else None,
        "epoch": str(int(dt.timestamp())) if dt else None,
        "wc_time": dt.strftime("%H%M") if dt else None,
        "wc_date": dt.strftime("%Y%m%d") if dt else None,
        "softwaretype": "fauxcloud-reference",
    }
    return {k: x for k, x in v.items() if x is not None}


# ---------------------------------------------------------------------------------------------------------------
# Templates. A template is a URL whose query pairs may hold {var} placeholders; a pair whose placeholder has no value
# is dropped (a missing reading is left out, never sent as 0). Placeholders in the path must all be present.
# {key} and {id} come from the service's secrets entry. Optional: method, headers, body (same placeholder rules for
# headers; a body is a whole-string template, missing value -> error).

PH = re.compile(r"\{([A-Za-z0-9_]+)\}")


def render_pairs(query, vals):
    out = []
    for k, v in urllib.parse.parse_qsl(query, keep_blank_values=True):
        names = PH.findall(v)
        if any(n not in vals for n in names):
            continue
        out.append((k, PH.sub(lambda m: vals[m.group(1)], v)))
    return out


def render_strict(s, vals, what):
    missing = [n for n in PH.findall(s) if n not in vals]
    if missing:
        raise SystemExit(f"{what}: no value for {missing}")
    return PH.sub(lambda m: urllib.parse.quote(vals[m.group(1)], safe=""), s)


def render(tpl, vals):
    u = urllib.parse.urlsplit(tpl["url"])
    path = render_strict(u.path, vals, "path")
    pairs = render_pairs(u.query, vals)
    url = urllib.parse.urlunsplit((u.scheme, u.netloc, path, urllib.parse.urlencode(pairs), ""))
    headers = {"User-Agent": UA}
    for hk, hv in (tpl.get("headers") or {}).items():
        headers[hk] = PH.sub(lambda m: vals.get(m.group(1), ""), hv)
    body = None
    if tpl.get("body") is not None:
        b = tpl["body"]
        missing = [n for n in PH.findall(b) if n not in vals]
        if missing:
            raise SystemExit(f"body: no value for {missing}")
        body = PH.sub(lambda m: vals[m.group(1)], b)
    return tpl.get("method", "GET").upper(), url, headers, body


# ---------------------------------------------------------------------------------------------------------------
# Presets. min_s = the minimum interval we hold ourselves to (the service's documented one, or ours where the doc
# is silent: never faster than the console's own 60 s).

WU_FIELDS = ("&dateutc={dateutc}&tempf={tempf}&dewptf={dewptf}&humidity={humidity}&baromin={baromin}"
             "&windspeedmph={windspeedmph}&windgustmph={windgustmph}&winddir={winddir}"
             "&windspdmph_avg2m={windspdmph_avg2m}&winddir_avg2m={winddir_avg2m}&windgustmph_10m={windgustmph_10m}"
             "&rainin={rainin}&dailyrainin={dailyrainin}&UV={UV}&solarradiation={solarradiation}"
             "&softwaretype={softwaretype}")

PRESETS = {
    "wu": {
        "doc": "Weather Underground PWS upload, clean record (WU semantics: rainin = past 60 min)",
        "url": "https://rtupdate.wunderground.com/weatherstation/updateweatherstation.php"
               "?ID={id}&PASSWORD={key}&action=updateraw" + WU_FIELDS,
        "min_s": 60,
    },
    "pwsweather": {
        "doc": "PWSweather (Xweather): WU-style names, station ID + API key",
        "url": "https://pwsupdate.pwsweather.com/api/v1/submitwx?ID={id}&PASSWORD={key}" + WU_FIELDS,
        "min_s": 300,
    },
    "wow-be": {
        "doc": "WOW-BE (RMI; also WOW-NL), WU protocol on /api/v2/send; rainin = RATE in/h here; WOW-UK retires 2026",
        "url": "https://wow.meteo.be/api/v2/send?ID={id}&PASSWORD={key}&action=updateraw"
               "&dateutc={dateutc}&tempf={tempf}&dewptf={dewptf}&humidity={humidity}&baromin={baromin}"
               "&windspeedmph={windspeedmph}&windgustmph={windgustmph}&winddir={winddir}"
               "&rainin={rainratein}&dailyrainin={dailyrainin}&UV={UV}&solarradiation={solarradiation}"
               "&softwaretype={softwaretype}",
        "min_s": 60,
    },
    "awekas": {
        "doc": "AWEKAS WU-compatible path (forum-documented): ID = AWEKAS user name, PASSWORD = plain data password",
        "url": "https://ws.awekas.at/weatherstation/updateweatherstation.php?ID={id}&PASSWORD={key}&action=updateraw"
               + WU_FIELDS,
        "min_s": 300,
    },
    "weathercloud": {
        "doc": "Weathercloud device API v01, query style, metric x10",
        "url": "https://api.weathercloud.net/v01/set?wid={id}&key={key}&temp={wc_temp}&dew={wc_dew}&hum={humidity}"
               "&bar={wc_bar}&wspd={wc_wspd}&wspdavg={wc_wspdavg}&wspdhi={wc_wspdhi}&wdir={winddir}"
               "&wdiravg={wc_wdiravg}&rain={wc_rain}&rainrate={wc_rainrate}&uvi={wc_uvi}&solarrad={wc_solarrad}"
               "&time={wc_time}&date={wc_date}&software={softwaretype}",
        "min_s": 600,
    },
    "windy": {
        "doc": "Windy stations API v2 (from 2026-01; v1 dies end-2026), metric names, station id + station password",
        "url": "https://stations.windy.com/api/v2/observation/update?id={id}&PASSWORD={key}&time={dateutc_iso}"
               "&temp={temp_c}&dewpoint={dewpt_c}&humidity={humidity}&mbar={pressure_hpa}&wind={wind_mean_ms}"
               "&gust={gust_max_ms}&winddir={winddir}&precip={rain_1h_mm}&uv={UV}&solarradiation={solarradiation}"
               "&softwaretype={softwaretype}",
        "min_s": 300,
    },
    "windy-wu": {
        "doc": "Windy v2 WU-compatible alias (wu.windy.com/wu), WU names: the 'one WU string, other host' test",
        "url": "https://wu.windy.com/wu?ID={id}&PASSWORD={key}" + WU_FIELDS,
        "min_s": 300,
    },
}


# Presets that share one account (and so one rate limit) with another preset.
ACCOUNT = {"windy-wu": "windy", "wu-verbatim": "wu"}


def console_line(rec):
    """What the YT60234 console would have sent for this record (docs/station-profiles.md): its field set and order,
    rainin = the RATE, solarRadiation camelCase, dateutc unpadded, no action/softwaretype. Lets the relay option be
    tested with real values instead of the fixture's invented ones (which must never reach a public page)."""
    g = rec.get
    c2f = lambda c: c * 9 / 5 + 32
    t = datetime.strptime(rec["_time"], "%Y-%m-%dT%H:%M:%SZ")
    pairs = [("ID", "EXAMPLE1"), ("PASSWORD", "examplekey"),
             ("dateutc", f"{t.year}-{t.month}-{t.day} {t.hour}:{t.minute:02d}:{t.second:02d}")]
    def add(k, v, dec):
        if v is not None:
            pairs.append((k, f"{v:.{dec}f}"))
    add("baromin", None if g("pressure_hpa") is None else g("pressure_hpa") / 33.8638866667, 2)
    add("tempf", None if g("temp_c") is None else c2f(g("temp_c")), 1)
    add("humidity", g("humidity"), 0)
    add("dewptf", None if g("dewpt_c") is None else c2f(g("dewpt_c")), 1)
    add("rainin", None if g("rain_rate_mmh") is None else g("rain_rate_mmh") / 25.4, 2)
    add("dailyrainin", None if g("rain_daily_mm") is None else g("rain_daily_mm") / 25.4, 2)
    add("winddir", g("wind_dir"), 0)
    add("windspeedmph", None if g("wind_kmh") is None else g("wind_kmh") / 1.609344, 1)
    add("windgustmph", None if g("gust_kmh") is None else g("gust_kmh") / 1.609344, 1)
    add("UV", g("uv"), 0)
    add("solarRadiation", g("solar_wm2"), 1)
    return urllib.parse.urlencode(pairs)


def verbatim_wu(raw, sec, scheme, keep_date=False):
    """The relay option: the console's own query string, only ID/PASSWORD swapped (dateutc refreshed for a fixture)."""
    pairs = []
    for k, v in urllib.parse.parse_qsl(raw, keep_blank_values=True):
        if k == "ID":
            v = sec["id"]
        elif k == "PASSWORD":
            v = sec["key"]
        elif k == "dateutc" and not keep_date:
            v = "now"
        pairs.append((k, v))
    url = f"{scheme}://rtupdate.wunderground.com/weatherstation/updateweatherstation.php?" + urllib.parse.urlencode(pairs)
    return "GET", url, {"User-Agent": UA}, None


# ---------------------------------------------------------------------------------------------------------------


def load_secrets():
    if not os.path.exists(SECRETS):
        return {}
    with open(SECRETS, encoding="utf-8") as f:
        return json.load(f)


def secret_strings(sec):
    out = []
    for k, v in sec.items():
        if k in ("key", "id", "token", "password") and isinstance(v, str) and v:
            out.append(v)
        if k == "headers" and isinstance(v, dict):
            out += [x for x in v.values() if isinstance(x, str) and x]
    return out


def redact(s, secrets):
    if s is None:
        return None
    for x in sorted(secrets, key=len, reverse=True):
        for form in {x, urllib.parse.quote(x, safe=""), urllib.parse.quote_plus(x)}:
            s = s.replace(form, "***")
    return s


def wire_size(method, url, headers, body):
    """Bytes an ESP would put on the wire: request line + Host + the given headers + Connection: close + body."""
    u = urllib.parse.urlsplit(url)
    target = u.path + ("?" + u.query if u.query else "")
    lines = [f"{method} {target} HTTP/1.1", f"Host: {u.netloc}"]
    lines += [f"{k}: {v}" for k, v in headers.items()]
    if body is not None:
        lines.append(f"Content-Length: {len(body.encode())}")
    lines.append("Connection: close")
    head = "\r\n".join(lines) + "\r\n\r\n"
    return len(head.encode()) + (len(body.encode()) if body else 0)


def state_load():
    p = os.path.join(STATE_DIR, "last_send.json")
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def state_save(st):
    os.makedirs(STATE_DIR, exist_ok=True)
    with open(os.path.join(STATE_DIR, "last_send.json"), "w", encoding="utf-8") as f:
        json.dump(st, f, indent=1)


def log(entry):
    os.makedirs(STATE_DIR, exist_ok=True)
    with open(os.path.join(STATE_DIR, "log.jsonl"), "a", encoding="utf-8") as f:
        f.write(json.dumps(entry) + "\n")


def build(service, rec, raw, secrets_all, scheme_http):
    vals = variables(rec)
    if service == "wu-verbatim":
        live = raw is None
        if live:
            raw = console_line(rec)  # the bridge does not expose the raw line: rebuild it in the console's shape
        sec = secrets_all.get("wu") or {}
        if not sec.get("id") or not sec.get("key"):
            sec = {"id": "EXAMPLE1", "key": "examplekey"}
        return verbatim_wu(raw, sec, "http" if scheme_http else "https", keep_date=live), sec, 60
    if service.startswith("custom:"):
        name = service.split(":", 1)[1]
        sec = (secrets_all.get("custom") or {}).get(name)
        if not sec:
            sys.exit(f"no custom.{name} in {SECRETS}")
        tpl = sec
        min_s = int(sec.get("min_s", 300))
    else:
        tpl = PRESETS.get(service)
        if not tpl:
            sys.exit(f"unknown service {service}; try: list")
        sec = secrets_all.get(ACCOUNT.get(service, service)) or {}
        if sec.get("url") and service not in ACCOUNT:
            tpl = dict(tpl, url=sec["url"])  # the secrets entry may override the endpoint (e.g. a new API version)
        min_s = tpl["min_s"]
    if not sec.get("id") and not sec.get("key"):
        sec = dict(sec, id="EXAMPLE1", key="examplekey")  # dry run without an account: placeholders
    vals = dict(vals, id=sec.get("id", ""), key=sec.get("key", ""))
    for hk, hv in (sec.get("headers") or {}).items():
        tpl = dict(tpl, headers=dict(tpl.get("headers") or {}, **{hk: hv}))
    method, url, headers, body = render(tpl, vals)
    if scheme_http and url.startswith("https://"):
        url = "http://" + url[len("https://"):]
    return (method, url, headers, body), sec, min_s


def one(args, secrets_all):
    if args.source == "live":
        rec, raw = record_from_live(args.bridge, args.station)
    else:
        rec, raw = record_from_fixture(args.fixture, args.profile)
    (method, url, headers, body), sec, min_s = build(args.service, rec, raw, secrets_all, args.http)
    if args.extra:  # isolate one parameter's effect (e.g. --extra action=updateraw on wu-verbatim)
        url += ("&" if "?" in url else "?") + urllib.parse.urlencode([tuple(x.split("=", 1)) for x in args.extra])
    secs = secret_strings(sec)
    real = bool(sec.get("key")) and sec.get("key") != "examplekey"
    print(f"service   {args.service}   min interval {min_s} s   source {args.source}")
    print(f"record    time {rec['_time']}  online {rec['_online']}  stale {rec['_stale']}  age_s {rec['_age_s']}"
          f"  profile {rec['_profile']}  derived {sorted(rec['_derived'])}  warming {rec['_warming']}"
          f"  sources {rec['_sources']}")
    print(f"request   {method} {redact(url, secs)}")
    for k, v in headers.items():
        print(f"header    {k}: {redact(v, secs)}")
    if body is not None:
        print(f"body      {redact(body, secs)}")
    size = wire_size(method, url, headers, body)
    print(f"wire      {size} B request ({'TLS' if url.startswith('https') else 'plain HTTP'})")
    if not args.send:
        print("dry run   nothing sent (add --send)")
        return None
    if not real:
        sys.exit(f"no real key for {args.service} in {SECRETS}; fill it in first")
    if not rec["_online"] or rec["_stale"]:
        sys.exit("refused: the station is offline or its outdoor sensor is stale (never forward frozen data)")
    if not rec["_time_synced"]:
        sys.exit("refused: the bridge has no real time")
    st = state_load()
    acct = ACCOUNT.get(args.service, args.service)
    last = st.get(acct, 0)
    wait = last + min_s - time.time()
    if wait > 0:
        sys.exit(f"refused: last send {int(time.time() - last)} s ago; this service's minimum is {min_s} s")
    t0 = time.monotonic()
    err = None
    try:
        r = requests.request(method, url, headers=headers, data=body.encode() if body else None, timeout=TIMEOUT_S,
                             allow_redirects=False)
        status, text, rh = r.status_code, r.text, dict(r.headers)
    except requests.RequestException as e:
        status, text, rh, err = None, "", {}, type(e).__name__ + ": " + redact(str(e), secs)
    ms = int((time.monotonic() - t0) * 1000)
    st[acct] = time.time()
    state_save(st)
    resp_head = sum(len(k) + len(v) + 4 for k, v in rh.items()) + 17
    entry = {"t": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "service": args.service,
             "source": args.source, "method": method, "url": redact(url, secs), "req_bytes": size, "status": status,
             "location": redact(rh.get("Location"), secs), "resp_head_bytes": resp_head if rh else None,
             "resp_body_bytes": len(text.encode()) if text else 0, "body": redact(text[:300], secs), "ms": ms,
             "error": err}
    log(entry)
    print(f"response  {status}  {ms} ms  body {len(text.encode()) if text else 0} B  location {entry['location']}")
    print(f"body      {entry['body']!r}")
    if err:
        print(f"error     {err}")
    return entry


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("service", help="list | wu | wu-verbatim | pwsweather | wow-be | awekas | weathercloud | windy | windy-wu | custom:<name>")
    ap.add_argument("--source", choices=("fixture", "live"), default="fixture")
    ap.add_argument("--fixture", default=FIXTURE)
    ap.add_argument("--profile", default="yt60234", choices=("yt60234", "generic"), help="how the fixture's rainin reads")
    ap.add_argument("--bridge", default="fauxcloud.local")
    ap.add_argument("--station", default=None)
    ap.add_argument("--http", action="store_true", help="plain HTTP instead of HTTPS (to test what the ESP could do)")
    ap.add_argument("--send", action="store_true", help="really send (one request)")
    ap.add_argument("--every", type=float, default=0, help="minutes between sends; 0 = off (default)")
    ap.add_argument("--max-sends", type=int, default=6)
    ap.add_argument("--extra", action="append", default=[], help="append k=v to the query (repeatable)")
    args = ap.parse_args()
    secrets_all = load_secrets()
    if args.service == "list":
        for k, p in PRESETS.items():
            have = "key set" if (secrets_all.get(ACCOUNT.get(k, k)) or {}).get("key") else "no key"
            print(f"{k:13} min {p['min_s']:4} s  {have:7}  {p['doc']}")
        print(f"{'wu-verbatim':13} min   60 s  (uses wu)  the console's own query string, ID/PASSWORD swapped")
        for k in (secrets_all.get("custom") or {}):
            print(f"custom:{k}")
        return
    if args.every and not args.send:
        sys.exit("--every needs --send")
    p = PRESETS.get(args.service)
    if args.every and p and args.every * 60 < p["min_s"]:
        sys.exit(f"--every {args.every} min is faster than {args.service}'s minimum of {p['min_s']} s")
    one(args, secrets_all)
    if args.every:
        n = 1
        while n < args.max_sends:
            time.sleep(args.every * 60)
            one(args, secrets_all)
            n += 1


if __name__ == "__main__":
    main()
