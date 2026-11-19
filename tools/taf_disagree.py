"""Offline TAF-vs-outlook disagreement analysis (a research tool). Python 3.12 stdlib only.

Usage: python -I tools/taf_disagree.py [--dir captures/forecast-trial] [--from YYYY-MM-DDTHH:MMZ] [--out CSV]
                                       [--recent] [--include-stale] [--show-icao] [--cycle ICAO=3,ICAO=6]

Reads what the laptop trial (tools/forecast_trial.py) already collected: `<day>-aviation.txt` (raw METAR + TAF),
`<day>-hourly.csv` (station hours), `<day>-outlook.csv` (one live outlook row per run, used only as a cross-check).
Writes `<out>` (one row per airport-hour) and `<out minus .csv>-cases.csv` (runs of >= 2 disagreeing hours) and prints
the summary as Markdown.

What it does, per airport and per UTC hour:
  * the TAF in force = the newest captured issue <= the hour (and inside its validity), turned into hour flags per
    group: rain (RA DZ UP GR GS IC), snow (SN SG PL), ts, gust (G >= 40 km/h: 22 kt / 12 m/s), fog (FG or BR with
    vis < 1000 m); E = expected (base, FM, BECMG after its period), P = possible (TEMPO, PROBnn, INTER, BECMG
    transition); E wins over P. Hours are rounded outward (start floor, end ceil).
  * "covered" = the captured TAF is younger than the airport's issue cycle (3 or 6 h, inferred), so no newer issue
    can exist unseen (an amendment could); other hours are listed but not counted unless
    --include-stale.
  * the outlook state of the hour is NOT stored by the trial (one snapshot row per run), so it is rebuilt from the
    hourly CSV with the shipped outlook rules: rain state = a tip in the hour (the console's rate is the only source;
    --recent adds the hour after a tip, the later `recent_rain` word), "No rain." claim ("dry") = no tip and hourly
    mean T > 2 C, silence below that, gusts warning = hourly gust max >= 30 km/h. The pressure tendency is not
    rebuilt (needs the tide correction); the 5 real snapshot rows are compared against this proxy at the end.
  * the outlook has no word of its own for snow, thunder or fog, so those groups are "outlook-blind": only
    the TAF side and the observations are reported.
"""
import argparse
import csv
import glob
import os
import re
import sys
from collections import defaultdict
from datetime import datetime, timedelta, timezone

HOUR = timedelta(hours=1)
GROUPS = ("rain", "snow", "ts", "gust", "fog")
COMPARABLE = ("rain", "gust")  # groups the outlook can state; the others are outlook-blind
GUST_KMH_TAF = 40.0  # a significant TAF/METAR gust: 22 kt = 40.7, 12 m/s = 43.2 (21 kt, 11 m/s do not count)
GUST_KMH_OUTLOOK = 30.0  # the strip's own "Gusts to ..." warning
FALLING_HPA = -1.6  # the strip's 3-h tendency band (informational only: the tide correction is not rebuilt)
DRY_T = 2.0  # the strip says "No rain." only above this temperature (gauge-blind rule)
MIN_N = 10  # an hourly row with fewer readings is treated as missing
RAIN_PH = ("DZ", "RA", "UP", "GR", "GS", "IC")
SNOW_PH = ("SN", "SG", "PL")
PHEN = "DZ|RA|SN|SG|IC|PL|GR|GS|UP|BR|FG|FU|VA|DU|SA|HZ|PY|PO|SQ|FC|SS|DS"
WX_RE = re.compile(r"^([-+])?((?:MI|PR|BC|DR|BL|SH|TS|FZ)*)((?:%s)*)$" % PHEN)
WIND_RE = re.compile(r"^(?:\d{3}|VRB)(\d{2,3})(?:G(\d{2,3}))?(KT|MPS)$")
PERIOD_RE = re.compile(r"^(\d{2})(\d{2})/(\d{2})(\d{2})$")
FM_RE = re.compile(r"^FM(\d{2})(\d{2})(\d{2})$")
PROB_RE = re.compile(r"^PROB(\d{2})$")
VIS_SM_RE = re.compile(r"^(?:P|M)?(\d+)(?:/(\d+))?SM$")


