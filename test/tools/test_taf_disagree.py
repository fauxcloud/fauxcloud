"""tools/taf_disagree.py: the TAF parser (FM / BECMG / TEMPO / PROBnn, AWC raw quirks, units), the METAR reader, the
station-outlook proxy, the per-hour classes, the case runs and the CLI end to end. Foreign airports (KJFK, EGLL, UUEE)
and synthetic ZZZZ lines only, no network, no board."""
import contextlib
import csv
import importlib.util
import io
import os
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("taf_disagree", os.path.join(HERE, "..", "..", "tools", "taf_disagree.py"))
td = importlib.util.module_from_spec(spec)
spec.loader.exec_module(td)


def utc(y, mo, d, h=0, mi=0):
    return datetime(y, mo, d, h, mi, tzinfo=timezone.utc)


def flags(taf, y, mo, d, h):
    return taf.hour_flags(utc(y, mo, d, h))


KJFK = ("TAF KJFK 132035Z 1321/1424 35014G20KT P6SM VCSH BKN020 BKN040 \n  FM140000 34011KT P6SM SCT018 BKN040 \n"
        "  TEMPO 1400/1404 BKN018 \n  FM141400 31015G25KT P6SM BKN050")
UUEE = ("TAF UUEE 132016Z 1321/1421 15008G13MPS 6000 BKN010 TX05/1321Z TN02/1406Z \n"
        "  TEMPO 1321/1324 15011G17MPS 3000 -SHRA BKN005 BKN016CB \n  BECMG 1401/1403 29006MPS \n"
        "  BECMG 1407/1409 24008G15MPS BKN016")
EGLL = ("TAF EGLL 141100Z 1412/1518 25012KT 9999 SCT025 \n  TEMPO 1412/1418 4000 RA BKN012 \n"
        "  PROB30 \n  TEMPO 1500/1506 0800 FG BKN001 \n  PROB30 1502/1506 4000 BR \n  BECMG 1508/1511 SHRA")


class Vocabulary(unittest.TestCase):
    def test_resolve_month_roll(self):
        ref = utc(2026, 12, 31, 22)
        self.assertEqual(td.resolve(1, 3, 0, ref), utc(2027, 1, 1, 3))
        self.assertEqual(td.resolve(31, 18, 0, ref), utc(2026, 12, 31, 18))
        self.assertEqual(td.resolve(1, 24, 0, ref), utc(2027, 1, 2, 0))  # hour 24 = next midnight
        self.assertEqual(td.resolve(30, 6, 0, utc(2026, 11, 1, 2)), utc(2026, 10, 30, 6))  # day number ahead of ref = last month

    def test_gust_threshold_units(self):
        self.assertIsNone(td.gust_kmh("27010KT"))
        self.assertLess(td.gust_kmh("27010G21KT"), 40)  # 38.9
        self.assertGreaterEqual(td.gust_kmh("27010G22KT"), 40)  # 40.7
        self.assertLess(td.gust_kmh("27008G11MPS"), 40)  # 39.6
        self.assertGreaterEqual(td.gust_kmh("27008G12MPS"), 40)  # 43.2

    def test_wx_flags(self):
        self.assertEqual(td.wx_flags("-SHRA"), {"rain"})
        self.assertEqual(td.wx_flags("RASN"), {"rain", "snow"})
        self.assertEqual(td.wx_flags("+TSRA"), {"ts", "rain"})
        self.assertEqual(td.wx_flags("TS"), {"ts"})
        self.assertEqual(td.wx_flags("FZDZ"), {"rain"})
        self.assertEqual(td.wx_flags("BCFG"), {"fg"})
        self.assertEqual(td.wx_flags("BR"), {"br"})
        for t in ("VCSH", "VCFG", "BKN020", "SCT018CB", "9999", "RMK", "NSW", "CAVOK"):
            self.assertEqual(td.wx_flags(t), set(), t)

    def test_vis(self):
        self.assertEqual(td.vis_of("0800"), 800)
        self.assertEqual(td.vis_of("9999"), 9999)
        self.assertGreater(td.vis_of("P6SM"), 9000)
        self.assertLess(td.vis_of("1/4SM"), 1000)
        self.assertIsNone(td.vis_of("BKN020"))


