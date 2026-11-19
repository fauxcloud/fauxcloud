"""Forecast dry run: pull the bridge's history, keep the raw rows, print the stage-1 facts and
the Zambretti numbers. Usage: python -I tools/forecast_trial.py [--bridge fauxcloud.local] [--station ID] [--out DIR]
Raw data goes to --out (default captures/forecast-trial/, gitignored) (CSV, appended and de-duplicated; the live outlook block, one row per run, to
<date>-outlook.csv); the calculations go to
stdout. Times: UTC in the CSV, the bridge's local time in the
output. The time zone (POSIX string) and the location come from the bridge's /api/bridge; the location is used for
the tide and the solar time only and is never printed or stored.
"""
import argparse
import csv
import json
import math
import os
import urllib.request
import re
from datetime import datetime, timedelta, timezone

OUT = os.path.join(os.path.dirname(__file__), "..", "captures", "forecast-trial")
ZAMBRETTI_STEP = 1.6  # hPa / 3 h: falling / steady / rising
TZ = {"std": timedelta(hours=1), "dst": None, "rules": None}  # set from the bridge in main()
LAT, LON = 52.0, 19.0  # replaced by the bridge's effective location in main()


def parse_posix_tz(s):
    """'CET-1CEST,M3.5.0,M10.5.0/3' -> std/dst offsets east of UTC + the two Mm.w.d[/h] rules (all the bridge uses)."""
    m = re.match(r"^(?:<[^>]+>|[A-Za-z]+)([+-]?\d+)(?::(\d+))?(?:(?:<[^>]+>|[A-Za-z]+)(?:([+-]?\d+)(?::(\d+))?)?"
                 r",M(\d+)\.(\d)\.(\d)(?:/(\d+))?,M(\d+)\.(\d)\.(\d)(?:/(\d+))?)?$", s)
    if not m:
        return
    sign = -1 if not m.group(1).startswith("-") else 1  # POSIX offsets are west-positive
    std = timedelta(hours=sign * abs(int(m.group(1))), minutes=sign * int(m.group(2) or 0))
    TZ["std"] = std
    if m.group(5):
        dst = std + timedelta(hours=1) if m.group(3) is None else timedelta(
            hours=(-1 if not m.group(3).startswith("-") else 1) * abs(int(m.group(3))))
        TZ["dst"] = dst
        TZ["rules"] = [tuple(int(x) for x in m.group(5, 6, 7)) + (int(m.group(8) or 2),),
                       tuple(int(x) for x in m.group(9, 10, 11)) + (int(m.group(12) or 2),)]


def _rule_day(year, mon, week, dow):
    first = datetime(year, mon, 1)
    d = (dow - (first.weekday() + 1) % 7) % 7 + 1 + (week - 1) * 7
    while d > 28:
        try:
            datetime(year, mon, d)
            break
        except ValueError:
            d -= 7
    return d


def offset(dt_utc):
    if not TZ["rules"]:
        return TZ["std"]
    (m1, w1, d1, h1), (m2, w2, d2, h2) = TZ["rules"]
    y = dt_utc.year
    on = datetime(y, m1, _rule_day(y, m1, w1, d1), h1, tzinfo=timezone.utc) - TZ["std"]
    off = datetime(y, m2, _rule_day(y, m2, w2, d2), h2, tzinfo=timezone.utc) - TZ["dst"]
    return TZ["dst"] if on <= dt_utc < off else TZ["std"]


def get(bridge, path):
    for attempt in range(4):  # a weak Wi-Fi link cuts chunked answers; retry, do not crash
        try:
            with urllib.request.urlopen(f"http://{bridge}{path}", timeout=20) as r:
                return json.load(r)
        except (OSError, ValueError):
            if attempt == 3:
                raise


def log_aviation(path, icao):
    """Appends the current METAR and TAF (AWC API, raw) for validation; a report already logged is not repeated."""
    seen = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%MZ")
    with open(path, "a", encoding="utf-8") as f:
        for kind in ("metar", "taf"):
            # the METARs of the last 26 h, so each daily run keeps the whole day (de-duplicated below)
            url = f"https://aviationweather.gov/api/data/{kind}?ids={icao}&format=raw" + ("&hours=26" if kind == "metar" else "")
            try:
                with urllib.request.urlopen(url, timeout=20) as r:
                    text = r.read().decode("utf-8", "replace").strip()
            except OSError as e:
                f.write(f"# {stamp} {kind} fetch failed: {e}\n")
                continue
            for rep in [x for x in text.split("\n\n") if x.strip()] if kind == "taf" else text.splitlines():
                if rep.strip() and rep.strip() not in seen:
                    f.write(f"# fetched {stamp} ({kind})\n{rep.strip()}\n")