# ---------------------------------------------------------------------------------------------------- time helpers
def _cands(day, hour, minute, ref):
    out = []
    for dm in (-1, 0, 1):
        m = ref.month + dm
        y = ref.year + (m - 1) // 12
        m = (m - 1) % 12 + 1
        try:
            out.append(datetime(y, m, day, 0, 0, tzinfo=timezone.utc))
        except ValueError:
            pass
    return out


def resolve(day, hour, minute, ref):
    """The datetime with this day-of-month / hour (24 = next midnight) / minute nearest to `ref` (month inferred)."""
    extra = timedelta(hours=hour, minutes=minute)
    best = min((c + extra for c in _cands(day, hour if hour < 24 else 0, minute, ref)),
               key=lambda d: abs(d - ref))
    return best


def parse_iso_hour(s):
    s = s.strip()
    for fmt in ("%Y-%m-%dT%H:%MZ", "%Y-%m-%dT%H:%M:%SZ"):
        try:
            return datetime.strptime(s, fmt).replace(tzinfo=timezone.utc)
        except ValueError:
            pass
    raise ValueError("bad time: " + s)


def floor_hour(d):
    return d.replace(minute=0, second=0, microsecond=0)


def iso(d):
    return d.strftime("%Y-%m-%dT%H:%MZ") if d else ""


# ------------------------------------------------------------------------------------------------ weather groups
def wx_flags(tok):
    """Present-weather token -> set of {'rain','snow','ts','fg','br'}; empty for anything else (VC..., clouds, ...)."""
    if tok.startswith(("VC", "+VC", "-VC")):
        return set()
    m = WX_RE.match(tok)
    if not m or not tok.strip("+-"):
        return set()
    _, desc, phen = m.groups()
    out = set()
    descs = [desc[i:i + 2] for i in range(0, len(desc), 2)]
    phens = [phen[i:i + 2] for i in range(0, len(phen), 2)]
    if "TS" in descs:
        out.add("ts")
    for p in phens:
        if p in RAIN_PH:
            out.add("rain")
        elif p in SNOW_PH:
            out.add("snow")
        elif p == "FG":
            out.add("fg")
        elif p == "BR":
            out.add("br")
    return out


def vis_of(tok):
    """Visibility token in metres, or None. 4 digits (m), CAVOK handled by the caller, US statute miles."""
    if re.fullmatch(r"\d{4}", tok):
        return int(tok)
    m = VIS_SM_RE.match(tok)
    if m:
        v = int(m.group(1)) if not m.group(2) else int(m.group(1)) / int(m.group(2))
        return int(v * 1609)
    return None


def gust_kmh(tok):
    m = WIND_RE.match(tok)
    if not m or not m.group(2):
        return None
    return int(m.group(2)) * (1.852 if m.group(3) == "KT" else 3.6)


class State:
    __slots__ = ("wx", "vis", "gust")

    def __init__(self, wx=frozenset(), vis=None, gust=None):
        self.wx, self.vis, self.gust = frozenset(wx), vis, gust

    def flags(self):
        f = {k for k in ("rain", "snow", "ts") if k in self.wx}
        if self.gust is not None and self.gust >= GUST_KMH_TAF:
            f.add("gust")
        if ("fg" in self.wx or "br" in self.wx) and self.vis is not None and self.vis < 1000:
            f.add("fog")
        return f


def flags_of_tokens(tokens):
    """(State of one change group's own tokens, wind stated?, NSW?, CAVOK?) from its tokens."""
    wx, vis, gust, wind, nsw, cavok = set(), None, None, False, False, False
    for t in tokens:
        if WIND_RE.match(t):
            wind, gust = True, gust_kmh(t)
        elif t == "CAVOK":
            cavok, vis = True, 9999
        elif t == "NSW":
            nsw = True
        elif vis_of(t) is not None:
            vis = vis_of(t)
        else:
            wx |= wx_flags(t)
    return State(wx, vis, gust), wind, nsw, cavok


# ------------------------------------------------------------------------------------------------------- the TAF
class Taf:
    def __init__(self):
        self.icao = ""
        self.amd = False
        self.issue = None
        self.valid_from = self.valid_to = None
        self.status = "ok"  # ok / nil / cnl / bad
        self.timeline = []  # [(start, State)] expected
        self.overlays = []  # [(p0, p1, State)] possible
        self.seen = None  # first fetch stamp that carried it
        self.text = ""

    def hour_flags(self, h):
        """{group: 'E'|'P'|''} for the hour starting at h (rounded outward)."""
        out = {g: "" for g in GROUPS}
        if self.status != "ok" or not (self.valid_from < h + HOUR and self.valid_to > h):
            return out
        e = None
        for start, st in self.timeline:
            if start < h + HOUR:
                e = st
        ef = e.flags() if e else set()
        pf = set()
        for p0, p1, st in self.overlays:
            if p0 < h + HOUR and p1 > h and p0 < self.valid_to and p1 > self.valid_from:
                pf |= st.flags()
        for g in GROUPS:
            out[g] = "E" if g in ef else "P" if g in pf else ""
        return out