class TafParse(unittest.TestCase):
    def test_kjfk_gust_only_after_fm(self):
        t = td.parse_taf(KJFK, utc(2026, 11, 13, 20, 40))
        self.assertEqual((t.icao, t.status, t.amd), ("KJFK", "ok", False))
        self.assertEqual(t.issue, utc(2026, 11, 13, 20, 35))
        self.assertEqual((t.valid_from, t.valid_to), (utc(2026, 11, 13, 21), utc(2026, 11, 15, 0)))
        self.assertEqual(flags(t, 2026, 11, 13, 21), {g: "" for g in td.GROUPS})  # G20KT = 37 km/h, VCSH ignored
        self.assertEqual(flags(t, 2026, 11, 14, 2)["gust"], "")  # TEMPO with clouds only
        self.assertEqual(flags(t, 2026, 11, 14, 13)["gust"], "")  # FM 14:00 starts at 14:00, not inside hour 13
        self.assertEqual(flags(t, 2026, 11, 14, 14)["gust"], "E")  # G25KT = 46 km/h
        self.assertEqual(flags(t, 2026, 11, 14, 23)["gust"], "E")
        self.assertEqual(flags(t, 2026, 11, 15, 0), {g: "" for g in td.GROUPS})  # past the validity

    def test_uucee_mps_becmg_and_tx_tn(self):
        t = td.parse_taf(UUEE, utc(2026, 11, 13, 21))
        self.assertEqual(t.status, "ok")
        self.assertEqual(flags(t, 2026, 11, 13, 21)["gust"], "E")  # G13MPS = 46.8 km/h
        self.assertEqual(flags(t, 2026, 11, 13, 21)["rain"], "P")  # TEMPO -SHRA
        self.assertEqual(flags(t, 2026, 11, 13, 23)["rain"], "P")
        self.assertEqual(flags(t, 2026, 11, 14, 0)["rain"], "")
        self.assertEqual(flags(t, 2026, 11, 14, 2)["gust"], "E")  # BECMG 01/03 (no gust) ends it at 03:00 ...
        self.assertEqual(flags(t, 2026, 11, 14, 3)["gust"], "")  # ... not before
        self.assertEqual(flags(t, 2026, 11, 14, 7)["gust"], "P")  # BECMG 07/09 G15MPS: possible during the transition
        self.assertEqual(flags(t, 2026, 11, 14, 8)["gust"], "P")
        self.assertEqual(flags(t, 2026, 11, 14, 9)["gust"], "E")  # expected after its end

    def test_egll_prob_alone_on_a_line_fog_and_becmg_transition(self):
        t = td.parse_taf(EGLL, utc(2026, 11, 14, 11, 10))
        self.assertEqual(flags(t, 2026, 11, 14, 12)["rain"], "P")
        self.assertEqual(flags(t, 2026, 11, 14, 17)["rain"], "P")
        self.assertEqual(flags(t, 2026, 11, 14, 18)["rain"], "")
        self.assertEqual(flags(t, 2026, 11, 15, 0)["fog"], "P")  # PROB30 / TEMPO 0800 FG
        self.assertEqual(flags(t, 2026, 11, 15, 5)["fog"], "P")
        self.assertEqual(flags(t, 2026, 11, 15, 6)["fog"], "")
        self.assertEqual(flags(t, 2026, 11, 15, 8)["rain"], "P")  # BECMG SHRA transition
        self.assertEqual(flags(t, 2026, 11, 15, 11)["rain"], "E")  # expected after the period

    def test_br_needs_vis_below_1000(self):
        a = td.parse_taf("TAF ZZZZ 141200Z 1412/1512 27010KT 0800 BR BKN001", utc(2026, 11, 14, 12, 30))
        b = td.parse_taf("TAF ZZZZ 141200Z 1412/1512 27010KT 4000 BR BKN001", utc(2026, 11, 14, 12, 30))
        self.assertEqual(flags(a, 2026, 11, 14, 13)["fog"], "E")
        self.assertEqual(flags(b, 2026, 11, 14, 13)["fog"], "")

    def test_fm_replaces_everything(self):
        t = td.parse_taf("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 -RA BKN015 FM141700 27008KT 9999 SCT030 "
                         "FM150300 18012G42KT 3000 +TSRA BKN010CB", utc(2026, 11, 14, 12, 30))
        self.assertEqual(flags(t, 2026, 11, 14, 16)["rain"], "E")
        self.assertEqual(flags(t, 2026, 11, 14, 17)["rain"], "")
        f = flags(t, 2026, 11, 15, 3)
        self.assertEqual((f["rain"], f["ts"], f["gust"]), ("E", "E", "E"))

    def test_becmg_keeps_weather_until_nsw(self):
        t = td.parse_taf("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 -RA BKN015 BECMG 1414/1415 30012KT "
                         "BECMG 1418/1419 30012KT 9999 NSW SCT030", utc(2026, 11, 14, 12, 30))
        self.assertEqual(flags(t, 2026, 11, 14, 16)["rain"], "E")
        self.assertEqual(flags(t, 2026, 11, 14, 18)["rain"], "E")
        self.assertEqual(flags(t, 2026, 11, 14, 19)["rain"], "")

    def test_tempo_beyond_validity_is_clipped_and_amd_rmk_ignored(self):
        t = td.parse_taf("TAF AMD ZZZZ 140600Z 1406/1415 27010KT 9999 SCT030 TEMPO 1412/1424 4000 -RA BKN015 "
                         "RMK NXT FCST BY 141800Z", utc(2026, 11, 14, 7))
        self.assertTrue(t.amd)
        self.assertEqual(flags(t, 2026, 11, 14, 14)["rain"], "P")
        self.assertEqual(flags(t, 2026, 11, 14, 15)["rain"], "")

    def test_nil_cnl_and_garbage(self):
        self.assertEqual(td.parse_taf("TAF KJFK 132035Z NIL", utc(2026, 11, 13, 21)).status, "nil")
        self.assertEqual(td.parse_taf("TAF AMD KJFK 132035Z 1321/1424 CNL", utc(2026, 11, 13, 21)).status, "cnl")
        self.assertEqual(td.parse_taf("METAR KJFK 132051Z 35016KT", utc(2026, 11, 13, 21)).status, "bad")
        self.assertEqual(flags(td.parse_taf("TAF KJFK 132035Z NIL", utc(2026, 11, 13, 21)), 2026, 11, 13, 22),
                         {g: "" for g in td.GROUPS})

    def test_month_end(self):
        t = td.parse_taf("TAF KDEN 311651Z 3118/0124 34006KT P6SM SCT220 FM010100 VRB06KT P6SM BKN220 "
                         "PROB30 0102/0106 -SHRA BKN060", utc(2026, 10, 31, 17))
        self.assertEqual(t.valid_to, utc(2026, 11, 2, 0))
        self.assertEqual(flags(t, 2026, 11, 1, 2)["rain"], "P")
        self.assertEqual(flags(t, 2026, 11, 1, 6)["rain"], "")


