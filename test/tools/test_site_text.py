"""The /try/ demo bar's words are pinned: every string of demo.js's word tables (W, EX, SC) is in site/try/demo_text.py
and the other way round, each table covers the ids of demo/fixtures/index.json, and the bar builds no sentence from
pieces (one template per case).

Usage: python -I -m unittest discover -s test/tools
"""
import json
import pathlib
import re
import runpy
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
JS = (ROOT / "site" / "try" / "demo.js").read_text(encoding="utf-8")
PINNED = runpy.run_path(str(ROOT / "site" / "try" / "demo_text.py"))["TEXTS"]
INDEX = json.loads((ROOT / "demo" / "fixtures" / "index.json").read_text(encoding="utf-8"))
ENTRY = re.compile(r"^\s*('[^']+'|[A-Za-z]\w*): '((?:[^'\\]|\\.)*)',$", re.M)


def table(name):
    m = re.search(r"const %s = \{\n(.*?)\n  \};" % name, JS, re.S)
    if not m:
        raise AssertionError("no table %s in demo.js" % name)
    rows = ENTRY.findall(m.group(1))
    if len(rows) != len([l for l in m.group(1).splitlines() if l.strip()]):
        raise AssertionError("table %s: one `key: 'text',` per line" % name)
    return {k.strip("'"): v for k, v in rows}


class Words(unittest.TestCase):
    def test_tables_match_the_pinned_list_both_ways(self):
        for name, pinned in PINNED.items():
            self.assertEqual(sorted(table(name).values()), sorted(pinned), name)

    def test_every_example_day_and_scenario_has_its_name(self):
        self.assertEqual(set(table("EX")), {e["id"] for e in INDEX["example_days"]})
        self.assertEqual(set(table("SC")), set(INDEX["scenarios"]))

    def test_sentence_case_and_one_template_per_case(self):
        for name in PINNED:
            for v in table(name).values():
                if v != "demo: not saved":  # the board's error format, "<key>: <reason>"
                    self.assertTrue(v[0].isupper(), v)
        # the only slot is filled whole; no word table entry is concatenated with another
        self.assertEqual(re.findall(r"\{(\w+)\}", "".join(table("W").values())), ["n"])
        self.assertIsNone(re.search(r"W\.\w+\s*\+|\+\s*W\.\w+", JS), "a sentence built from pieces")

    def test_no_other_visible_words_in_the_markup(self):
        # the bar's markup takes its words from the tables only: no text between tags in a template literal
        code = re.sub(r"(^|\s)//.*$", "", JS, flags=re.M)  # line comments (no string of demo.js holds "//")
        for m in re.finditer(r"<[a-z][^<>]*>([^<>${}`]*[A-Za-z]{2,}[^<>${}`]*)<", code):
            self.fail("words outside the tables: %r" % m.group(1))


if __name__ == "__main__":
    unittest.main()