def utc(ts):
    return datetime.fromtimestamp(ts, timezone.utc)


def loc(dt):
    return (dt + offset(dt)).strftime("%H:%M")


def merge_recent(path, fields, rows):
    have = {}
    if os.path.exists(path):
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                have[int(r["t"])] = r
    for row in rows:
        have[row[0]] = {"t": row[0], "utc": utc(row[0]).strftime("%Y-%m-%dT%H:%M:%SZ"),
                        **{k: ("" if v is None else v) for k, v in zip(fields[1:], row[1:])}}
    cols = ["t", "utc"] + fields[1:]
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, cols)
        w.writeheader()
        for t in sorted(have):
            w.writerow({c: have[t].get(c, "") for c in cols})
    return [have[t] for t in sorted(have)]


def write_hourly(path, doc):
    cols = ["hour_utc", "n", "p_lo", "p_mean", "p_hi", "t_mean", "dew_mean", "rh_mean", "wind_mean", "gust_max",
            "wind_dir", "rain_mm", "solar_mean"]
    hours = doc["hours"] + ([doc["open"]] if doc.get("open") else [])
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(cols)
        for h in hours:
            m = lambda k, i=1: (h.get(k) or [None] * 3)[i]
            w.writerow([h["h"], h["n"], m("pressure_hpa", 0), m("pressure_hpa"), m("pressure_hpa", 2), m("temp_c"),
                        m("dewpt_c"), m("humidity"), m("wind_kmh"), h.get("gust_kmh"), h.get("wind_dir"),
                        h.get("rain_mm"), m("solar_wm2")])
    return hours


OUTLOOK_COLS = ["run_utc", "station", "t", "v", "gate", "state", "qual", "next", "warn", "left_h", "tend_d", "tend_code",
                "tend_tide", "tend_fast", "p_gust", "p_since", "in_night", "in_night_src", "in_t", "in_td", "in_rate",
                "in_rain12", "in_gust10", "in_wind_h", "in_calm", "in_dt_h", "in_cool_at", "in_rh", "in_wind10",
                "in_near"]  # the ICAO behind a shown T3 code