def parse_taf(text, ref):
    """Parses one TAF report (AWC raw: continuation lines indented, PROBnn possibly alone on a line)."""
    t = Taf()
    t.text = text
    toks = text.replace("=", " ").split()
    i = 0
    if not toks or toks[0] != "TAF":
        t.status = "bad"
        return t
    i = 1
    while i < len(toks) and toks[i] in ("AMD", "COR", "RTD", "CCA", "CCB", "CCC"):
        t.amd = t.amd or toks[i] == "AMD"
        i += 1
    if i + 1 >= len(toks) or not re.fullmatch(r"\d{6}Z", toks[i + 1]):
        t.status = "bad"
        return t
    t.icao = toks[i]
    d = toks[i + 1]
    t.issue = resolve(int(d[0:2]), int(d[2:4]), int(d[4:6]), ref)
    i += 2
    if i < len(toks) and toks[i] in ("NIL", "CNL"):
        t.status = toks[i].lower()
        return t
    m = PERIOD_RE.match(toks[i]) if i < len(toks) else None
    if not m:
        t.status = "bad"
        return t
    t.valid_from = resolve(int(m.group(1)), int(m.group(2)), 0, t.issue)
    t.valid_to = resolve(int(m.group(3)), int(m.group(4)), 0, t.issue)
    i += 1
    if t.valid_to <= t.valid_from:
        t.status = "bad"
        return t
    if i < len(toks) and toks[i] == "CNL":  # `TAF AMD CCCC DDHHMMZ ddhh/ddhh CNL`
        t.status = "cnl"
        return t
    groups = []  # (kind, p0, p1, tokens)
    cur = ("base", t.valid_from, t.valid_to, [])
    prob = None
    while i < len(toks):
        k = toks[i]
        fm = FM_RE.match(k)
        if k == "RMK":
            break
        if fm:
            groups.append(cur)
            cur = ("FM", resolve(int(fm.group(1)), int(fm.group(2)), int(fm.group(3)), t.issue), t.valid_to, [])
        elif PROB_RE.match(k):
            prob = k
        elif k in ("BECMG", "TEMPO", "INTER") or (prob and PERIOD_RE.match(k)):
            if k in ("BECMG", "TEMPO", "INTER"):
                i += 1
                if i >= len(toks):
                    break
                k2 = toks[i]
                kind = "TEMPO" if k == "INTER" else k
            else:  # PROBnn ddhh/ddhh (no TEMPO)
                k2, kind = k, "PROB"
            pm = PERIOD_RE.match(k2)
            if not pm:
                prob = None
                i += 1
                continue
            groups.append(cur)
            p0 = resolve(int(pm.group(1)), int(pm.group(2)), 0, t.issue)
            p1 = resolve(int(pm.group(3)), int(pm.group(4)), 0, t.issue)
            cur = ("PROB" if prob else kind, p0, p1, [])
            prob = None
        elif re.match(r"^(TX|TN)M?\d+/\d{4}Z$", k):
            pass
        else:
            cur[3].append(k)
        i += 1
    groups.append(cur)
    # the expected timeline and the possible overlays
    kind, p0, p1, toks0 = groups[0]
    st, _, _, _ = flags_of_tokens(toks0)
    t.timeline.append((t.valid_from, st))
    state = st
    for kind, p0, p1, tk in groups[1:]:
        own, wind, nsw, cavok = flags_of_tokens(tk)
        if kind == "FM":
            state = State(own.wx, own.vis, own.gust)
            t.timeline.append((p0, state))
        elif kind == "BECMG":
            t.overlays.append((p0, p1, State(own.wx, own.vis if own.vis is not None else state.vis, own.gust)))
            wx = own.wx if (own.wx or nsw or cavok) else state.wx
            state = State(wx, own.vis if own.vis is not None else state.vis, own.gust if wind else state.gust)
            t.timeline.append((p1, state))
        else:  # TEMPO / PROB / PROB TEMPO
            vis = own.vis if own.vis is not None else state.vis
            t.overlays.append((p0, p1, State(own.wx, vis, own.gust)))
    return t


