"""tools/forecast_trial.py log_learn: the first layout's columns keep their places, a row of the extended layout has one
fixed column set, and a day file never mixes two layouts (every candidate's header is checked on every run). Synthetic dicts, no board."""
import csv
import importlib.util
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("forecast_trial", os.path.join(HERE, "..", "..", "tools", "forecast_trial.py"))
ft = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ft)


def learn_v1(night=True):
    d = {"state": "learning", "last_hour": 1, "backfill": {"hours": 5, "done": True},
         "l1": {"cells": [{"el": 5, "pm": False, "e": .55, "n": 0, "conf": "default"}]}, "l1b": {"sun": 0, "shade": 0},
         "l2": {"s": 100, "src": "prior"}, "l3": {"e": 1.0, "n": 0}, "l4": {"p50": 3}, "l7": {"e": None},
         "gates": {g: 0 for g in ft.GATES_V1}, "night": {"last": None}}
    if night:
        d["night"]["last"] = {k: 1 for k in ft.NIGHT_V1}
    return d


def learn_l5l6(night=True):
    d = learn_v1(night)
    d["gates"].update({g: 2 for g in ft.GATES_L5L6})
    if night:
        d["night"]["last"].update({"l_h": 14.9, "w5": 15, "rise": False, "l6": "quality", "pred": -4.3})
    d["l5"] = {"clear": {"n": 3}, "overcast": {"n": 2}, "thr": 1.0, "src": "default"}
    d["l6"] = {"a": 1, "b": .4, "c": .2, "n": 0, "sd": None, "conf": "default"}
    return d


def header(p):
    with open(p, encoding="utf-8") as f:
        return next(csv.reader(f))


def rows(p):
    with open(p, encoding="utf-8") as f:
        return list(csv.reader(f))[1:]


class LogLearn(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.day = os.path.join(self.dir, "2026-11-11-learn.csv")

    def test_b9_prefix_kept(self):
        ft.log_learn(self.day, "ST1", learn_v1())
        h9 = header(self.day)
        ft.log_learn(self.day, "ST1", learn_l5l6())
        files = sorted(os.listdir(self.dir))
        self.assertEqual(len(files), 2)  # the first layout's day file and one -v file
        v = os.path.join(self.dir, [f for f in files if "-v" in f][0])
        h12 = header(v)
        self.assertEqual(h12[:len(h9)], h9)  # every first-layout column keeps its place
        self.assertEqual(h12[len(h9):len(h9) + 10], ["l5_thr", "l5_src", "l5_cn", "l5_on", "l6_a", "l6_b", "l6_c", "l6_n",
                                                     "l6_sd", "l6_conf"])

    def test_b12_fixed_columns(self):
        # a board right after boot (no closed night yet) and later (a night): one header, one file
        ft.log_learn(self.day, "ST1", learn_l5l6(night=False))
        ft.log_learn(self.day, "ST1", learn_l5l6(night=True))
        self.assertEqual(os.listdir(self.dir), ["2026-11-11-learn.csv"])
        self.assertEqual(len(rows(self.day)), 2)
        h = header(self.day)
        self.assertIn("night_pred", h)
        self.assertIn("gate_l6_night", h)

    def test_versioned_file_header_checked(self):
        # an unrelated file of the same length already holds the -v name: the row goes to -v<len>-2, never into it
        ft.log_learn(self.day, "ST1", learn_v1())
        cols = len(header(self.day))
        probe = os.path.join(self.dir, "probe.csv")
        ft.log_learn(probe, "ST1", learn_l5l6())
        n12 = len(header(probe))
        squat = os.path.join(self.dir, "2026-11-11-learn-v%d.csv" % n12)
        with open(squat, "w", encoding="utf-8") as f:
            f.write(",".join("x%d" % i for i in range(n12)) + "\n")
        ft.log_learn(self.day, "ST1", learn_l5l6())
        self.assertEqual(len(rows(squat)), 0)
        second = os.path.join(self.dir, "2026-11-11-learn-v%d-2.csv" % n12)
        self.assertTrue(os.path.exists(second))
        ft.log_learn(self.day, "ST1", learn_l5l6())
        self.assertEqual(len(rows(second)), 2)
        self.assertEqual(len(header(self.day)), cols)
        self.assertEqual(len(rows(self.day)), 1)


    def test_b124_migration_columns_last(self):
        d = learn_l5l6()
        d["migrated_from"] = 1
        d["backfill"]["replay"] = {"hours": 2853, "ms": 140123, "learners": ["l5", "l6"]}
        ft.log_learn(self.day, "ST1", d)
        h = header(self.day)
        self.assertEqual(h[-3:], ["migrated_from", "replay_h", "replay_ms"])
        self.assertEqual(rows(self.day)[0][-3:], ["1", "2853", "140123"])

    def test_b23_states_and_use_last(self):
        """States, needs and the `use` block after every older column"""
        d = learn_l5l6()
        d["migrated_from"] = 2
        d["bars"] = {"l1": [6, 12, 27]}
        d["relearn_hour"] = 0
        for c in d["l1"]["cells"]:
            c.update({"state": "default", "need": 6, "from": None})
        d["l2"].update({"state": "in_use"})
        d["use"] = {"hour": 1794322800, "l2": True, "rh_sat": 93.4}
        ft.log_learn(self.day, "ST1", d)
        h = header(self.day)
        self.assertLess(h.index("replay_ms"), h.index("l2_state"))
        self.assertEqual(h[-3:], ["use_hour", "use_l2", "use_rh_sat"])
        self.assertEqual(rows(self.day)[0][-3:], ["1794322800", "True", "93.4"])

if __name__ == "__main__":
    unittest.main()
