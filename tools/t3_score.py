"""Airport-word scoring: how often the airport's report would show in the outlook. Python 3.12 stdlib only.

Usage: python -I tools/t3_score.py [--dir captures/forecast-trial] [--main ICAO] [--from YYYY-MM-DDTHH:MMZ] [--show-icao]

For the airport words the truth is the report itself, so this scores information and agreement, not POD/FAR. Reads
what the laptop trial (tools/forecast_trial.py) collected: `<day>-aviation.txt` (raw METARs, `&hours=26`) and
`<day>-hourly.csv` (the station's hours). Per UTC hour of the station:
  * the main airport's METAR in force at the hour's end = its newest report <= the end and <= 75 min old
    (METAR_CURRENT_S); an hour without one is not eligible;
  * the report's phenomena by the firmware's mapping (metar.cpp wxGroup + metar_store.cpp nearFromStore, ported here;
    vectors shared with the host test in test/tools/t3_vectors.json): snow > rain > drizzle, thunder, fog > mist;
  * our own state from the hourly row (taf_disagree.outlook_proxy, with recent rain): an airport state word shows only
    over "No rain." or the gauge-blind silence (local first); an airport warning shows unless our gusts warning outranks it
    (fog / mist; thunder outranks gusts). Approximation: the proxy has no icy / hoar / ground-frost flags,
    which also outrank fog_near / mist_near (and icy outranks thunder_near), so "shown" is slightly high on frosty
    nights.
Prints per word: eligible hours, shown, the share (> 60 % of eligible hours in a month = no information: the
word is demoted; `mist_near` is the suspect), how often local first suppressed it, and the disagreement runs (the
airport reports rain or drizzle for >= 2 h while our gauge is dry above 2 °C) for a person to check. Airports are
printed as placeholders unless --show-icao (the captures are your own data). The distance rule (<= 60 km) is the
user's choice of airport here: the trial airport is assumed nearby.
"""
import argparse
import glob
import importlib.util
import json
import os
import sys
from collections import Counter
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("taf_disagree", os.path.join(HERE, "taf_disagree.py"))
td = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(td)

CURRENT_S = 4500  # METAR_CURRENT_S (metar_store.h)
HOUR = timedelta(hours=1)
DESC = ("MI", "BC", "PR", "DR", "BL", "SH", "TS", "FZ")
PHEN = ("DZ", "RA", "SN", "SG", "IC", "PL", "GR", "GS", "UP", "BR", "FG", "FU", "VA", "DU", "SA", "HZ", "PY", "PO",
        "SQ", "FC", "SS", "DS")
STATE_WORDS = ("snow", "rain", "drizzle")
WARN_WORDS = ("thunder", "fog", "mist")


def wx_bits(tok):
    """The firmware's wxGroup(): the bits of one present-weather group, None = not a weather group; RE / VC are read
    and dropped (recent weather, in the vicinity)."""
    p, ignore = tok, False
    if p.startswith(("RE", "VC")):
        ignore, p = True, p[2:]
    elif p[:1] in "-+":
        p = p[1:]
    if not p or len(p) % 2:
        return None
    bits = set()
    for i in range(0, len(p), 2):
        c = p[i:i + 2]
        if c not in DESC and c not in PHEN:
            return None
        bits |= {"TS": {"thunder"}, "FZ": {"freezing"}, "FG": {"fog"}, "BR": {"mist"}, "DZ": {"drizzle", "precip"},
                 "RA": {"rain", "precip"}, "SN": {"snow", "precip"}, "SG": {"snow", "precip"}}.get(c, set())
        if c in ("IC", "PL", "GR", "GS", "UP"):
            bits.add("precip")
    return set() if ignore else bits


def near_of(groups):
    """nearFromStore()'s mapping of a report's weather groups (a string of groups or a token list) to the airport words."""
    bits = set()
    for t in groups.split() if isinstance(groups, str) else groups:
        b = wx_bits(t)
        if b:
            bits |= b
    out = []
    for w in STATE_WORDS:
        if w in bits:
            out.append(w)
            break
    if "thunder" in bits:
        out.append("thunder")
    if "fog" in bits:
        out.append("fog")
    elif "mist" in bits:
        out.append("mist")
    return out


