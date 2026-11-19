"""The language pack source format 2 (tools/i18n_pack.py): a reference resolves to the common entry with at most one of
the two whole-phrase transforms (cap, sent), an explicit variant wins, a dangling or mismatched reference is a problem.

The fixtures use ASCII stand-ins for Polish (the leak gate keeps Polish text to web/lang/).

Usage: python -I -m unittest discover -s test/tools
"""
import importlib.util
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def _load(name):
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name + "_t", os.path.join(ROOT, "tools", name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


P = _load("i18n_pack")


class Pack(unittest.TestCase):
    D = {"_meta": {"name": "Polski", "format": 2},
         "cmn.light_rain": {"en": "light rain", "pl": "slaby deszcz"},
         "cmn.calm": {"en": "Calm", "pl": "Quiet"},
         "cmn.calm#sent": {"en": "Calm.", "pl": "No wind."},
         "a.w": {"en": "light rain", "pl": "@cmn.light_rain"},
         "a.s": {"en": "Light rain", "pl": "@cmn.light_rain#cap"},
         "b.o": {"en": "Light rain.", "pl": "@cmn.light_rain#sent"},
         "b.c": {"en": "Calm.", "pl": "@cmn.calm#sent"},
         "b.x": {"en": "Calm", "pl": "Spokoj"},
         "b.n": {"en": "{n} items", "pl": {"one": "{n} item", "few": "{n} itemy", "many": "{n} itemy", "other": "{n} itemy"}}}

    def test_references_resolve_with_the_two_transforms(self):
        out, bad = P.resolve(self.D, "pl")
        self.assertEqual(bad, [])
        self.assertEqual([out["a.w"], out["a.s"], out["b.o"]], ["slaby deszcz", "Slaby deszcz", "Slaby deszcz."])
        self.assertEqual(out["b.x"], "Spokoj")  # a literal: the override
        self.assertNotIn("cmn.calm", out)  # the common table is no page's namespace

    def test_an_explicit_variant_wins_over_the_transform(self):
        self.assertEqual(P.resolve(self.D, "pl")[0]["b.c"], "No wind.")

    def test_a_sentence_keeps_its_own_end_mark(self):
        self.assertEqual([P.sent("ok"), P.sent("ok?"), P.sent("wait…"), P.cap("élan")], ["Ok.", "Ok?", "Wait…", "Élan"])

    def test_dangling_mismatched_and_unknown_references_are_problems(self):
        for v, en, want in (("@cmn.nope", "x", "dangling"), ("@cmn.light_rain#sent", "Light rain", "in English"),
                            ("@cmn.light_rain#lower", "light rain", "not a reference"), ("@dash.x", "x", "not a reference")):
            d = dict(self.D, **{"c.k": {"en": en, "pl": v}})
            bad = dict(P.resolve(d, "pl")[1])
            self.assertIn("c.k", bad, v)
            self.assertIn(want, bad["c.k"], v)

    def test_unused_common_entries_are_found(self):
        self.assertEqual(P.refs(self.D, "pl")["calm"], ["b.c"])


if __name__ == "__main__":
    unittest.main()
