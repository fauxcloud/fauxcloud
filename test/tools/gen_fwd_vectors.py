"""gen_fwd_vectors.py -- the forwarding host vectors, rendered by the oracle (tools/fwd_reference.py).

Reads test/host/fwd_records.json (invented records in the /api/live shape), applies the firmware's drops
before handing a record to fwd_reference.py (a hidden field, a warming field, a field whose age is unknown or above the
target's min_s; the 10-min mean wind goes with a hidden or warming wind speed), then renders with fwd_reference.py's own
variables() and render():
  * every preset (wu, windy, wow-be, pwsweather) over HTTPS for every record and every account, plus HTTP for one;
  * the WU-compatible server (the wu query on example.org, http and https);
  * every template of the fixture.
fwd_reference.py's variables are extended by the firmware's five extra names (wind_mean_kn, gust_max_kn, dateutc_compact,
date_local, time_local) exactly as the firmware computes them, the wc_* names are removed (the firmware has none), and
softwaretype is the fixture's (the firmware sends fauxcloud-<FC_VERSION>). Writes test/host/fwd_vectors.inc; the C++
result must be byte-equal for every case (test/host/test_fwd.inc).

fwd_reference.py imports `requests`; a stub module stands in (nothing is sent). Run isolated:
    python -I test/tools/gen_fwd_vectors.py           # regenerate
    python -I test/tools/gen_fwd_vectors.py --check   # CI: exit 1 if the committed .inc is stale
"""
import importlib.util
import json
import pathlib
import sys
import types
from datetime import datetime, timedelta, timezone

ROOT = pathlib.Path(__file__).resolve().parents[2]
ORACLE = ROOT / "tools" / "fwd_reference.py"
FAKE = ROOT / "tools" / "fwd_fake_endpoint.py"  # its canned answers are the parser vectors too
RECORDS = ROOT / "test" / "host" / "fwd_records.json"
OUT = ROOT / "test" / "host" / "fwd_vectors.inc"

# /api/live decimals per key (firmware/fauxcloud/src/fields.cpp): the fixture's texts must be in this form
DECIMALS = {"temp_c": 1, "dewpt_c": 1, "humidity": 0, "pressure_hpa": 1, "wind_kmh": 1, "gust_kmh": 1, "wind_dir": 0,
            "rain_mm": 2, "rain_daily_mm": 1, "uv": 1, "solar_wm2": 0, "wind_avg2m_kmh": 1, "wind_dir_avg2m": 0,
            "gust_10m_kmh": 1, "rain_rate_mmh": 1}
PRESETS = (("wu", "wu"), ("windy", "windy"), ("wowbe", "wow-be"), ("pwsweather", "pwsweather"))


