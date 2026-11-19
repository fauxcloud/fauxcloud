"""The notices' English in the language pack source (web/lang/pl.json, namespace notice: dash.js NT is an empty table the pack
fills) says the firmware's notices (src/notices.cpp) in the same words, key by key:
every id with its chip, its text (the count-of-one variants and baro_ref's ending without a reference included), the console
clock's amounts and the direction words. Each C format specifier maps, in order, to the JS template's slots in the order they
appear. A missing id, an extra id or one changed word fails; the self-test below proves the check bites.

Usage: python -I -m unittest discover -s test/tools
"""
import json
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
CPP = os.path.join(ROOT, "firmware", "fauxcloud", "src", "notices.cpp")
PACK = os.path.join(ROOT, "firmware", "fauxcloud", "web", "lang", "pl.json")
SPEC = re.compile(r"%(?:lu|ld|d|s)")


def cpp_table(src):
    """{"id": (chip, text)} from NOTICES[], plus the whole-sentence variants and the slot words, from notices.cpp."""
    body = src[src.index("const NoticeDef NOTICES[NK_COUNT] = {"):]
    body = body[:body.index("};")]
    rows = {m.group(1): (m.group(2), m.group(3).replace('\\"', '"'))
            for m in re.finditer(r'\{"(\w+)",\s*"([^"]*)",\s*"((?:[^"\\]|\\.)*)"\}', body)}
    const = lambda name: re.search(r'static const char %s\[\] = "((?:[^"\\]|\\.)*)";' % name, src).group(1)
    words = {"n_days": re.search(r'"(%lu days)"', src).group(1), "n_h": re.search(r'"(%lu h)"', src).group(1),
             "n_min": re.search(r'"(%lu min)"', src).group(1),
             "n_below": "below" if '"below" : "above"' in src else None, "n_above": "above" if '"below" : "above"' in src else None}
    return rows, {"baro_ref_old": const("BARO_REF_OLD"), "key_mismatch_one": const("KEY_MISMATCH_ONE"),
                  "store_fail_one": const("STORE_FAIL_ONE")}, words


def as_js(c_text, js_text):
    """The C text with its specifiers replaced, in order, by the JS template's slots in order of appearance; None when the
    counts differ."""
    slots = re.findall(r"\{(\w+)\}", js_text)
    parts = SPEC.split(c_text)
    if len(parts) - 1 != len(slots):
        return None
    return "".join(p + ("{%s}" % slots[i] if i < len(slots) else "") for i, p in enumerate(parts))


def differences(cpp, js):
    """[problem] between the C++ notices and the JS table js = {key: text or count entry} (NT's nc_ / nt_ / n_ keys)."""
    rows, extra, words = cpp
    out = []
    ids = {k[3:] for k in js if k.startswith("nc_")}
    for i in sorted(set(rows) - ids):
        out.append("missing in NT: %s" % i)
    for i in sorted(ids - set(rows)):
        out.append("not in notices.cpp: %s" % i)
    pairs = []
    for i, (chip, text) in rows.items():
        if i not in ids:
            continue
        if js.get("nc_" + i) != chip:
            out.append("chip %s: %r != %r" % (i, js.get("nc_" + i), chip))
        t = js.get("nt_" + i)
        if isinstance(t, dict):
            pairs += [(i + " (n)", text, t.get("other", "")), (i + " (1)", extra[i + "_one"], t.get("one", ""))]
        else:
            pairs.append((i, text, t or ""))
    pairs.append(("baro_ref without a reference", extra["baro_ref_old"], js.get("nt_baro_ref_old", "")))
    for k, w in words.items():
        v = js.get(k)
        pairs.append((k, w or "", (v.get("other", "") if isinstance(v, dict) else v) or ""))
    for name, c, j in pairs:
        if as_js(c, j) != j:
            out.append("text %s: %r != %r" % (name, j, c))
    return out


class NoticesSync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(CPP, encoding="utf-8") as f:
            cls.cpp = cpp_table(f.read())
        # the notices' English lives in the pack source (dash.js registers an empty table the pack fills): its "en" fields
        with open(PACK, encoding="utf-8") as f:
            cls.js = {k.split(".", 1)[1]: e["en"] for k, e in json.load(f).items() if k.startswith("notice.")}

    def test_the_table_reads_the_firmware(self):
        self.assertEqual(len(self.cpp[0]), 15)

    def test_same_words_key_by_key(self):
        self.assertEqual(differences(self.cpp, self.js), [])

    def test_the_check_bites(self):
        changed = dict(self.js, nt_heap_low=self.js["nt_heap_low"].replace("readings still arrive", "readings arrive"))
        self.assertEqual(len(differences(self.cpp, changed)), 1)
        one = dict(self.js, nt_store_fail=dict(self.js["nt_store_fail"], one=self.js["nt_store_fail"]["one"].replace("1 write", "one write")))
        self.assertEqual(len(differences(self.cpp, one)), 1)
        gone = {k: v for k, v in self.js.items() if not k.endswith("clock_behind")}
        self.assertEqual(differences(self.cpp, gone), ["missing in NT: clock_behind"])
        slots = dict(self.js, nt_profile_changed="Station profile changed: {since} {extra}.")
        self.assertEqual(len(differences(self.cpp, slots)), 1)
        words = dict(self.js, n_below="under")
        self.assertEqual(len(differences(self.cpp, words)), 1)


if __name__ == "__main__":
    unittest.main()
