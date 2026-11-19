"""tools/check_partitions.py -- the shipped tables pass; a one-byte edit of the frozen table fails the hash gate; a
table that is not contiguous, ends short or does not match the profile's constants fails the static check."""
import io
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import check_partitions as cp  # noqa: E402

SKETCH = os.path.join(ROOT, "firmware", "fauxcloud")


class CheckPartitions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.tmp, "src"))
        for f in ("partitions.csv", "partitions-psram.csv", "partitions.csv.sha256"):
            shutil.copy(os.path.join(SKETCH, f), self.tmp)
        shutil.copy(os.path.join(SKETCH, "src", "ring.h"), os.path.join(self.tmp, "src"))

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_check(self):
        with redirect_stdout(io.StringIO()) as out:
            rc = cp.main(["--sketch", self.tmp])
        return rc, out.getvalue()

    def edit(self, name, old, new):
        p = os.path.join(self.tmp, name)
        s = open(p, encoding="utf-8").read()
        self.assertIn(old, s)
        open(p, "w", encoding="utf-8").write(s.replace(old, new, 1))

    def test_shipped_tables_pass(self):
        rc, out = self.run_check()
        self.assertEqual(rc, 0, out)
        self.assertIn("hash gate ok", out)

    def test_one_byte_edit_fails_the_hash_gate(self):
        self.edit("partitions.csv", "0x46000", "0x46001")
        rc, out = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("the 1.x table is frozen", out)

    def test_table_and_pin_edited_together_still_fail(self):
        # a new pin next to a new table is not enough: the script keeps its own copy of the frozen hash
        self.edit("partitions.csv", "0x46000", "0x46001")
        with open(os.path.join(self.tmp, "partitions.csv.sha256"), "w", encoding="utf-8") as fh:
            fh.write(cp.table_hash(os.path.join(self.tmp, "partitions.csv")) + "  partitions.csv\n")
        rc, out = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("FROZEN_SHA256", out)

    def test_crlf_does_not_change_the_hash(self):
        p = os.path.join(self.tmp, "partitions.csv")
        data = open(p, "rb").read().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        open(p, "wb").write(data)
        rc, out = self.run_check()
        self.assertEqual(rc, 0, out)

    def test_static_checks(self):
        self.edit("partitions-psram.csv", "littlefs, data, littlefs, 0x398000, 0x68000,", "littlefs, data, littlefs, 0x398000, 0x67000,")
        rc, out = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("ends at 0x3FF000", out)
        self.assertIn("littlefs must be at 0x398000 with 104", out)

    def test_empty_pin_file_fails_cleanly(self):
        open(os.path.join(self.tmp, "partitions.csv.sha256"), "w").close()
        rc, out = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("the 1.x table is frozen", out)

    def test_profile_mismatch(self):
        self.edit(os.path.join("src", "ring.h"), "constexpr int LFS_BLOCKS = 70;", "constexpr int LFS_BLOCKS = 72;")
        rc, out = self.run_check()
        self.assertEqual(rc, 1)
        self.assertIn("partitions.csv: littlefs must be at 0x3BA000 with 72", out)


if __name__ == "__main__":
    unittest.main()