def load_oracle():
    stub = types.ModuleType("requests")
    stub.RequestException = Exception
    sys.modules["requests"] = stub
    spec = importlib.util.spec_from_file_location("forward", ORACLE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_fake():
    spec = importlib.util.spec_from_file_location("fwd_fake_endpoint", FAKE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def c_bytes(b):
    return c_str(b.decode("latin-1")) if all(x < 0x80 for x in b) else None


def last_sunday(year, month):
    d = datetime(year, month + 1, 1, tzinfo=timezone.utc) - timedelta(days=1) if month < 12 else \
        datetime(year, 12, 31, tzinfo=timezone.utc)
    return d - timedelta(days=(d.weekday() + 1) % 7)


def eu_offset(dt):
    """CET-1CEST,M3.5.0,M10.5.0/3: summer time from the last Sunday of March 01:00 UTC to the last Sunday of October
    01:00 UTC (the only zone the fixture uses; the firmware evaluates the POSIX string itself)."""
    start = last_sunday(dt.year, 3).replace(hour=1)
    end = last_sunday(dt.year, 10).replace(hour=1)
    return 7200 if start <= dt < end else 3600


def record_for_oracle(r):
    hidden, warming = set(r.get("hidden", [])), set(r.get("warming", []))
    ages = r.get("age", {})
    rec = {}
    for k, text in r["values"].items():
        dec = DECIMALS[k]
        assert (len(text.split(".")[1]) if "." in text else 0) == dec, f"{r['name']}.{k}: {text} needs {dec} decimals"
        if k in hidden or k in warming or ages.get(k, 0) > r["min_s"]:
            continue
        rec[k] = json.loads(text)  # what fwd_reference.py --source live reads (int or float, as /api/live's JSON)
    if r.get("wind10") is not None and "wind_kmh" not in hidden and "wind_kmh" not in warming:
        rec["wind_avg10m_kmh"] = json.loads(r["wind10"])
    rec["_time"] = r["t"]
    return rec


def variables(fw, rec, software, acct):
    v = {k: x for k, x in fw.variables(rec).items() if not k.startswith("wc_")}
    g = rec.get
    first = lambda *ks: next((g(k) for k in ks if g(k) is not None), None)
    wind_mean = first("wind_avg10m_kmh", "wind_avg2m_kmh", "wind_kmh")
    gust_max = first("gust_10m_kmh", "gust_kmh")
    kn = lambda x: None if x is None else fw.fmt(x / 1.852, 1)
    dt = datetime.strptime(rec["_time"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    local = dt + timedelta(seconds=eu_offset(dt))
    v.update({"wind_mean_kn": kn(wind_mean), "gust_max_kn": kn(gust_max), "dateutc_compact": dt.strftime("%Y%m%d%H%M"),
              "date_local": local.strftime("%Y-%m-%d"), "time_local": local.strftime("%H:%M:%S"),
              "softwaretype": software, "id": acct["id"], "key": acct["key"]})
    return {k: x for k, x in v.items() if x is not None}


def render(fw, url, vals):
    try:
        _, out, _, _ = fw.render({"url": url}, vals)
        return out, ""
    except SystemExit as e:
        return "", "no_value" if "no value" in str(e) else str(e)


def c_str(s):
    if s is None:
        return "nullptr"
    out = []
    octal = False  # a digit right after an octal escape starts a new literal ("\012" "8": MSVC warning C4125)
    for b in s.encode("utf-8"):
        ch = chr(b)
        if octal and ch.isdigit():
            out.append('" "')
        octal = False
        if ch in '"\\?':
            out.append("\\" + ch)
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\n":
            out.append("\\n")
        elif 0x20 <= b < 0x7F:
            out.append(ch)
        else:
            out.append("\\%03o" % b)
            octal = True
    return '"' + "".join(out) + '"'


def build():
    fw = load_oracle()
    data = json.loads(RECORDS.read_text(encoding="utf-8"))
    software = data["software"]
    lines = ["// Generated by test/tools/gen_fwd_vectors.py from test/host/fwd_records.json with",
             "// tools/fwd_reference.py (the oracle). Do not edit; regenerate.",
             "// clang-format off",
             "static const char* const FWD_VEC_TZ = %s;" % c_str(data["tz"]),
             "static const char* const FWD_VEC_SOFTWARE = %s;" % c_str(software),
             "static const char* const FWD_VEC_ACCT[][2] = {%s};" % ", ".join(
                 "{%s, %s}" % (c_str(a["id"]), c_str(a["key"])) for a in data["accounts"]),
             "static const FwdVecRec FWD_VEC_RECS[] = {"]
    recs = data["records"]
    for r in recs:
        vals = ";".join(f"{k}={v}" for k, v in r["values"].items())
        ages = ";".join(f"{k}={v}" for k, v in r.get("age", {}).items())
        lines.append("  {%s, %s, %d, %s, %s, %s, %s, %s}," % (
            c_str(r["name"]), c_str(r["t"]), r["min_s"], c_str(vals), c_str(ages), c_str(",".join(r.get("warming", []))),
            c_str(",".join(r.get("hidden", []))), c_str(r.get("wind10"))))
    lines.append("};")
    lines.append("static const FwdVecCase FWD_VEC_CASES[] = {")
    n = 0

    def case(ri, typ, https, host, url, ai, tpl):
        nonlocal n
        acct = data["accounts"][ai]
        vals = variables(fw, record_for_oracle(recs[ri]), software, acct)
        out, err = render(fw, tpl, vals)
        if out and not https and out.startswith("https://"):
            out = "http://" + out[len("https://"):]
        lines.append("  {%d, %s, %s, %s, %s, %d, %s, %s}," % (ri, c_str(typ), "true" if https else "false",
                                                             c_str(host), c_str(url), ai, c_str(out), c_str(err)))
        n += 1

    for ri in range(len(recs)):
        for ai in range(len(data["accounts"])):
            for typ, name in PRESETS:
                case(ri, typ, True, None, None, ai, fw.PRESETS[name]["url"])
    for typ, name in PRESETS:
        case(0, typ, False, None, None, 0, fw.PRESETS[name]["url"])
    wu_q = fw.PRESETS["wu"]["url"].split("?", 1)[1]
    for https in (True, False):
        for host in ("example.org", "example.org:8080"):
            tpl = ("https://" if https else "http://") + host + "/weatherstation/updateweatherstation.php?" + wu_q
            case(2, "wu_compat", https, host, None, 1, tpl)
    for t in data["templates"]:
        for ri in (0, 3, 6, 9):
            for ai in range(len(data["accounts"])):
                case(ri, "template", t["url"].startswith("https://"), None, t["url"], ai, t["url"])
    lines.append("};")
    lines.append("static const FwdVecResp FWD_VEC_RESP[] = {")
    for name, typ, success, body, result, status, retry in load_fake().CASES:
        assert all(x < 0x80 for x in body), name
        lines.append("  {%s, %s, %s, %s, %d, %s, %d, %d}," % (c_str(name), c_str(typ), c_str(success),
                                                            c_str(body.decode("ascii")), len(body), c_str(result),
                                                            status, retry))
    lines.append("};")
    lines.append("// %d cases" % n)
    lines.append("// clang-format on")
    return "\n".join(lines) + "\n"


def main():
    text = build()
    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != text:
            print("gen_fwd_vectors: test/host/fwd_vectors.inc is stale; run python -I test/tools/gen_fwd_vectors.py")
            sys.exit(1)
        print("gen_fwd_vectors: up to date")
        return
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print("gen_fwd_vectors: wrote %s (%s)" % (OUT.relative_to(ROOT), text.strip().splitlines()[-2]))


if __name__ == "__main__":
    main()
