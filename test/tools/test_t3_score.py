"""tools/t3_score.py: the Python port of the T3 mapping against the vectors the host test runs through
the firmware (t3_vectors.json), and the scoring on a synthetic day. Run: python -I -m unittest discover -s test/tools"""
import contextlib
import importlib.util
import io
import json
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("t3_score", os.path.join(HERE, "..", "..", "tools", "t3_score.py"))
ts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ts)
VEC = json.load(open(os.path.join(HERE, "t3_vectors.json"), encoding="utf-8"))

HOURLY_HEAD = "hour_utc,hour_local,n,t_mean,t_min,t_max,td_mean,rh_mean,p_mean,wind_mean,gust_max,rain_mm,solar_mean"


class Mapping(unittest.TestCase):
    def test_vectors(self):
        self.assertGreaterEqual(len(VEC["map"]), 20)
        for v in VEC["map"]:
            self.assertEqual(ts.near_of(v["wx"]), v["near"], v["wx"])

    def test_constants_match_the_firmware(self):
        src = open(os.path.join(HERE, "..", "..", "firmware", "fauxcloud", "src", "metar_store.h"), encoding="utf-8").read()
        self.assertIn("METAR_CURRENT_S = %d;" % VEC["current_s"], src)
        self.assertIn("METAR_NEAR_KM = %d;" % VEC["near_km"], src)
        self.assertEqual(ts.CURRENT_S, VEC["current_s"])

    def test_report_groups_stop_at_the_trend(self):
        self.assertEqual(ts.report_groups("METAR EGLL 161220Z 25010KT 9999 -DZ BR OVC008 08/07 Q1013 TEMPO RA"),
                         ["25010KT", "9999", "-DZ", "BR", "OVC008", "08/07", "Q1013"])


class Scoring(unittest.TestCase):
    def day(self, rain_hours=(), gust_hours=()):
        d = tempfile.mkdtemp()
        rows = [HOURLY_HEAD]
        for h in range(6, 18):
            rows.append("2026-11-16T%02d:00Z,x,55,8.0,7.5,8.5,6.0,92,1012.0,6,%s,%s,0" % (
                h, "35" if h in gust_hours else "12", "0.2" if h in rain_hours else "0"))
        with open(os.path.join(d, "2026-11-16-hourly.csv"), "w", encoding="utf-8", newline="") as f:
            f.write("\n".join(rows) + "\n")
        av = []
        for h in range(6, 18):  # half-hourly drizzle + mist all day; fog at 12:20
            for mm in (20, 50):
                wx = "FG" if (h, mm) == (12, 20) else "-DZ BR"
                av.append("# fetched 2026-11-16T%02d:%02dZ (metar)\nMETAR EGLL 16%02d%02dZ 25008KT 3000 %s OVC006 08/07 Q1012" % (h, mm + 5, h, mm, wx))
        with open(os.path.join(d, "2026-11-16-aviation.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(av) + "\n")
        return d

    def run_score(self, d):
        _, metars = ts.td.read_aviation([os.path.join(d, "2026-11-16-aviation.txt")])
        hourly = ts.td.read_hourly([os.path.join(d, "2026-11-16-hourly.csv")])
        return ts.score(metars, hourly)

    def test_local_first_and_shares(self):
        eligible, tally, disagree = self.run_score(self.day(rain_hours=(9,), gust_hours=(14,)))
        self.assertEqual(eligible, 12)
        # our tip at 09 (rain) and 10 (recent rain): drizzle suppressed twice, shown the other 10 hours
        self.assertEqual((tally["drizzle"]["shown"], tally["drizzle"]["suppressed"]), (10, 2))
        # mist below our gusts warning at 14 (the 12:50 report is in force at 13:00 -> the 12:00 hour is drizzle + mist)
        self.assertEqual(tally["mist"]["suppressed"], 1)
        self.assertEqual(tally["mist"]["shown"] + tally["fog"]["shown"] + tally["mist"]["suppressed"], 12)
        # the airport's drizzle while our gauge says "No rain." for hours on end: two runs around our tip
        self.assertEqual([n for _, n in disagree], [3, 7])

    def test_old_report_is_not_eligible(self):
        _, metars = ts.td.read_aviation([os.path.join(self.day(), "2026-11-16-aviation.txt")])
        hourly = ts.td.read_hourly([os.path.join(self.day(), "2026-11-16-hourly.csv")])
        early = [m for m in metars if m.time.hour < 8]  # reports end at 07:50: the 06, 07 and 08 hours (10 / 10 / 70 min), not 09 (130)
        eligible, _, _ = ts.score(early, hourly)
        self.assertEqual(eligible, 3)

    def test_main_prints_placeholders(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(ts.main(["--dir", self.day()]), 0)
        self.assertIn("AIRPORT-1", out.getvalue())
        self.assertNotIn("EGLL", out.getvalue())


if __name__ == "__main__":
    unittest.main()
