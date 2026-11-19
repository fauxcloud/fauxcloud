"""The tables the pages copy from the firmware stay equal to it: promotion types, calibration keys and
ranges, filter rule names, the core fields and the dashboard's unit classes. Plain text parsing of both sides, no
build. Run: python -I -m unittest discover -s test/tools"""
import math
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "firmware" / "fauxcloud" / "src"
WEB = ROOT / "firmware" / "fauxcloud" / "web"


def read(p):
    return p.read_text(encoding="utf-8")


def cstr(s):
    s = s.strip()
    return "" if s == "nullptr" else s.strip('"')


def num(s):
    s = s.strip().rstrip("f")
    return math.nan if s == "NAN" else float(s)


class TableSync(unittest.TestCase):
    def test_promotion_types(self):
        body = read(SRC / "config.cpp")
        start = body.index("PROMO_TYPES[(int)PromoType::COUNT] = {")
        block = body[start:body.index("// clang-format on", start)]
        cpp = []
        for row in re.findall(r"^\s*\{(\"[^\n]*)\},\s*$", block, re.M):
            f = [x.strip() for x in re.findall(r'"[^"]*"|[^,]+', row)]
            cpp.append(dict(id=cstr(f[0]), label=cstr(f[1]), unit=cstr(f[2]),
                            conv={"None": "none"}.get(f[3].split("::")[1], f[3].split("::")[1]), dc=cstr(f[4]),
                            sc=cstr(f[5]), group=cstr(f[6]), icon=cstr(f[7]), span=num(f[8]), max=num(f[9]),
                            dec=int(num(f[10]))))
        js = []
        page = read(WEB / "admin-station.js")
        # the labels are WA keys since the i18n extraction (or a literal name, PM2.5): read the text from the table
        wa = dict(re.findall(r"\b(\w+): '([^']*)'", page[page.index("const WA = FC.w("):page.index("});", page.index("const WA = FC.w("))]))
        for row in re.findall(r"^\s*\{ id: [^\n]*\}", page[page.index("const TYPES = ["):page.index("const guessType")], re.M):
            def g(k, d=None):
                m = re.search(r"\b%s: ('([^']*)'|WA\.(\w+)|[-\d.]+)" % k, row)
                return d if not m else (m.group(2) if m.group(2) is not None else wa[m.group(3)] if m.group(3) else float(m.group(1)))
            js.append(dict(id=g("id"), label=g("label"), unit=g("unit"), conv=g("conv"), dc=g("dc"), sc=g("sc"),
                           group=g("group"), icon=g("icon"), span=g("span"), max=g("max", math.nan), dec=int(g("dec"))))
        self.assertEqual(len(cpp), len(js))
        self.assertGreater(len(cpp), 10)
        for a, b in zip(cpp, js):
            for k in a:
                if k == "max" and math.isnan(a[k]):
                    self.assertTrue(math.isnan(b[k]), (a["id"], k))
                else:
                    self.assertEqual(a[k], b[k], (a["id"], k))

    def test_calibration(self):
        cpp = re.findall(r'\{"(\w+)", &StationConfig::\w+, ([-\d.f]+), ([-\d.f]+), ([-\d.f]+)\}', read(SRC / "config.cpp"))
        js = re.findall(r"\['(\w+)', (?:'[^']*'|WA\.\w+), '[^']*', ([-\d.]+), ([-\d.]+), ([-\d.]+)\]", read(WEB / "admin-station.js"))
        self.assertEqual(len(cpp), 6)
        self.assertEqual([(k, num(a), num(b), num(c)) for k, a, b, c in cpp],
                         [(k, num(a), num(b), num(c)) for k, a, b, c in js])

    def test_rules(self):
        m = re.search(r"RULE_NAMES\[R_COUNT\] = \{([^}]*)\}", read(SRC / "filter.cpp"))
        cpp = re.findall(r'"(\w+)"', m.group(1))
        page = read(WEB / "admin-station.js")
        js = re.findall(r"\['(\w+)', (?:'|WA\.)", page[page.index("const RULES = ["):page.index("const CALIB = [")])
        self.assertEqual(cpp, js)

    def fields(self):
        rows = re.findall(r'^\s*\{"(\w+)",\s*"(\w+)",\s*"([^"]*)",\s*"([^"]*)",\s*Conv::(\w+)', read(SRC / "fields.cpp"), re.M)
        return [dict(param=p, key=k, label=l, unit=u, conv=c) for p, k, l, u, c in rows]

    def test_core_fields(self):
        cpp = self.fields()[:11]
        page = read(WEB / "admin-station.js")
        js = re.findall(r"\['(\w+)', (?:'[^']*'|WA\.\w+), '([^']*)', '[\w-]+', '\w+'\]", page[page.index("const FIELDS = ["):page.index("const RULES = [")])
        self.assertEqual([(f["key"], f["unit"]) for f in cpp], js)

    def test_details_labels(self):
        """Every field key and every computed key the firmware can send has a Details label in admin-station.js
        (LBL), so a new field never reaches Details as a raw key."""
        render = read(SRC / "web_render.cpp")
        block = render[render.index('",\\"derived\\":{"'):]
        block = block[:block.index('o.raw("}")')]
        derived = re.findall(r'\\"(\w+)\\":%', block)  # any conversion
        self.assertEqual(derived, ["wind_avg10m_kmh"])
        m = re.search(r"const LBL = \{(.*?)\};", read(WEB / "admin-station.js"), re.S)
        lbl = set(re.findall(r"(\w+): (?:'|WA\.)", m.group(1)))
        want = {f["key"] for f in self.fields()} | set(derived)
        self.assertGreater(len(want), 25)
        self.assertEqual(sorted(want - lbl), [])

    def test_rel_hint_threshold(self):
        """The REL sentence's threshold (outlook-why.js REL_THR, hPa) is the firmware's REL_HINT_DHPA (rel_check.h, 0.1 hPa):
        a retune of one side alone would make the sheet say "within" while the pressure notice says "off"."""
        fw = int(re.search(r"constexpr int16_t REL_HINT_DHPA = (\d+);", read(SRC / "rel_check.h")).group(1))
        page = float(re.search(r"const REL_THR = ([\d.]+);", read(WEB / "outlook-why.js")).group(1))
        self.assertEqual(round(page * 10), fw)

    def test_dashboard_unit_classes(self):
        cls = {"FtoC": "t", "MphToKmh": "w", "InToMm": "r", "InHgToHpa": "p"}
        want = {f["key"]: cls[f["conv"]] for f in self.fields() if f["conv"] in cls}
        m = re.search(r"const CONV = \{([^}]*)\};", read(WEB / "dash.js"))
        got = dict(re.findall(r"(\w+): '(\w)'", m.group(1)))
        self.assertEqual(got, want)