def log_outlook(path, sid, o):
    """Appends the live outlook block as one row: the strip's codes next to the raw rows."""
    if not o:
        return
    t, p, i = o.get("tend") or {}, o.get("p") or {}, o.get("in") or {}
    row = {"run_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "station": sid}
    row.update({k: o.get(k) for k in ("t", "v", "gate", "state", "qual", "next", "warn", "left_h")})
    row.update({"tend_" + k: t.get(k) for k in ("d", "code", "tide", "fast")})
    row.update({"p_" + k: p.get(k) for k in ("gust", "since")})
    row.update({"in_" + k: i.get(k) for k in ("night", "night_src", "t", "td", "rate", "rain12", "gust10", "wind_h",
                                              "calm", "dt_h", "cool_at", "rh", "wind10")})
    row["in_near"] = (i.get("near") or {}).get("icao")
    if os.path.exists(path):  # added columns: an older header starts a new file beside it
        with open(path, encoding="utf-8") as f:
            if f.readline().strip().split(",") != OUTLOOK_COLS:
                path = path.replace(".csv", "-v2.csv")
    new = not os.path.exists(path)
    with open(path, "a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, OUTLOOK_COLS)
        if new:
            w.writeheader()
        w.writerow({k: ("" if row.get(k) is None else row[k]) for k in OUTLOOK_COLS})


NIGHT_V1 = ("start", "len_h", "hours", "ok", "calm", "rain", "hoar", "restart", "rh_max", "t2", "td2", "t0", "dt3", "tmin")
GATES_V1 = ("quality", "readings", "physics", "frozen", "indoor", "geo", "dome", "iced", "stuck", "steady", "k_range", "k_low",
            "cap", "location", "baro", "scratch")
NIGHT_L5L6 = ("l_h", "w5", "rise", "l6", "pred")
GATES_L5L6 = ("l5_label", "l5_evening", "l6_night")


def learn_path(path, cols):
    """The day file when it is absent or its header is `cols`; else the first `-v<len>[-k].csv` beside it
    that is absent or has `cols` as its header. Every candidate's header is read on every run; a header is only ever
    written to a new file, so two layouts never share one file."""
    stem = path[:-4]
    for p in [path, "%s-v%d.csv" % (stem, len(cols))] + ["%s-v%d-%d.csv" % (stem, len(cols), k) for k in range(2, 100)]:
        if not os.path.exists(p):
            return p
        with open(p, encoding="utf-8") as f:
            if f.readline().rstrip("\r\n").split(",") == cols:
                return p
    raise RuntimeError("no free learn CSV beside " + path)


def log_learn(path, sid, d):
    """Appends the learners' state (/api/learn) as one row: per learner value / n / conf /
    hit, the 12 L1 cells, the L1b counts and the last night summary. Private: it stays in the gitignored out folder."""
    if not d:
        return
    row = {"run_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "station": sid, "state": d.get("state"),
           "last_hour": d.get("last_hour"), "backfill_h": (d.get("backfill") or {}).get("hours")}
    for c in (d.get("l1") or {}).get("cells", []):
        k = "l1_%d%s" % (c["el"], "pm" if c["pm"] else "am")
        row.update({k + "_e": c["e"], k + "_n": c["n"], k + "_conf": c["conf"]})
    b = d.get("l1b") or {}
    # the mask itself (base64, 2 bits per cell) so the horizon can be checked against the site
    row.update({"l1b_sun": b.get("sun"), "l1b_shade": b.get("shade"), "l1b_unknown": b.get("unknown"), "l1b_mask": b.get("mask")})
    l2 = d.get("l2") or {}
    row.update({"l2_s": l2.get("s"), "l2_src": l2.get("src"), "l2_wet_e": (l2.get("wet") or {}).get("e"),
                "l2_wet_n": (l2.get("wet") or {}).get("n"), "l2_night_e": (l2.get("night") or {}).get("e"),
                "l2_night_n": (l2.get("night") or {}).get("n")})
    for k in ("l3", "l4", "l7"):
        for f, v in (d.get(k) or {}).items():
            row[k + "_" + f] = v
    # the first night and gate columns keep their places; every newer key goes to the end of the row
    last = ((d.get("night") or {}).get("last")) or {}
    gates = d.get("gates") or {}
    with_l5 = "l5" in d  # a board with L5 / L6: a fixed column set, empty cells where the board has no value yet
    for f in NIGHT_V1:
        if with_l5 or f in last:
            row["night_" + f] = last.get(f)
    for f in GATES_V1:
        if with_l5 or f in gates:
            row["gate_" + f] = gates.get(f)
    if with_l5:
        l5, l6 = d.get("l5") or {}, d.get("l6") or {}
        row.update({"l5_thr": l5.get("thr"), "l5_src": l5.get("src"), "l5_cn": (l5.get("clear") or {}).get("n"),
                    "l5_on": (l5.get("overcast") or {}).get("n"), "l6_a": l6.get("a"), "l6_b": l6.get("b"), "l6_c": l6.get("c"),
                    "l6_n": l6.get("n"), "l6_sd": l6.get("sd"), "l6_conf": l6.get("conf")})
        for f in NIGHT_L5L6:
            row["night_" + f] = last.get(f)
        for f in GATES_L5L6:
            row["gate_" + f] = gates.get(f)
    for f, v in last.items():  # keys of a later firmware
        if "night_" + f not in row:
            row["night_" + f] = v
    for f, v in gates.items():
        if "gate_" + f not in row:
            row["gate_" + f] = v
    if "migrated_from" in d:  # a board that migrates learner blobs: the migration figures, at the end of the row
        rp = (d.get("backfill") or {}).get("replay") or {}
        row.update({"migrated_from": d.get("migrated_from"), "replay_h": rp.get("hours"), "replay_ms": rp.get("ms")})
    if "bars" in d:  # a board with the learner bars: states, needs and the outlook's `use` snapshot, at the end of the row
        for c in (d.get("l1") or {}).get("cells", []):
            k = "l1_%d%s" % (c["el"], "pm" if c["pm"] else "am")
            row.update({k + "_state": c.get("state"), k + "_need": c.get("need"), k + "_from": c.get("from")})
        l5, l6 = d.get("l5") or {}, d.get("l6") or {}
        row.update({"l2_state": l2.get("state"), "l2_wet_need": (l2.get("wet") or {}).get("need"),
                    "l2_night_need": (l2.get("night") or {}).get("need"), "l5_state": l5.get("state"),
                    "l5_c_need": (l5.get("clear") or {}).get("need"), "l5_o_need": (l5.get("overcast") or {}).get("need"),
                    "l6_state": l6.get("state"), "l6_need": l6.get("need"), "relearn_hour": d.get("relearn_hour")})
        for f, v in (d.get("use") or {}).items():
            row["use_" + f] = v
    cols = list(row)
    path = learn_path(path, cols)
    new = not os.path.exists(path)
    with open(path, "a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, cols)
        if new:
            w.writeheader()
        w.writerow({k: ("" if row[k] is None else row[k]) for k in cols})


def tide(dt_utc):
    """Semi-diurnal pressure tide S2 (hPa): amplitude ~1.16 cos^3(lat), maxima ~10:00 and 22:00 local solar time."""
    a = 1.16 * math.cos(math.radians(LAT)) ** 3
    solar = dt_utc + timedelta(hours=LON / 15)  # mean solar time from the longitude
    solar_h = solar.hour + solar.minute / 60
    return a * math.cos(2 * math.pi * (solar_h - 10) / 12)


def zambretti(p, trend):
    """Simplified linear Zambretti (the common Arduino/ESPHome form): Z in 1-9 falling, 10-19 steady, 20-32 rising."""
    if trend == "falling":
        return 127 - 0.12 * p
    if trend == "rising":
        return 185 - 0.16 * p
    return 144 - 0.13 * p


TEXT = {1: "Settled fine", 2: "Fine weather", 3: "Fine, becoming less settled", 4: "Fairly fine, showery later",
        5: "Showery, becoming more unsettled", 6: "Unsettled, rain later", 7: "Rain at times, worse later",
        8: "Rain at times, becoming very unsettled", 9: "Very unsettled, rain", 10: "Settled fine", 11: "Fine weather",
        12: "Fine, possibly showers", 13: "Fairly fine, showers likely", 14: "Showery, bright intervals",
        15: "Changeable, some rain", 16: "Unsettled, rain at times", 17: "Rain at frequent intervals",
        18: "Very unsettled, rain", 19: "Stormy, much rain", 20: "Settled fine", 21: "Fine weather",
        22: "Becoming fine", 23: "Fairly fine, improving", 24: "Fairly fine, possibly showers early",
        25: "Showery early, improving", 26: "Changeable, mending", 27: "Rather unsettled, clearing later",
        28: "Unsettled, probably improving", 29: "Unsettled, short fine intervals", 30: "Very unsettled, finer at times",
        31: "Stormy, possibly improving", 32: "Stormy, much rain"}


def classify(d3):
    return "falling" if d3 <= -ZAMBRETTI_STEP else "rising" if d3 >= ZAMBRETTI_STEP else "steady"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bridge", default="fauxcloud.local")
    ap.add_argument("--station", default=None)
    ap.add_argument("--icao", default="", help="comma-separated airports whose METAR/TAF are logged for validation")
    ap.add_argument("--out", default=None, help="output folder (default: captures/forecast-trial/ in the repo)")
    a = ap.parse_args()
    global LAT, LON, OUT
    if a.out:
        OUT = a.out
    eff = get(a.bridge, "/api/bridge").get("effective") or {}
    parse_posix_tz(eff.get("tz") or "CET-1CEST,M3.5.0,M10.5.0/3")
    if eff.get("lat") is not None and eff.get("lon") is not None:
        LAT, LON = float(eff["lat"]), float(eff["lon"])
    live = get(a.bridge, "/api/live")
    st = live["stations"][0]
    sid = a.station or st["id"]
    rec = get(a.bridge, f"/api/recent?station={sid}")
    hr = get(a.bridge, f"/api/hourly?station={sid}")
    day = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    os.makedirs(OUT, exist_ok=True)
    raw = merge_recent(os.path.join(OUT, f"{day}-raw-recent.csv"), rec["fields"], rec["rows"])
    hours = write_hourly(os.path.join(OUT, f"{day}-hourly.csv"), hr)
    live_st = next((x for x in live["stations"] if x["id"] == sid), st)
    log_outlook(os.path.join(OUT, f"{day}-outlook.csv"), sid, live_st.get("outlook"))
    try:
        log_learn(os.path.join(OUT, f"{day}-learn.csv"), sid, get(a.bridge, f"/api/learn?station={sid}"))
    except Exception as e:  # an older firmware has no /api/learn
        print(f"(no /api/learn: {e})")
    if a.icao:
        log_aviation(os.path.join(OUT, f"{day}-aviation.txt"), a.icao)

    v = st["values"]
    now = utc(raw[-1]["t"] if isinstance(raw[-1]["t"], int) else int(raw[-1]["t"]))
    print(f"## Run at {loc(datetime.now(timezone.utc))} local (latest reading {loc(now)})\n")
    print("### Hourly pressure (means, local hour start) and 3 h tendency\n")
    print("| hour | p mean | d3 raw | tide(h) - tide(h-3) | d3 corrected | class raw / corrected |")
    print("|---|---|---|---|---|---|")
    ps = [(datetime.strptime(h["h"], "%Y-%m-%dT%H:%MZ").replace(tzinfo=timezone.utc), h["pressure_hpa"][1])
          for h in hours if h.get("pressure_hpa") and h["pressure_hpa"][1] is not None]
    by = dict(ps)
    for t, p in ps[-12:]:
        p3 = by.get(t - timedelta(hours=3))
        if p3 is None:
            print(f"| {loc(t)} | {p:.1f} | | | | |")
            continue
        d3 = p - p3
        dt = tide(t + timedelta(minutes=30)) - tide(t - timedelta(hours=3) + timedelta(minutes=30))
        print(f"| {loc(t)} | {p:.1f} | {d3:+.1f} | {dt:+.2f} | {d3 - dt:+.1f} | {classify(d3)} / {classify(d3 - dt)} |")

    p_now = v.get("pressure_hpa")
    t_end = int(raw[-1]["t"])  # a reading within 10 min of "3 h ago", else the hourly table decides (gaps in the raw log)
    last3 = [r for r in raw if t_end - 3 * 3600 - 600 <= int(r["t"]) <= t_end - 3 * 3600 + 600 and r["pressure_hpa"] != ""]
    print()
    if last3 and p_now is not None:
        r3 = last3[-1]
        t3 = utc(int(r3["t"]))
        d3 = p_now - float(r3["pressure_hpa"])
        dt = tide(now) - tide(t3)
        print(f"Instant 3 h tendency from raw readings: {p_now:.1f} ({loc(now)}) - {float(r3['pressure_hpa']):.1f} "
              f"({loc(t3)}) = {d3:+.1f} hPa; tide part {dt:+.2f}; corrected {d3 - dt:+.1f}\n")
        trend_raw, trend_cor = classify(d3), classify(d3 - dt)
    else:
        t, p = ps[-1]
        p3 = by.get(t - timedelta(hours=3))
        trend_raw = trend_cor = None
        if p3 is not None:
            d3 = p - p3
            dt = tide(t + timedelta(minutes=30)) - tide(t - timedelta(hours=3) + timedelta(minutes=30))
            trend_raw, trend_cor = classify(d3), classify(d3 - dt)
        print(f"Instant 3 h tendency: no raw reading 3 h back; the newest hourly row is used "
              f"({trend_raw} / {trend_cor}).\n")
    if p_now is not None:
        for name, tr in (("raw", trend_raw), ("tide-corrected", trend_cor)):
            if tr:
                z = zambretti(p_now, tr)
                zi = max(1, min(32, round(z)))
                print(f"Zambretti ({name}, {tr}): Z = {z:.1f} -> {zi} \"{TEXT[zi]}\"")
    t, d = v.get("temp_c"), v.get("dewpt_c")
    if t is not None and d is not None:
        print(f"\nTemperature - dew point: {t:.1f} - {d:.1f} = {t - d:.1f} C")
    win = [r for r in raw if int(r["t"]) >= int(raw[-1]["t"]) - 3600 and r["wind_kmh"] != "" and r["wind_dir"] != ""]
    if win:
        x = sum(float(r["wind_kmh"]) * math.sin(math.radians(float(r["wind_dir"]))) for r in win)
        y = sum(float(r["wind_kmh"]) * math.cos(math.radians(float(r["wind_dir"]))) for r in win)
        mean = sum(float(r["wind_kmh"]) for r in win) / len(win)
        print(f"Wind, last 60 min ({len(win)} readings): mean {mean:.1f} km/h, vector-mean direction "
              f"{(math.degrees(math.atan2(x, y)) + 360) % 360:.0f} deg, max gust "
              f"{max(float(r['gust_kmh']) for r in win if r['gust_kmh'] != ''):.1f} km/h")
    sky_table(raw, hours)


def sun_elevation(dt_utc):
    """Solar elevation (deg), NOAA low-precision formulas; good to ~0.5 deg."""
    doy = dt_utc.timetuple().tm_yday
    g = 2 * math.pi / 365 * (doy - 1 + (dt_utc.hour - 12 + dt_utc.minute / 60) / 24)
    decl = (0.006918 - 0.399912 * math.cos(g) + 0.070257 * math.sin(g) - 0.006758 * math.cos(2 * g)
            + 0.000907 * math.sin(2 * g) - 0.002697 * math.cos(3 * g) + 0.00148 * math.sin(3 * g))
    eqt = 229.18 * (0.000075 + 0.001868 * math.cos(g) - 0.032077 * math.sin(g) - 0.014615 * math.cos(2 * g)
                    - 0.040849 * math.sin(2 * g))
    tst = dt_utc.hour * 60 + dt_utc.minute + dt_utc.second / 60 + eqt + 4 * LON
    ha = math.radians(tst / 4 - 180)
    lat = math.radians(LAT)
    cz = math.sin(lat) * math.sin(decl) + math.cos(lat) * math.cos(decl) * math.cos(ha)
    return math.degrees(math.asin(max(-1.0, min(1.0, cz))))


def clear_sky(elev):
    """Haurwitz clear-sky global horizontal irradiance (W/m2)."""
    if elev <= 0:
        return 0.0
    cz = math.sin(math.radians(elev))
    return 1098 * cz * math.exp(-0.057 / cz)


def sky_class(kc, elev=90.0):
    if elev < 10:
        return "low sun (unreliable)"  # Haurwitz and a cheap sensor's cosine response both fail near the horizon
    # clearness vs a clear-sky model; the thresholds are a first guess to calibrate against notes of the sky
    return "sunny" if kc >= 0.7 else "partly sunny" if kc >= 0.4 else "dull" if kc >= 0.15 else "overcast"


def sky_table(raw, hours):
    print("\n### Sky from the solar sensor (measured / clear-sky, 15-min means, sun above 3 deg)\n")
    print("| local | sun elev | clear-sky W/m2 | measured W/m2 | ratio | class |")
    print("|---|---|---|---|---|---|")
    bins = {}
    for r in raw:
        if r["solar_wm2"] == "":
            continue
        t = utc(int(r["t"]))
        key = t.replace(minute=t.minute // 15 * 15, second=0)
        bins.setdefault(key, []).append(float(r["solar_wm2"]))
    for k in sorted(bins):
        mid = k + timedelta(minutes=7, seconds=30)
        e = sun_elevation(mid)
        if e < 3:
            continue
        cs, m = clear_sky(e), sum(bins[k]) / len(bins[k])
        print(f"| {loc(k)} | {e:.1f} | {cs:.0f} | {m:.0f} | {m / cs:.2f} | {sky_class(m / cs, e)} |")
    print("\n| local hour | sun elev (mid) | clear-sky | solar mean | ratio | class |")
    print("|---|---|---|---|---|---|")
    for h in hours:
        s = h.get("solar_wm2")
        if not s or s[1] is None:
            continue
        t = datetime.strptime(h["h"], "%Y-%m-%dT%H:%MZ").replace(tzinfo=timezone.utc) + timedelta(minutes=30)
        e = sun_elevation(t)
        if e < 3:
            continue
        cs = clear_sky(e)
        print(f"| {loc(t - timedelta(minutes=30))} | {e:.1f} | {cs:.0f} | {s[1]} | {s[1] / cs:.2f} | {sky_class(s[1] / cs, e)} |")


if __name__ == "__main__":
    main()