class MetarParse(unittest.TestCase):
    ref = utc(2026, 11, 13, 21)

    def test_rain_and_no_gust(self):
        m = td.parse_metar("METAR KJFK 132051Z 35016KT 10SM -RA SCT013 BKN047 OVC070 05/03 A2966 RMK AO2 SLP043", self.ref)
        self.assertEqual((m.icao, m.time), ("KJFK", utc(2026, 11, 13, 20, 51)))
        self.assertEqual(m.flags, {"rain"})

    def test_gust_fog_and_trend_part_ignored(self):
        g = td.parse_metar("METAR KJFK 132151Z 31020G28KT 10SM FEW040 05/03 A2970", self.ref)
        self.assertEqual(g.flags, {"gust"})
        f = td.parse_metar("METAR EGLL 132120Z 24006KT 0800 R27L/1200 FG VV001 04/04 Q1020 NOSIG", self.ref)
        self.assertEqual(f.flags, {"fog"})
        t = td.parse_metar("METAR EGLL 132120Z 24006KT 9999 SCT020 04/04 Q1020 TEMPO 3000 RA", self.ref)
        self.assertEqual(t.flags, set())  # the trend is a forecast, not an observation
        b = td.parse_metar("METAR EGLL 132120Z 24006KT 3000 BR SCT020 04/04 Q1020", self.ref)
        self.assertEqual(b.flags, set())  # BR above 1000 m is not fog

    def test_not_a_metar(self):
        self.assertIsNone(td.parse_metar("TAF KJFK 132035Z NIL", self.ref))
        self.assertIsNone(td.parse_metar("METAR", self.ref))