# ----------------------------------------------------------------------------------------------------- the METAR
class Metar:
    def __init__(self, icao, time, flags, gust, vis, text):
        self.icao, self.time, self.flags, self.gust, self.vis, self.text = icao, time, flags, gust, vis, text


def parse_metar(line, ref):
    toks = line.split()
    if len(toks) < 3 or toks[0] not in ("METAR", "SPECI"):
        return None
    i = 1
    while i < len(toks) and toks[i] in ("COR", "AUTO"):
        i += 1
    if i + 1 >= len(toks) or not re.fullmatch(r"\d{6}Z", toks[i + 1]):
        return None
    icao, d = toks[i], toks[i + 1]
    time = resolve(int(d[0:2]), int(d[2:4]), int(d[4:6]), ref)
    wx, vis, gust = set(), None, None
    for t in toks[i + 2:]:
        if t in ("TEMPO", "BECMG", "NOSIG", "RMK"):
            break
        if WIND_RE.match(t):
            gust = gust_kmh(t)
        elif t == "CAVOK":
            vis = 9999
        elif vis is None and vis_of(t) is not None:
            vis = vis_of(t)
        else:
            wx |= wx_flags(t)
    f = {k for k in ("rain", "snow", "ts") if k in wx}
    if gust is not None and gust >= GUST_KMH_TAF:
        f.add("gust")
    if ("fg" in wx or "br" in wx) and vis is not None and vis < 1000:
        f.add("fog")
    return Metar(icao, time, f, gust, vis, line)


# ----------------------------------------------------------------------------------------------------- read data
def read_aviation(paths):
    """(tafs {(icao, issue): Taf}, metars [Metar]) from the `<day>-aviation.txt` files, oldest first."""
    tafs, metars = {}, []

    def flush(lines, stamp):
        if not lines:
            return
        taf = parse_taf(" ".join(lines), stamp)
        if taf.status != "bad" and taf.issue:
            old = tafs.get((taf.icao, taf.issue))
            taf.seen = old.seen if old else stamp
            tafs[(taf.icao, taf.issue)] = taf  # a later text of the same issue (COR) wins

    for path in sorted(paths):
        stamp, cur = None, []
        with open(path, encoding="utf-8") as f:
            for raw in f:
                line = raw.rstrip("\r\n")
                if line.startswith("# fetched "):
                    flush(cur, stamp)
                    cur = []
                    stamp = parse_iso_hour(line.split()[2])
                elif line.startswith("#") or not line.strip():
                    continue
                elif line.startswith(("METAR", "SPECI")):
                    flush(cur, stamp)
                    cur = []
                    m = parse_metar(line.strip(), stamp or datetime.now(timezone.utc))
                    if m:
                        metars.append(m)
                elif line.startswith("TAF"):
                    flush(cur, stamp)
                    cur = [line.strip()]
                elif line[0].isspace() and cur:
                    cur.append(line.strip())
        flush(cur, stamp)
    # METAR dedup by (icao, time)
    seen, uniq = set(), []
    for m in metars:
        if (m.icao, m.time) not in seen:
            seen.add((m.icao, m.time))
            uniq.append(m)
    return tafs, uniq