class OutlookSync(unittest.TestCase):
    """The firmware's HA text table (src/forecast.cpp) says what the page's web/outlook.js says,
    code for code (`%s` <-> {gust} / {left_h}), and both know the same codes."""

    @staticmethod
    def table(src, name):
        m = re.search(r"const char\* const " + name + r"\[[A-Z_]+\] = \{(.*?)\};", src, re.S)
        return re.findall(r'"([^"]*)"', m.group(1)) if m else []

    def test_codes_and_texts(self):
        cpp, js = read(SRC / "forecast.cpp"), read(WEB / "outlook.js")
        web = dict(re.findall(r"\b([a-z_0-9]+): '([^']*)'", js[js.index("const W = {"):js.index("};", js.index("const W = {"))]))
        pairs = [("OUT_STATE", "OUT_STATE_TEXT"), ("OUT_NEXT", "OUT_NEXT_TEXT"), ("OUT_WARN", "OUT_WARN_TEXT"),
                 ("OUT_QUAL", "OUT_QUAL_TEXT")]
        n = 0
        for codes, texts in pairs:
            c, t = self.table(cpp, codes), self.table(cpp, texts)
            self.assertEqual(len(c), len(t), codes)
            for code, text in zip(c, t):
                self.assertIn(code, web, "outlook.js has no phrase for " + code)
                want = text.replace("%s h", "{left_h} h") if code == "collecting" else                     text.replace("%s", "{range}") if code == "min_range" else text.replace("%s", "{gust}")
                self.assertEqual(web[code], want, code)
                n += 1
        gate = self.table(cpp, "OUT_GATE_TEXT")
        self.assertEqual(web["time"], gate[0])
        self.assertEqual(n, 36)  # incl. 3 T3 states, 3 T3 warnings, 3 day words, min_range, fog_possible, sleet_snow


    def test_icao_country_flag_packs(self):
        """The firmware's ICAO-prefix table (airport.cpp ICAO_CC): each country's pack is tz.json's region of the
        country's first zone (picker.js CC_R), the flag is in that pack, and no prefix shadows a longer one"""
        import json
        body = read(SRC / "airport.cpp")
        block = body[body.index("ICAO_CC[] ="):body.index("// clang-format on", body.index("ICAO_CC[] ="))]
        rows = [(p.strip(), cc, pk) for p, cc, pk in re.findall(r'"([A-Z]{1,3} *)" "([A-Z]{2})(\d)"', block)]
        self.assertTrue(all(len(p) == 3 for p in re.findall(r'"([A-Z ]{1,3})" "[A-Z]{2}\d"', block)))  # 6 bytes a row
        self.assertEqual(len("".join(re.findall(r'"([^"]*)"', block))), 6 * len(rows))  # no row the regex skipped
        self.assertGreater(len(rows), 100)
        tz = json.loads(read(WEB / "data" / "tz.json"))
        ccr = {}
        for cc, r in zip(tz["z"]["cc"], tz["z"]["r"]):
            ccr.setdefault(cc, r)
        packs = {}
        for pfx, cc, pk in rows:
            self.assertEqual(ccr.get(cc), int(pk), pfx + ":" + cc)
            if pk not in packs:
                packs[pk] = json.loads(read(WEB / "data" / ("flags-%s.json" % pk)))
            self.assertIn(cc, packs[pk], pfx + ":" + cc)
        lens = [len(p) for p, _, _ in rows]
        self.assertEqual(lens, sorted(lens, reverse=True))  # the lookup takes the first match: longest prefixes first
        self.assertEqual(len({p for p, _, _ in rows}), len(rows))
        self.assertNotIn("const PFX", read(WEB / "admin-metar.js"))  # one table: the page reads `cc`

if __name__ == "__main__":
    unittest.main()