def report_groups(text):
    """The weather groups of a raw METAR line: the tokens after the time, before a trend or the remarks."""
    toks, out = text.split(), []
    for t in toks[3:]:
        if t in ("TEMPO", "BECMG", "NOSIG", "RMK"):
            break
        out.append(t)
    return out


def in_force(metars, end):
    """The newest report at or before `end`, <= CURRENT_S old; None = not eligible."""
    best = None
    for m in metars:
        if m.time <= end and (best is None or m.time > best.time):
            best = m
    return best if best and (end - best.time).total_seconds() <= CURRENT_S else None


def score(metars, hourly, start=None):
    """Counters per word: eligible, shown, suppressed; and the disagreement runs [(first hour, hours)]."""
    tally = {w: Counter() for w in STATE_WORDS + WARN_WORDS}
    eligible, disagree, run = 0, [], []
    for h in sorted(hourly):
        if start and h < start:
            continue
        m = in_force(metars, h + HOUR)
        ours = td.outlook_proxy(hourly, h, recent=True)
        if m is None or ours is None:
            run = _close(run, disagree)
            continue
        eligible += 1
        words = near_of(report_groups(m.text))
        for w in words:
            tally[w]["reported"] += 1
            if w in STATE_WORDS:
                tally[w]["shown" if ours["state"] in ("dry", "silent") else "suppressed"] += 1
            else:
                tally[w]["suppressed" if ours["gust"] and w != "thunder" else "shown"] += 1
        wet = any(w in ("rain", "drizzle") for w in words)
        if wet and ours["state"] == "dry":
            run.append(h)
        else:
            run = _close(run, disagree)
    _close(run, disagree)
    return eligible, tally, disagree


def _close(run, out):
    if len(run) >= 2:
        out.append((run[0], len(run)))
    return []


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dir", default="captures/forecast-trial")
    ap.add_argument("--main", help="the main airport's ICAO (default: the one with the most reports)")
    ap.add_argument("--from", dest="start", help="first UTC hour, as YYYY-MM-DDTHH:MMZ")
    ap.add_argument("--show-icao", action="store_true",
                    help="print the airport code (off by default: the captures are your own data)")
    a = ap.parse_args(argv)
    _, metars = td.read_aviation(glob.glob(os.path.join(a.dir, "*-aviation.txt")))
    hourly = td.read_hourly(glob.glob(os.path.join(a.dir, "*-hourly.csv")))
    if not metars or not hourly:
        print("no aviation or hourly files in", a.dir)
        return 1
    main_icao = a.main or Counter(m.icao for m in metars).most_common(1)[0][0]
    start = td.parse_iso_hour(a.start) if a.start else None
    label = main_icao if a.show_icao else "AIRPORT-1"
    for auto in (None, False, True):
        ms = [m for m in metars if m.icao == main_icao and (auto is None or (" AUTO " in m.text) == auto)]
        if not ms:
            continue
        eligible, tally, disagree = score(ms, hourly, start)
        print("\n### %s%s: %d eligible hours" % (label, "" if auto is None else " (AUTO)" if auto else " (manned)", eligible))
        print("| word | reported | shown (frost / icy warnings not modelled) | share of eligible | suppressed (local first) |\n|---|---|---|---|---|")
        for w in STATE_WORDS + WARN_WORDS:
            c = tally[w]
            share = 100.0 * c["shown"] / eligible if eligible else 0
            print("| %s_near | %d | %d | %.0f %%%s | %d |" % ((("wx_" + w) if w in STATE_WORDS else w), c["reported"],
                  c["shown"], share, " (> 60 %: demote)" if share > 60 else "", c["suppressed"]))
        if auto is None:
            for h, n in disagree:
                print("disagreement: airport rain/drizzle while our gauge says No rain, from %s for %d h" % (td.iso(h), n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