def read_hourly(paths):
    """{hour datetime: {col: str}} from the `<day>-hourly.csv` files; a later file overrides (its open hour is complete)."""
    rows = {}
    for path in sorted(paths):
        with open(path, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                rows[parse_iso_hour(r["hour_utc"])] = r
    return rows


def num(r, k):
    try:
        v = r.get(k, "")
        return float(v) if v not in ("", None) else None
    except ValueError:
        return None


def infer_cycles(tafs, override=None):
    """Issue cycle per airport in hours: 3 when two regular (non-AMD) issues 3 h apart were seen, else 6."""
    by = defaultdict(list)
    for (icao, issue), t in tafs.items():
        if not t.amd:
            by[icao].append(issue)
    out = {}
    for icao, v in by.items():
        v.sort()
        out[icao] = 3 if any(abs((b - a).total_seconds() / 3600 - 3) < 0.1 for a, b in zip(v, v[1:])) else 6
    out.update(override or {})
    return out


# ------------------------------------------------------------------------------------------------ outlook proxy
def outlook_proxy(hourly, h, recent=False):
    """The outlook's state for hour h rebuilt from the hourly CSV (None when the station has no usable row)."""
    r = hourly.get(h)
    if r is None or (num(r, "n") or 0) < MIN_N:
        return None
    rain = (num(r, "rain_mm") or 0) > 0
    if recent and not rain:
        prev = hourly.get(h - HOUR)
        rain = bool(prev and (num(prev, "n") or 0) >= MIN_N and (num(prev, "rain_mm") or 0) > 0)
    gust = (num(r, "gust_max") or 0) >= GUST_KMH_OUTLOOK
    p, p3 = num(r, "p_mean"), hourly.get(h - 3 * HOUR)
    p3 = num(p3, "p_mean") if p3 else None
    falling = None if p is None or p3 is None else (p - p3) <= FALLING_HPA
    tm = num(r, "t_mean")
    # "No rain." is claimed only while the gauge can see (T > 2 C); below that the strip is silent (DRY_T)
    state = "rain" if rain else "dry" if tm is not None and tm > DRY_T else "silent"
    return {"state": state, "rain": rain, "gust": gust, "falling": falling}


# -------------------------------------------------------------------------------------------- the per-hour table
def build_rows(tafs, metars, hourly, start, end, cycles, recent=False, stale=False, labels=None):
    by_icao = defaultdict(list)
    for (icao, issue), t in tafs.items():
        if t.status == "ok":
            by_icao[icao].append(t)
    for v in by_icao.values():
        v.sort(key=lambda t: t.issue)
    met = defaultdict(list)
    for m in metars:
        met[(m.icao, floor_hour(m.time))].append(m)
    rows = []
    h = start
    while h <= end:
        station = hourly.get(h)
        out = outlook_proxy(hourly, h, recent)
        for icao in sorted(by_icao):
            cand = [t for t in by_icao[icao] if t.issue <= h]
            taf = cand[-1] if cand else None
            if taf and h >= taf.valid_to:
                taf = None
            age = (h - taf.issue).total_seconds() / 3600 if taf else None
            covered = bool(taf and age < cycles.get(icao, 6))
            fl = taf.hour_flags(h) if taf and (covered or stale) else {g: "" for g in GROUPS}
            ms = met.get((icao, h), [])
            row = {"hour_utc": iso(h), "airport": (labels or {}).get(icao, icao),
                   "taf_issue": iso(taf.issue) if taf else "", "taf_age_h": "" if age is None else "%.1f" % age,
                   "covered": int(covered), "use": int(covered or (stale and taf is not None)),
                   "st_n": "" if station is None else station.get("n", ""),
                   "st_rain_mm": "" if station is None else station.get("rain_mm", ""),
                   "st_gust_max": "" if station is None else station.get("gust_max", ""),
                   "st_rh": "" if station is None else station.get("rh_mean", ""),
                   "out_state": out["state"] if out else "", "out_gust": ("" if not out else int(out["gust"])),
                   "metar_n": len(ms)}
            for g in GROUPS:
                row["taf_" + g] = fl[g]
                row["metar_" + g] = (int(any(g in m.flags for m in ms)) if ms else "")
            for g in GROUPS:
                row["class_" + g] = classify(row, g, out)
            rows.append(row)
        h += HOUR
    return rows


def classify(row, g, out):
    if not row["use"]:
        return ""
    taf = row["taf_" + g] != ""
    if g not in COMPARABLE:
        return "taf_blind" if taf else "quiet"
    if out is None:
        return ""
    o = out["rain"] if g == "rain" else out["gust"]
    return ("agree_active" if taf and o else "taf_only" if taf else "outlook_only" if o else "agree_quiet")


# ------------------------------------------------------------------------------------------------------ cases
def station_flag(row, g):
    if g == "rain":
        return (num(row, "st_rain_mm") or 0) > 0
    if g == "gust":
        return (num(row, "st_gust_max") or 0) >= GUST_KMH_OUTLOOK
    if g == "fog":
        return (num(row, "st_rh") or 0) >= 92  # weak: the RH sensor caps at ~93 %
    return False


def find_cases(rows):
    """Runs of >= 2 consecutive hours of the same disagreement class, per airport and group."""
    cases = []
    idx = defaultdict(list)
    for r in rows:
        idx[r["airport"]].append(r)
    for ap, rs in idx.items():
        rs.sort(key=lambda r: r["hour_utc"])
        for g in GROUPS:
            run = []
            for r in rs + [None]:
                c = r["class_" + g] if r else ""
                ok = c in ("taf_only", "outlook_only", "taf_blind")
                if run and (not ok or c != run[0]["class_" + g] or
                            parse_iso_hour(r["hour_utc"]) - parse_iso_hour(run[-1]["hour_utc"]) != HOUR):
                    if len(run) >= 2:
                        cases.append(make_case(ap, g, run, rs))
                    run = []
                if ok:
                    run.append(r)
    cases.sort(key=lambda c: (c["start"], c["airport"], c["group"]))
    return cases


def make_case(ap, g, run, rs):
    cls = run[0]["class_" + g]
    kinds = {r["taf_" + g] for r in run}
    kind = "" if cls == "outlook_only" else ("E" if kinds == {"E"} else "P" if kinds == {"P"} else "E+P")
    inside = {r["hour_utc"] for r in run}
    lo, hi = parse_iso_hour(run[0]["hour_utc"]) - 2 * HOUR, parse_iso_hour(run[-1]["hour_utc"]) + 2 * HOUR
    near = [r for r in rs if r["hour_utc"] not in inside and lo <= parse_iso_hour(r["hour_utc"]) <= hi]
    st_in = sum(station_flag(r, g) for r in run)
    claim = sum(1 for r in run if g == "rain" and r["out_state"] == "dry")
    st_near = sum(station_flag(r, g) for r in near)
    m_n = sum(1 for r in run if r["metar_n"])
    m_hit = sum(1 for r in run if r["metar_" + g] == 1)
    if cls == "outlook_only":
        verdict = ("airport METAR agrees" if m_hit else "airport METAR quiet" if m_n else "no METAR")
    else:
        verdict = (("RH at its cap (weak)" if g == "fog" else "station confirms") if st_in else
                   "METAR confirms" if m_hit else
                   "station tip within 2 h" if st_near else "METAR quiet" if m_n else "unconfirmed (no obs)")
    return {"airport": ap, "group": g, "class": cls, "start": run[0]["hour_utc"], "hours": len(run), "taf_kind": kind,
            "station_hours": st_in, "station_near_2h": st_near, "metar_hours": m_n, "metar_hits": m_hit,
            "no_rain_claim_hours": claim, "rain_mm": round(sum(num(r, "st_rain_mm") or 0 for r in run), 2),
            "gust_max": max([num(r, "st_gust_max") or 0 for r in run] or [0]), "verdict": verdict}


# ----------------------------------------------------------------------------------------------------- summary
def tally(rows, airport=None):
    """{group: counts}: the class counts, TAF-only split by E / P, and what the observations say about each side."""
    out = {}
    for g in GROUPS:
        c = defaultdict(int)
        for r in rows:
            if airport and r["airport"] != airport:
                continue
            k = r["class_" + g]
            if not k:
                continue
            c[k] += 1
            if k in ("taf_only", "taf_blind"):
                c[k + "_" + r["taf_" + g]] += 1
                c["taf_metar_obs"] += 1 if r["metar_n"] else 0
                c["taf_metar_hit"] += 1 if r["metar_" + g] == 1 else 0
                c["taf_station_hit"] += 1 if station_flag(r, g) else 0
                c["taf_over_claim"] += 1 if g == "rain" and r["out_state"] == "dry" else 0
            if k == "outlook_only":
                c["out_metar_obs"] += 1 if r["metar_n"] else 0
                c["out_metar_hit"] += 1 if r["metar_" + g] == 1 else 0
        out[g] = c
    return out


def markdown(rows, cases, tafs, metars, hourly, cycles, args, labels):
    L = []
    aps = sorted({r["airport"] for r in rows})
    start, end = rows[0]["hour_utc"], rows[-1]["hour_utc"]
    L.append("Window %s .. %s; %d airport-hours; cycle (h): %s" % (
        start, end, len(rows), ", ".join("%s %d" % (labels.get(k, k), v) for k, v in sorted(cycles.items()))))
    L.append("TAFs captured: %d; METARs: %d; station hours with data: %d" % (
        len([t for t in tafs.values() if t.status == "ok"]), len(metars),
        sum(1 for h, r in hourly.items() if start <= iso(h) <= end and (num(r, "n") or 0) >= MIN_N)))
    L.append("")
    L.append("### Coverage (hours)\n")
    L.append("| airport | window hours | TAF in force | covered (younger than a cycle) | covered + station data | hours with a METAR |")
    L.append("|---|---|---|---|---|---|")
    for ap in aps:
        rs = [r for r in rows if r["airport"] == ap]
        L.append("| %s | %d | %d | %d | %d | %d |" % (
            ap, len(rs), sum(1 for r in rs if r["taf_issue"] and r["taf_age_h"] != ""), sum(r["covered"] for r in rs),
            sum(1 for r in rs if r["covered"] and r["out_state"]), sum(1 for r in rs if r["metar_n"])))
    L.append("")
    L.append("### Hours by group (counted: covered TAF hours; rain and gust also need station data)\n")
    L.append("| airport | group | agree active | agree quiet | TAF only (E / P) | outlook only | TAF-only hours: METAR shows it / with a METAR | outlook-only hours: METAR shows it / with a METAR | TAF-only hours where the strip claimed \"No rain.\" |")
    L.append("|---|---|---|---|---|---|---|---|---|")
    for ap in aps + ["all"]:
        t = tally(rows, None if ap == "all" else ap)
        for g in GROUPS:
            c = t[g]
            k = "taf_only" if g in COMPARABLE else "taf_blind"
            tafonly = "%d (%d / %d)" % (c[k], c[k + "_E"], c[k + "_P"])
            if g in COMPARABLE:
                L.append("| %s | %s | %d | %d | %s | %d | %d / %d | %d / %d | %s |" % (
                    ap, g, c["agree_active"], c["agree_quiet"], tafonly, c["outlook_only"], c["taf_metar_hit"],
                    c["taf_metar_obs"], c["out_metar_hit"], c["out_metar_obs"],
                    c["taf_over_claim"] if g == "rain" else "-"))
            else:
                L.append("| %s | %s | blind | %d | %s | blind | %d / %d | blind | - |" % (
                    ap, g, c["quiet"], tafonly, c["taf_metar_hit"], c["taf_metar_obs"]))
    L.append("")
    L.append("### Cases (>= 2 consecutive disagreeing hours)\n")
    if not cases:
        L.append("none")
    else:
        L.append("| start (UTC) | airport | group | class | hours | TAF kind | station hrs | station tip +-2 h | METAR hrs / shows it | hours the strip said \"No rain.\" | mm | gust max | verdict |")
        L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for c in cases:
            L.append("| %s | %s | %s | %s | %d | %s | %d | %d | %d / %d | %d | %s | %s | %s |" % (
                c["start"], c["airport"], c["group"], c["class"], c["hours"], c["taf_kind"], c["station_hours"],
                c["station_near_2h"], c["metar_hours"], c["metar_hits"], c["no_rain_claim_hours"], c["rain_mm"],
                c["gust_max"], c["verdict"]))
    return "\n".join(L)


def skill(rows):
    """TAF rain vs a station tip (= the outlook's rain state), pooled and per airport: POD / FAR on covered hours."""
    L = ["| airport | hits | misses | false alarms | correct quiet | POD | FAR |", "|---|---|---|---|---|---|---|"]
    for ap in sorted({r["airport"] for r in rows}) + ["all"]:
        c = tally(rows, None if ap == "all" else ap)["rain"]
        a, b, m, d = c["agree_active"], c["taf_only"], c["outlook_only"], c["agree_quiet"]
        L.append("| %s | %d | %d | %d | %d | %s | %s |" % (
            ap, a, m, b, d, "%.2f" % (a / (a + m)) if a + m else "-", "%.2f" % (b / (a + b)) if a + b else "-"))
    return "\n".join(L)


def check_snapshots(dirpath, hourly, recent=False):
    """The real `<day>-outlook.csv` rows against the proxy at their hour (rain / "No rain." claim / silence)."""
    L = ["| run (UTC) | board state | proxy | |", "|---|---|---|---|"]
    for p in sorted(glob.glob(os.path.join(dirpath, "*-outlook*.csv"))):
        with open(p, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                try:
                    h = floor_hour(parse_iso_hour(r["t"]))
                except (ValueError, KeyError):
                    continue
                o = outlook_proxy(hourly, h, recent)
                b = r.get("state") or ""
                bs = "rain" if b.startswith("rain") else "dry" if b.startswith("dry") else "silent" if not b else "other"
                if not o:
                    L.append("| %s | %s | no station row | |" % (r["t"], b or "(silent)"))
                else:
                    L.append("| %s | %s | %s | %s |" % (r["t"], b or "(silent)", o["state"],
                                                        "n/a" if bs == "other" else "ok" if bs == o["state"] else "DIFF"))
    return "\n".join(L)


def simulate_coverage(days, cycle_h, fetch_hours_utc, phase_h=5.5):
    """Share of hours whose in-force TAF would be captured when the nominal issues are every `cycle_h` hours from
    05:30 UTC and a fetch at each of `fetch_hours_utc` (hours of day, may be fractional) captures the newest issue."""
    def newest(t):  # newest nominal issue <= t (t in hours from day 0 00:00)
        k = int((t - phase_h) // cycle_h)
        return phase_h + k * cycle_h
    captured = set()
    for d in range(days):
        for fh in fetch_hours_utc:
            captured.add(newest(d * 24 + fh))
    n = covered = 0
    for h in range(24, days * 24):  # skip day 0 (no fetch before it)
        n += 1
        i = newest(h)
        covered += i in captured and h - i < cycle_h
    return covered / n


# ------------------------------------------------------------------------------------------------------- main
def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(here, "..", "captures", "forecast-trial"))
    ap.add_argument("--out", default=None, help="hour CSV (default <dir>/taf-disagree.csv)")
    ap.add_argument("--from", dest="start", default=None,
                    help="first UTC hour, as YYYY-MM-DDTHH:MMZ (skips the hours before the station was in place)")
    ap.add_argument("--recent", action="store_true", help="outlook rain state also in the hour after a tip (recent_rain)")
    ap.add_argument("--include-stale", action="store_true", help="count hours whose captured TAF is older than a cycle")
    ap.add_argument("--show-icao", action="store_true", help="airport codes instead of A/B in the CSV and the report")
    ap.add_argument("--cycle", default="", help="issue cycle override, e.g. KJFK=3,EGLL=6")
    a = ap.parse_args(argv)
    d = a.dir
    tafs, metars = read_aviation(glob.glob(os.path.join(d, "*-aviation.txt")))
    hourly = read_hourly(glob.glob(os.path.join(d, "*-hourly.csv")))
    if not tafs or not hourly:
        print("no data in", d)
        return 1
    override = {k: int(v) for k, v in (x.split("=") for x in a.cycle.split(",") if x)}
    cycles = infer_cycles(tafs, override)
    order = []  # airports in order of first appearance in the aviation logs -> A, B, ...
    for t in sorted(tafs.values(), key=lambda t: t.seen or t.issue):
        if t.icao not in order:
            order.append(t.icao)
    labels = {} if a.show_icao else {k: chr(ord("A") + i) for i, k in enumerate(order)}
    hrs = sorted(hourly)
    start = parse_iso_hour(a.start) if a.start else hrs[0]
    rows = build_rows(tafs, metars, hourly, max(start, hrs[0]), hrs[-1], cycles, a.recent, a.include_stale, labels)
    cases = find_cases(rows)
    out = a.out or os.path.join(d, "taf-disagree.csv")
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    with open(out[:-4] + "-cases.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, list(cases[0]) if cases else ["none"])
        w.writeheader()
        w.writerows(cases)
    print(markdown(rows, cases, tafs, metars, hourly, {k: v for k, v in cycles.items()}, a, labels))
    print("\n### TAF rain vs a station tip (covered hours)\n")
    print(skill(rows))
    print("\n### Proxy vs the board's own outlook rows\n")
    print(check_snapshots(d, hourly, a.recent))
    print("\n### Coverage if the fetches ran at ... (nominal issues at :30, simulated over 10 days)\n")
    print("| fetches (UTC) | 6-h issuer | 3-h issuer |\n|---|---|---|")
    for name, fh in (("once a day (07:00)", [7]), ("4 a day (00:00 06:40 12:00 18:00)", [0, 6.67, 12, 18]),
                     ("every 3 h from 06:00", [0, 3, 6, 9, 12, 15, 18, 21])):
        print("| %s | %.0f %% | %.0f %% |" % (name, 100 * simulate_coverage(10, 6, fh), 100 * simulate_coverage(10, 3, fh)))
    print("\nwrote", out, "and", out[:-4] + "-cases.csv")
    return 0


if __name__ == "__main__":
    sys.exit(main())