class Files(unittest.TestCase):
    def test_read_aviation_blocks_dedupe_and_failed_fetch(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "2026-11-13-aviation.txt")
            with open(p, "w", encoding="utf-8") as f:
                f.write("# fetched 2026-11-13T21:00Z (metar)\nMETAR KJFK 132051Z 35016KT 10SM -RA SCT013 05/03 A2966\n"
                        "# fetched 2026-11-13T21:00Z (taf)\n" + KJFK + "\nTAF EGLL 132050Z 1321/1424 25012KT 9999 SCT025\n"
                        "  TEMPO 1400/1404 4000 RA\n"
                        "# 2026-11-13T22:00Z taf fetch failed: HTTP Error 502: Bad Gateway\n"
                        "# fetched 2026-11-13T23:00Z (metar)\nMETAR KJFK 132051Z 35016KT 10SM -RA SCT013 05/03 A2966\n")
            tafs, metars = td.read_aviation([p])
        self.assertEqual(sorted(k[0] for k in tafs), ["EGLL", "KJFK"])
        self.assertEqual(len(metars), 1)  # the same report twice counts once
        self.assertEqual(tafs[("EGLL", utc(2026, 11, 13, 20, 50))].seen, utc(2026, 11, 13, 21))
        self.assertEqual(flags(tafs[("EGLL", utc(2026, 11, 13, 20, 50))], 2026, 11, 14, 1)["rain"], "P")

    def test_infer_cycles(self):
        def mk(icao, issues, amd=False):
            out = {}
            for i in issues:
                t = td.Taf()
                t.icao, t.issue, t.amd = icao, i, amd
                out[(icao, i)] = t
            return out
        tafs = mk("KJFK", [utc(2026, 11, 13, 5, 30), utc(2026, 11, 13, 8, 30), utc(2026, 11, 13, 11, 30)])
        tafs.update(mk("EGLL", [utc(2026, 11, 13, 5, 30), utc(2026, 11, 13, 11, 30)]))
        tafs.update(mk("UUEE", [utc(2026, 11, 13, 9, 0), utc(2026, 11, 13, 12, 0)], amd=True))  # AMDs do not count
        c = td.infer_cycles(tafs)
        self.assertEqual((c["KJFK"], c["EGLL"]), (3, 6))
        self.assertNotIn("UUEE", c)
        self.assertEqual(td.infer_cycles(tafs, {"EGLL": 3})["EGLL"], 3)


def hourly_rows(start, specs):
    """specs: [(rain_mm, t_mean, gust_max)] one per hour from `start`."""
    out = {}
    for i, (rain, t, gust) in enumerate(specs):
        h = start + timedelta(hours=i)
        out[h] = {"hour_utc": td.iso(h), "n": "55", "p_mean": "1010.0", "t_mean": str(t), "rh_mean": "90", "wind_mean": "1",
                  "gust_max": str(gust), "rain_mm": str(rain)}
    return out


