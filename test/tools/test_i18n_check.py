"""The i18n lint (tools/i18n_inventory.py --check): no strict-tier text outside a W table in the dashboard's files and no
allow-list entry that matches nothing; a planted sentence in a copy of the web tree fails the check (it bites), and so do a
planted short word (the soft tier), a pack entry whose English moved (stale) and a tag whose attributes the translation
changed; the Polish pack keeps every slot, tag and count form of the English.

Usage: python -I -m unittest discover -s test/tools   (needs node: FAUXCLOUD_NODE or node on PATH)
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TOOL = os.path.join(ROOT, "tools", "i18n_inventory.py")
WEB = os.path.join(ROOT, "firmware", "fauxcloud", "web")


def _inventory():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("i18n_inventory_ro", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run_check(web=None):
    argv = [sys.executable, "-I", TOOL, "--check"] + (["--web", web] if web else [])
    r = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=ROOT)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def planted(edit):
    """(exit code, output) of the check on a copy of the web tree after edit(web folder)."""
    with tempfile.TemporaryDirectory() as tmp:
        web = os.path.join(tmp, "web")
        shutil.copytree(WEB, web, ignore=shutil.ignore_patterns("data"))
        os.makedirs(os.path.join(web, "data"))
        edit(web)
        return run_check(web)


def replace_in(path, old, new):
    with open(path, encoding="utf-8") as f:
        s = f.read()
    assert old in s, old
    with open(path, "w", encoding="utf-8") as f:
        f.write(s.replace(old, new, 1))


class I18nCheck(unittest.TestCase):
    def test_no_text_outside_w_on_the_translated_faces(self):
        m = _inventory()
        hits, _, unused = m.lint(m.inventory(), m.lang_files())
        self.assertEqual(["%s:%d %s" % (r["file"], r["line"], r["msg"]) for r in hits], [])
        self.assertEqual(unused, [])

    def test_the_pack_keeps_slots_tags_and_forms(self):
        fails = [x for x in _inventory().coverage("pl") if x[0] == "fail"]
        self.assertEqual(fails, [])

    def test_a_planted_sentence_fails_the_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            web = os.path.join(tmp, "web")
            shutil.copytree(WEB, web, ignore=shutil.ignore_patterns("data"))
            os.makedirs(os.path.join(web, "data"))
            with open(os.path.join(web, "dash.js"), "a", encoding="utf-8") as f:
                f.write("\nconst planted = () => `<p>Planted words in a sentence.</p>`;\n")
            code, out = run_check(web)
            self.assertEqual(code, 1, out)
            self.assertIn("Planted words in a sentence.", out)

    def test_a_planted_short_word_fails_the_check(self):
        # the soft tier (a short lower-case word outside W) is a failure too: a real exception goes to the allow-list
        def edit(web):
            with open(os.path.join(web, "dash.js"), "a", encoding="utf-8") as f:
                f.write("\nconst plantedSoft = () => { $('#x').textContent = 'gust'; };\n")
        code, out = planted(edit)
        self.assertEqual(code, 1, out)
        self.assertIn("soft (check)", out)

    def test_a_stale_entry_fails_the_check(self):
        # the strings are frozen: an English that moved under its translation fails, it is not a warning
        code, out = planted(lambda web: replace_in(os.path.join(web, "lang", "pl.json"), '"en": "reading"', '"en": "a reading"'))
        self.assertEqual(code, 1, out)
        self.assertIn("stale: the English changed", out)

    def test_a_proposed_translation_fails_the_check(self):
        # a release carries only reviewed translations: one row still marked proposed fails, and the count is printed
        code, out = planted(lambda web: replace_in(os.path.join(web, "lang", "pl.json"), '"pl": "odczyt"}', '"pl": "odczyt", "proposed": true}'))
        self.assertEqual(code, 1, out)
        self.assertIn("proposed (not reviewed)  dash-sheet.n_read", out)
        self.assertIn("1 proposed", out)

    def test_a_tag_with_other_attributes_fails_the_check(self):
        # whole tags are compared: the same tag name with an attribute the English has not is a broken page
        code, out = planted(lambda web: replace_in(os.path.join(web, "lang", "pl.json"), "<b>Na konsoli</b>", '<b class=\\"x\\">Na konsoli</b>'))
        self.assertEqual(code, 1, out)
        self.assertIn("tags differ", out)

    def test_a_dangling_reference_fails_the_check(self):
        # format 2: a key that references a common entry that is not there breaks the pack
        with tempfile.TemporaryDirectory() as tmp:
            web = os.path.join(tmp, "web")
            shutil.copytree(WEB, web, ignore=shutil.ignore_patterns("data"))
            os.makedirs(os.path.join(web, "data"))
            p = os.path.join(web, "lang", "pl.json")
            with open(p, encoding="utf-8") as f:
                s = f.read()
            self.assertIn('"pl": "@cmn.close"', s)
            with open(p, "w", encoding="utf-8") as f:
                f.write(s.replace('"pl": "@cmn.close"', '"pl": "@cmn.no_such_label"', 1))
            code, out = run_check(web)
            self.assertEqual(code, 1, out)
            self.assertIn("dangling reference @cmn.no_such_label", out)

    def test_a_key_twice_in_one_file_fails(self):
        # two tables of one file share its pack namespace: a repeated key would get one Polish text for both
        with tempfile.TemporaryDirectory() as tmp:
            web = os.path.join(tmp, "web")
            shutil.copytree(WEB, web, ignore=shutil.ignore_patterns("data"))
            os.makedirs(os.path.join(web, "data"))
            p = os.path.join(web, "dash.js")
            with open(p, "a", encoding="utf-8") as f:
                f.write("\nconst WX = FC.w('dash', { off_why: 'Twice' });\n")
            r = subprocess.run([sys.executable, "-I", TOOL, "--keys", "--web", web], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, cwd=ROOT)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("duplicate key dash.off_why", r.stderr.decode("utf-8", "replace") + r.stdout.decode("utf-8", "replace"))


if __name__ == "__main__":
    unittest.main()
