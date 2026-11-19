"""REUSE.toml and LICENSES/ agree: every SPDX identifier the annotations declare has its licence text in LICENSES/,
every file in LICENSES/ is declared by some annotation (REUSE's "unused licenses"), and the licence texts ship.
`reuse lint` (the build.yml `reuse` job) is the full check; this is the stdlib part that runs everywhere, no install."""
import os
import re
import tomllib
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LICENSES = os.path.join(ROOT, "LICENSES")


def declared_ids(text=None):
    if text is None:
        with open(os.path.join(ROOT, "REUSE.toml"), "rb") as f:
            data = tomllib.load(f)
    else:
        data = tomllib.loads(text)
    ids = set()
    for ann in data.get("annotations", []):
        expr = ann.get("SPDX-License-Identifier", "")
        for one in [expr] if isinstance(expr, str) else expr:
            # an expression: ids joined by AND / OR / WITH, parentheses allowed
            ids.update(t for t in re.split(r"[\s()]+", one) if t and t not in ("AND", "OR", "WITH"))
    return ids


def licence_files():
    return sorted(os.listdir(LICENSES))


class ReuseLicences(unittest.TestCase):
    def test_every_declared_id_has_a_text(self):
        have = set(licence_files())
        missing = sorted(i for i in declared_ids() if i + ".txt" not in have)
        self.assertEqual(missing, [], "no LICENSES/<id>.txt for: %s" % ", ".join(missing))

    def test_every_text_is_declared(self):
        ids = declared_ids()
        unused = [n for n in licence_files() if not (n.endswith(".txt") and n[:-4] in ids)]
        self.assertEqual(unused, [], "LICENSES/ files no annotation references (or not named <id>.txt): %s" % ", ".join(unused))

    def test_texts_are_not_empty(self):
        for n in licence_files():
            with open(os.path.join(LICENSES, n), "rb") as f:
                self.assertGreater(len(f.read().strip()), 100, n)

    def test_the_expression_split(self):
        sample = """
version = 1
[[annotations]]
path = "a"
SPDX-License-Identifier = "GPL-3.0-or-later WITH GCC-exception-3.1"
[[annotations]]
path = "b"
SPDX-License-Identifier = "(ISC AND MIT) OR LicenseRef-x"
[[annotations]]
path = "c"
SPDX-License-Identifier = ["Zlib", "Apache-2.0 AND MIT"]
"""
        self.assertEqual(declared_ids(sample), {"GPL-3.0-or-later", "GCC-exception-3.1", "ISC", "MIT", "LicenseRef-x",
                                                 "Zlib", "Apache-2.0"})

    def test_the_licence_texts_ship(self):
        with open(os.path.join(ROOT, "tools", "publish-manifest.txt"), encoding="utf-8") as f:
            lines = [ln.split("#")[0].split() for ln in f]
        self.assertIn(["ship", "LICENSES/**"], lines)
        self.assertIn(["ship", "REUSE.toml"], lines)


if __name__ == "__main__":
    unittest.main()