class Proxy(unittest.TestCase):
    def test_states(self):
        s = utc(2026, 11, 14, 0)
        hr = hourly_rows(s, [(0.0, 4.0, 0), (0.25, 4.0, 0), (0.0, 4.0, 0), (0.0, 1.8, 35)])
        self.assertEqual(td.outlook_proxy(hr, s)["state"], "dry")
        self.assertEqual(td.outlook_proxy(hr, s + timedelta(hours=1))["state"], "rain")
        self.assertEqual(td.outlook_proxy(hr, s + timedelta(hours=2))["state"], "dry")
        self.assertEqual(td.outlook_proxy(hr, s + timedelta(hours=2), recent=True)["state"], "rain")  # hour after a tip
        o = td.outlook_proxy(hr, s + timedelta(hours=3))
        self.assertEqual((o["state"], o["gust"]), ("silent", True))  # gauge-blind below 2 C; 35 km/h >= 30
        self.assertIsNone(td.outlook_proxy(hr, s + timedelta(hours=9)))
        hr[s]["n"] = "3"
        self.assertIsNone(td.outlook_proxy(hr, s))  # too few readings


def synth(tmp, taf_lines, rain_by_hour, t=4.0, extra_metar=""):
    with open(os.path.join(tmp, "2026-11-14-aviation.txt"), "w", encoding="utf-8") as f:
        f.write("# fetched 2026-11-14T06:40Z (taf)\n" + taf_lines + "\n")
        if extra_metar:
            f.write("# fetched 2026-11-14T06:40Z (metar)\n" + extra_metar + "\n")
    with open(os.path.join(tmp, "2026-11-14-hourly.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["hour_utc", "n", "p_lo", "p_mean", "p_hi", "t_mean", "dew_mean", "rh_mean", "wind_mean", "gust_max",
                    "wind_dir", "rain_mm", "solar_mean"])
        for i, rain in enumerate(rain_by_hour):
            w.writerow(["2026-11-14T%02d:00Z" % i, 55, "", 1010.0, "", t, 1.0, 90, 1.0, 5.0, 270, rain, 0])


