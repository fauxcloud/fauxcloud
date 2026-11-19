"""gen_tz_flags.py -- build the location-picker data packs from the vendored sources.

Inputs (firmware/fauxcloud/web/data/sources/, see SOURCES.md there):
    tzdata/zone.tab, tzdata/iso3166.tab, tzdata/version   IANA tzdata (public domain)
    posix_tz_db/zones.json                                 IANA name -> POSIX TZ rule (MIT)
    circle-flags/flags/<cc>.svg                            round flags (MIT)
Outputs (firmware/fauxcloud/web/data/, packed as hashed assets by gen_web_assets.py):
    tz.json         {"l": notice, "v": tzdata version, "r": [regions], "c": {CC: country}, "p": [POSIX rules],
                     "x": [IANA prefix per region], "z": {"cc": [...], "r": [region], "n": [IANA name without its prefix],
                     "note": [...], "la": [lat x 100], "lo": [lon x 100], "p": [posix index]}}
                    (columns: they compress better; the city is the IANA
                    name's last part with "_" -> " " and the prefix is x[region], both rebuilt in picker.js; lossless,
                    asserted below)
    flags-<r>.json  {"_l": notice, CC: "<svg ...>"} for the countries with a zone in region r (0..9)

Rules: zone.tab (not zone1970.tab) so each country lists its own zones; the comment is dropped when a country has
one zone; five zones newer than posix_tz_db borrow the rule of a zone with the same clock; flags lose their fixed
width/height and every number is rounded to an integer; circle-flags' git symlinks (a text file holding the target
name on a checkout without symlinks) are followed.

Usage (stdlib only, run isolated):
    python -I tools/gen_tz_flags.py           # regenerate
    python -I tools/gen_tz_flags.py --check   # CI: exit 1 if a committed pack is stale
    python -I tools/gen_tz_flags.py --plain DIR   # without the licence keys, into DIR (byte-compare with the mockup)
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DATA = ROOT / "firmware" / "fauxcloud" / "web" / "data"
SRC = DATA / "sources"

REGIONS = ["Europe", "Asia", "Africa", "Americas", "Australia", "Pacific", "Atlantic islands", "Indian Ocean",
           "Antarctica", "Arctic"]
PREFIX = {"Europe": 0, "Asia": 1, "Africa": 2, "America": 3, "Australia": 4, "Pacific": 5, "Atlantic": 6,
          "Indian": 7, "Antarctica": 8, "Arctic": 9}
# Zones newer than posix_tz_db: the rule of a zone with the same clock (checked against tzdata 2026f).
FALLBACK = {
    "Europe/Kyiv": "Europe/Kiev",
    "Pacific/Kanton": "Pacific/Enderbury",
    "Asia/Qostanay": "Asia/Almaty",
    "America/Ciudad_Juarez": "America/Denver",
    "America/Coyhaique": "America/Punta_Arenas",
}
TZ_NOTICE = ("Time zones: IANA tzdata %s (public domain). POSIX rules: nayarsystems/posix_tz_db (MIT). "
             "See licenses.txt.")
FLAG_NOTICE = "Flags: HatScripts/circle-flags, MIT, (c) 2026 HatScripts. See licenses.txt."
NUMBER = re.compile(r"-?\d*\.\d+")


def tab_rows(path):
    for line in path.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            yield line.split("\t")


def coord(text):
    """ISO 6709 +DDMM+DDDMM or +DDMMSS+DDDMMSS -> (lat, lon) in degrees, 2 decimals."""
    m = re.fullmatch(r"([+-]\d{2})(\d{2})(\d{2})?([+-]\d{3})(\d{2})(\d{2})?", text)
    if not m:
        raise SystemExit("bad coordinate %r" % text)

    def deg(d, mi, s):
        v = abs(int(d)) + int(mi) / 60 + (int(s) / 3600 if s else 0)
        return -v if d.startswith("-") else v
    return round(deg(m.group(1), m.group(2), m.group(3)), 2), round(deg(m.group(4), m.group(5), m.group(6)), 2)


def build_tz():
    version = (SRC / "tzdata" / "version").read_text(encoding="utf-8").strip()
    countries = {r[0]: r[1] for r in tab_rows(SRC / "tzdata" / "iso3166.tab")}
    posix = json.loads((SRC / "posix_tz_db" / "zones.json").read_text(encoding="utf-8"))
    zones = list(tab_rows(SRC / "tzdata" / "zone.tab"))
    per_country = {}
    for z in zones:
        per_country[z[0]] = per_country.get(z[0], 0) + 1
    rules, rows = [], []
    for z in zones:
        cc, xy, name = z[0], z[1], z[2]
        note = z[3] if len(z) > 3 and per_country[cc] > 1 else ""
        rule = posix.get(name) or posix.get(FALLBACK.get(name, ""))
        if not rule:
            raise SystemExit("no POSIX rule for %s" % name)
        if rule not in rules:
            rules.append(rule)
        lat, lon = coord(xy)
        rows.append([cc, PREFIX[name.split("/", 1)[0]], name, note, lat, lon, rules.index(rule)])
    used = sorted({r[0] for r in rows})
    # parallel columns; the round trip the page relies on is checked for every zone
    x = sorted(PREFIX, key=PREFIX.get)
    z = {"cc": [r[0] for r in rows], "r": [r[1] for r in rows], "n": [r[2].split("/", 1)[1] for r in rows],
         "note": [r[3] for r in rows], "la": [round(r[4] * 100) for r in rows], "lo": [round(r[5] * 100) for r in rows],
         "p": [r[6] for r in rows]}
    for i, r in enumerate(rows):
        if x[z["r"][i]] + "/" + z["n"][i] != r[2] or z["la"][i] / 100 != r[4] or z["lo"][i] / 100 != r[5]:
            raise SystemExit("tz.json round trip fails for %s" % r[2])
    return {"v": version, "r": REGIONS, "c": {cc: countries[cc] for cc in used}, "p": rules, "x": x, "z": z}


def flag_svg(cc):
    path = SRC / "circle-flags" / "flags" / (cc.lower() + ".svg")
    text = path.read_text(encoding="utf-8").strip()
    if not text.startswith("<"):  # a git symlink stored as a text file: follow it
        text = (path.parent / text).read_text(encoding="utf-8").strip()
    text = re.sub(r' (width|height)="[^"]*"', "", text, count=2)
    return NUMBER.sub(lambda m: str(round(float(m.group(0)))), text)


def build_flags(tz):
    packs = []
    for r in range(len(REGIONS)):
        ccs = sorted({cc for cc, ri in zip(tz["z"]["cc"], tz["z"]["r"]) if ri == r})
        packs.append({cc: flag_svg(cc) for cc in ccs})
    return packs


def dump(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n"


def outputs(plain=False):
    tz = build_tz()
    flags = build_flags(tz)
    files = {}
    head = {} if plain else {"l": TZ_NOTICE % tz["v"]}
    files["tz.json"] = dump({**head, **tz})
    for i, pack in enumerate(flags):
        head = {} if plain else {"_l": FLAG_NOTICE}
        files["flags-%d.json" % i] = dump({**head, **pack})
    return files


def main():
    args = sys.argv[1:]
    files = outputs(plain="--plain" in args)
    if "--plain" in args:
        out = pathlib.Path(args[args.index("--plain") + 1])
        out.mkdir(parents=True, exist_ok=True)
        for name, text in files.items():
            (out / name).write_text(text, encoding="utf-8", newline="\n")
        print("wrote %d plain packs to %s" % (len(files), out))
        return 0
    if "--check" in args:
        stale = [n for n, t in files.items()
                 if not (DATA / n).is_file() or (DATA / n).read_text(encoding="utf-8") != t]
        if stale:
            print("stale data packs: %s -- run python -I tools/gen_tz_flags.py and commit" % ", ".join(stale))
            return 1
        print("data packs are up to date")
        return 0
    for name, text in files.items():
        (DATA / name).write_text(text, encoding="utf-8", newline="\n")
    print("wrote %s" % ", ".join("%s (%d B)" % (n, len(t.encode("utf-8"))) for n, t in files.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