class Classes(unittest.TestCase):
    TAF = ("TAF KJFK 140530Z 1406/1506 27010KT 9999 SCT030 \n  TEMPO 1408/1412 4000 -RA BKN012 \n"
           "  PROB30 \n  TEMPO 1418/1420 27015G25KT")

    def run_cli(self, tmp, *extra):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = td.main(["--dir", tmp] + list(extra))
        return rc, buf.getvalue()

    def rows(self, tmp, name="taf-disagree.csv"):
        with open(os.path.join(tmp, name), newline="", encoding="utf-8") as f:
            return list(csv.DictReader(f))

    def test_classes_and_cases_end_to_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            # hours 0..11 UTC: a tip at 09 only; TAF (issued 05:30, 6-h cycle by default) rain P 08..11
            synth(tmp, self.TAF, [0, 0, 0, 0, 0, 0, 0, 0, 0, 0.25, 0, 0, 0, 0])
            rc, out = self.run_cli(tmp)
            self.assertEqual(rc, 0)
            r = {x["hour_utc"][11:13]: x for x in self.rows(tmp)}
            self.assertEqual(r["07"]["class_rain"], "agree_quiet")  # covered from 06 (age < 6 h), TAF dry, no tip
            self.assertEqual(r["08"]["class_rain"], "taf_only")
            self.assertEqual(r["09"]["class_rain"], "agree_active")
            self.assertEqual(r["10"]["class_rain"], "taf_only")
            self.assertEqual(r["11"]["class_rain"], "taf_only")
            self.assertEqual(r["08"]["out_state"], "dry")  # T 4 C: the strip claims "No rain."
            self.assertEqual(r["05"]["covered"], "0")  # the TAF is not issued yet
            self.assertEqual(r["05"]["class_rain"], "")
            self.assertEqual(r["12"]["covered"], "0")  # age 6.5 h >= the 6-h cycle
            cases = self.rows(tmp, "taf-disagree-cases.csv")
            rain = [c for c in cases if c["group"] == "rain"]
            self.assertEqual(len(rain), 1)  # hours 10..11; the single hour 08 is not a case
            self.assertEqual((rain[0]["start"], rain[0]["hours"], rain[0]["taf_kind"]), ("2026-11-14T10:00Z", "2", "P"))
            self.assertEqual(rain[0]["no_rain_claim_hours"], "2")
            self.assertEqual(rain[0]["station_near_2h"], "1")  # the tip at 09 is next to the run
            self.assertEqual(rain[0]["verdict"], "station tip within 2 h")
            self.assertIn("| all | rain | 1 | ", out)

    def test_outlook_only_and_metar_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            synth(tmp, self.TAF, [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], extra_metar="")
            with open(os.path.join(tmp, "2026-11-14-hourly.csv"), newline="", encoding="utf-8") as f:
                rows = list(csv.reader(f))
            for i in (6, 7):  # tips in hours 06 and 07: the TAF is dry then
                rows[1 + i][11] = "0.25"
            with open(os.path.join(tmp, "2026-11-14-hourly.csv"), "w", newline="", encoding="utf-8") as f:
                csv.writer(f).writerows(rows)
            with open(os.path.join(tmp, "2026-11-14-aviation.txt"), "a", encoding="utf-8") as f:
                f.write("# fetched 2026-11-14T08:00Z (metar)\nMETAR KJFK 140651Z 27010KT 10SM -RA SCT030 05/03 A2990\n")
            self.run_cli(tmp)
            cases = self.rows(tmp, "taf-disagree-cases.csv")
            oo = [c for c in cases if c["class"] == "outlook_only"]
            self.assertEqual((len(oo), oo[0]["hours"], oo[0]["start"]), (1, "2", "2026-11-14T06:00Z"))
            self.assertEqual(oo[0]["verdict"], "airport METAR agrees")  # the METAR at 06:51 shows rain

    def test_gust_group_and_include_stale(self):
        with tempfile.TemporaryDirectory() as tmp:
            synth(tmp, self.TAF, [0] * 24)
            self.run_cli(tmp)
            self.assertTrue(all(x["class_gust"] != "taf_only" for x in self.rows(tmp)))  # hour 18 is 12.5 h old: not covered
            self.run_cli(tmp, "--include-stale")
            r = {x["hour_utc"][11:13]: x for x in self.rows(tmp)}
            self.assertEqual((r["18"]["taf_gust"], r["18"]["class_gust"]), ("P", "taf_only"))  # PROB30 TEMPO G25KT = 46 km/h

    def test_airports_are_anonymised_by_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            synth(tmp, self.TAF, [0] * 12)
            _, out = self.run_cli(tmp)
            with open(os.path.join(tmp, "taf-disagree.csv"), encoding="utf-8") as f:
                text = f.read() + out
            self.assertNotIn("KJFK", text)
            self.assertEqual({x["airport"] for x in self.rows(tmp)}, {"A"})
            self.run_cli(tmp, "--show-icao")
            self.assertEqual({x["airport"] for x in self.rows(tmp)}, {"KJFK"})

    def test_from_skips_earlier_hours_and_no_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self.run_cli(tmp)[0], 1)  # empty folder
            synth(tmp, self.TAF, [0] * 12)
            self.run_cli(tmp, "--from", "2026-11-14T06:00Z")
            self.assertEqual(self.rows(tmp)[0]["hour_utc"], "2026-11-14T06:00Z")


class Simulation(unittest.TestCase):
    def test_coverage_of_fetch_plans(self):
        self.assertAlmostEqual(td.simulate_coverage(10, 6, [7]), 0.25)
        self.assertAlmostEqual(td.simulate_coverage(10, 3, [7]), 0.125)
        self.assertAlmostEqual(td.simulate_coverage(10, 6, [0, 6.67, 12, 18]), 1.0)
        self.assertAlmostEqual(td.simulate_coverage(10, 3, [0, 6.67, 12, 18]), 0.5)
        self.assertAlmostEqual(td.simulate_coverage(10, 3, [0, 3, 6, 9, 12, 15, 18, 21]), 1.0)


if __name__ == "__main__":
    unittest.main()
