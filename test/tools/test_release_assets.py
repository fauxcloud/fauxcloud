"""tools/release_assets.py -- the tag check refuses a tag that is not "v" + FC_VERSION_BASE and a -dev version; the
assemble step refuses an image whose marker says another version, a partition table that is not partitions.csv, and
any first-install part that would touch NVS or another kept partition (a merged image at 0x0 included); it writes the
assets, SHA256SUMS in `sha256sum -c` form, the factory zip with its offsets, and the release body with every
placeholder filled. Fixtures are synthetic and built at run time."""
import hashlib
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import release_assets as R  # noqa: E402

CSV = """# Name, Type, SubType, Offset, Size, Flags
nvs,      data, nvs,      0x9000,   0x5000,
otadata,  data, ota,      0xE000,   0x2000,
app0,     app,  ota_0,    0x10000,  0x1D0000,
app1,     app,  ota_1,    0x1E0000, 0x1D0000,
coredump, data, coredump, 0x3B0000, 0xA000,
littlefs, data, littlefs, 0x3BA000, 0x46000,
"""
FLASH_ARGS = ("--flash-mode dio --flash-freq 80m --flash-size 4MB\n0x1000 fauxcloud.ino.bootloader.bin\n"
              "0x8000 fauxcloud.ino.partitions.bin\n0xe000 boot_app0.bin\n0x10000 fauxcloud.ino.bin\n")
TEMPLATE = ("# fauxcloud {{version}}\n\n{{changelog}}\n\nSource: this repository at tag `{{tag}}` ({{repo_url}}).\n"
            "Notices: THIRD_PARTY_NOTICES_FIRMWARE.txt\n\n```\n{{sha256sums}}\n```\n")
CHANGELOG = "# Changelog\n\n## [1.0.1] - later\n- next\n\n## [1.0.0] - 2026-11-27\n### Added\n- first release\n\n## [0.9.0]\n- old\n"


def table_bin(csv_text):
    out = b""
    for r in R.parse_csv(csv_text):
        t = R.TYPES[r["type"]]
        s = R.SUBTYPES[r["type"]][r["subtype"]]
        out += struct.pack("<HBBII", 0x50AA, t, s, r["offset"], r["size"]) + r["name"].encode().ljust(16, b"\0") + b"\0" * 4
    out += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(out).digest()
    return out.ljust(0xC00, b"\xff")


def image(version, size=4096):
    return (b"\xe9" + b"\0" * 31 + R.MARKER + version.encode() + b" Nov 27 2026 10:00:00\0").ljust(size, b"\x5a")


def run(argv):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = R.main(argv)
    return rc, out.getvalue(), err.getvalue()


class VersionTests(unittest.TestCase):
    def test_read_version(self):
        self.assertEqual(R.read_version('#define X 1\n#define FC_VERSION_BASE "1.0.0-rc.1"\n'), "1.0.0-rc.1")
        with self.assertRaises(R.Fail):
            R.read_version('#define FC_VERSION_BASE "1.0"\n')
        with self.assertRaises(R.Fail):
            R.read_version('#define FC_VERSION_BASE "1.0.0"\n#define FC_VERSION_BASE "1.0.1"\n')
        with self.assertRaises(R.Fail):
            R.read_version("nothing here\n")

    def test_tag_must_equal_the_version(self):
        R.check_tag("1.0.0-rc.1", "v1.0.0-rc.1")
        R.check_tag("1.0.0", "v1.0.0")
        for v, t in (("1.0.0", "v1.0.0-rc.1"), ("1.0.0", "1.0.0"), ("1.0.0-rc.1", "v1.0.0"), ("1.0.0", "v0.0.0-test")):
            with self.assertRaises(R.Fail):
                R.check_tag(v, t)

    def test_dev_is_never_released(self):
        with self.assertRaises(R.Fail):
            R.check_tag("1.0.1-dev.3", "v1.0.1-dev.3")
        self.assertTrue(R.is_prerelease("1.0.0-rc.1"))
        self.assertFalse(R.is_prerelease("1.0.0"))

    def test_cli_outputs(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        p = os.path.join(tmp, "app.h")
        with open(p, "w") as f:
            f.write('#define FC_VERSION_BASE "1.0.0-rc.1"\n')
        rc, out, _ = run(["version", "--app-h", p, "--tag", "v1.0.0-rc.1"])
        self.assertEqual(rc, 0)
        self.assertEqual(out.split(), ["version=1.0.0-rc.1", "tag=v1.0.0-rc.1", "prerelease=true"])
        rc, out, _ = run(["version", "--app-h", p])
        self.assertEqual((rc, out.split()[1]), (0, "tag=v1.0.0-rc.1"))
        rc, out, err = run(["version", "--app-h", p, "--tag", "v1.0.0"])
        self.assertEqual((rc, out), (1, ""))
        self.assertIn("does not match", err)


class LayoutTests(unittest.TestCase):
    rows = R.parse_csv(CSV)

    def parts(self, **over):
        p = {"bootloader": (0x1000, 25000), "partitions": (0x8000, 0xC00), "boot_app0": (0xE000, 0x2000),
             "app": (0x10000, 1700000)}
        p.update(over)
        return [(k, o, s) for k, (o, s) in p.items()]

    def test_the_four_parts_pass(self):
        R.check_layout(self.parts(), self.rows)

    def test_a_merged_image_at_zero_is_refused(self):
        with self.assertRaises(R.Fail):
            R.check_layout([("app", 0x0, 0x400000)], self.rows)
        with self.assertRaises(R.Fail):  # the app part placed at 0x0 with the others
            R.check_layout(self.parts(app=(0x0, 0x400000)), self.rows)

    def test_nothing_touches_nvs_or_the_kept_partitions(self):
        for over in ({"partitions": (0x8000, 0x1001)},          # one byte into NVS
                     {"bootloader": (0x1000, 0x8000)},          # over the table and into NVS
                     {"boot_app0": (0xE000, 0x2001)},           # past otadata into app0's start: room check
                     {"app": (0x10000, 0x1D0001)}):             # into app1
            with self.assertRaises(R.Fail, msg=str(over)):
                R.check_layout(self.parts(**over), self.rows)

    def test_wrong_offsets_and_sets(self):
        with self.assertRaises(R.Fail):
            R.check_layout(self.parts(app=(0x1E0000, 1000)), self.rows)  # the app written to app1
        with self.assertRaises(R.Fail):
            R.check_layout(self.parts()[:3], self.rows)                   # no app
        with self.assertRaises(R.Fail):
            R.check_layout(self.parts() + [("app", 0x10000, 10)], self.rows)

    def test_table_matches_the_csv(self):
        R.check_table(table_bin(CSV), self.rows)
        with self.assertRaises(R.Fail):
            R.check_table(table_bin(CSV.replace("0x46000", "0x45000")), self.rows)

    def test_marker(self):
        self.assertEqual(R.marker_version(image("1.0.0")), "1.0.0")
        with self.assertRaises(R.Fail):
            R.marker_version(b"no marker here")
        with self.assertRaises(R.Fail):
            R.marker_version(image("1.0.0") + image("1.0.0"))


class BodyTests(unittest.TestCase):
    def test_changelog_section(self):
        self.assertEqual(R.changelog_section(CHANGELOG, "1.0.0"), "### Added\n- first release")
        self.assertEqual(R.changelog_section(CHANGELOG, "1.0.1"), "- next")
        self.assertIsNone(R.changelog_section(CHANGELOG, "1.0.0-rc.1"))
        self.assertIsNone(R.changelog_section(CHANGELOG, "1.0"))

    def test_placeholders(self):
        vals = {"version": "1.0.0", "tag": "v1.0.0", "repo_url": "https://example.org/r", "changelog": "- x",
                "sha256sums": "00  a"}
        body = R.render_body(TEMPLATE, vals)
        self.assertIn("tag `v1.0.0`", body)
        self.assertNotIn("{{", body)
        with self.assertRaises(R.Fail):
            R.render_body(TEMPLATE + "{{unknown}}\n", vals)
        with self.assertRaises(R.Fail):
            R.render_body("no tag here THIRD_PARTY_NOTICES_FIRMWARE.txt\n", vals)
        with self.assertRaises(R.Fail):
            R.render_body("tag {{tag}} only\n", vals)


class AssembleTests(unittest.TestCase):
    V = "1.0.0"

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.parts = os.path.join(self.tmp, "parts")
        os.makedirs(os.path.join(self.parts, "diag"))
        files = {"fauxcloud.ino.bin": image(self.V), "fauxcloud.ino.elf": b"\x7fELF release",
                 "fauxcloud.ino.bootloader.bin": b"\xe9" * 2000, "fauxcloud.ino.partitions.bin": table_bin(CSV),
                 "boot_app0.bin": b"\xff" * 0x2000, "flash_args": FLASH_ARGS.encode(),
                 "diag/fauxcloud.ino.bin": image(self.V + "+diag"), "diag/fauxcloud.ino.elf": b"\x7fELF diag"}
        for rel, data in files.items():
            self.put(os.path.join(self.parts, rel), data)
        self.csv = self.put(os.path.join(self.tmp, "partitions.csv"), CSV.encode())
        self.notices = self.put(os.path.join(self.tmp, "THIRD_PARTY_NOTICES_FIRMWARE.txt"), b"notices\n")
        self.tpl = self.put(os.path.join(self.tmp, "RELEASE_TEMPLATE.md"), TEMPLATE.encode())
        self.log = self.put(os.path.join(self.tmp, "CHANGELOG.md"), CHANGELOG.encode())
        self.out = os.path.join(self.tmp, "out")

    @staticmethod
    def put(p, data):
        with open(p, "wb") as f:
            f.write(data)
        return p

    def assemble(self, *extra, version=None):
        v = version or self.V
        return run(["assemble", "--version", v, "--tag", "v" + v, "--parts", self.parts, "--out", self.out,
                    "--partitions", self.csv, "--notices", self.notices, "--template", self.tpl,
                    "--changelog", self.log] + list(extra))

    def test_assets_sums_zip_and_body(self):
        rc, out, err = self.assemble()
        self.assertEqual(rc, 0, err)
        assets = os.path.join(self.out, "assets")
        stem = "fauxcloud-" + self.V
        self.assertEqual(sorted(os.listdir(assets)),
                         sorted(["SHA256SUMS", "THIRD_PARTY_NOTICES_FIRMWARE.txt", stem + ".bin", stem + "-diag.bin",
                                 stem + "-factory.zip", stem + "-elf.zip"]))
        with open(os.path.join(assets, "SHA256SUMS")) as f:
            lines = f.read().splitlines()
        self.assertEqual(len(lines), 5)
        for ln in lines:
            digest, name = ln.split("  ")
            with open(os.path.join(assets, name), "rb") as f:
                self.assertEqual(hashlib.sha256(f.read()).hexdigest(), digest)
        with zipfile.ZipFile(os.path.join(assets, stem + "-factory.zip")) as z:
            names = z.namelist()
            man = json.loads(z.read(stem + "-factory/manifest.json"))
            fa = z.read(stem + "-factory/flash_args").decode()
            self.assertEqual(z.read(stem + "-factory/" + stem + ".bin"), image(self.V))
        self.assertNotIn("merged", " ".join(names))
        parts = man["builds"][0]["parts"]
        self.assertEqual([(p["path"], p["offset"]) for p in parts],
                         [(stem + "-bootloader.bin", 0x1000), (stem + "-partitions.bin", 0x8000),
                          (stem + "-boot_app0.bin", 0xE000), (stem + ".bin", 0x10000)])
        self.assertIn("0x10000 " + stem + ".bin", fa)
        with zipfile.ZipFile(os.path.join(assets, stem + "-elf.zip")) as z:
            self.assertEqual(sorted(z.namelist()), [stem + "-diag.elf", stem + ".elf"])
        with open(os.path.join(self.out, "body.md")) as f:
            body = f.read()
        self.assertIn("- first release", body)
        self.assertIn("tag `v1.0.0`", body)
        self.assertIn(lines[0], body)
        for sub in ("text", "bin", "elf"):
            self.assertTrue(os.listdir(os.path.join(self.out, "gate", sub)))

    def test_zip_is_dated_by_source_date_epoch(self):
        os.environ["SOURCE_DATE_EPOCH"] = "1795773600"  # 2026-11-27 10:00:00 UTC
        self.addCleanup(os.environ.pop, "SOURCE_DATE_EPOCH", None)
        self.assertEqual(self.assemble()[0], 0)
        with zipfile.ZipFile(os.path.join(self.out, "assets", "fauxcloud-1.0.0-factory.zip")) as z:
            self.assertEqual({i.date_time for i in z.infolist()}, {(2026, 11, 27, 10, 0, 0)})

    def test_refusals(self):
        cases = [
            ("fauxcloud.ino.bin", image("1.0.0-rc.1"), "app image says"),
            ("diag/fauxcloud.ino.bin", image(self.V), "diag image says"),
            ("fauxcloud.ino.partitions.bin", table_bin(CSV.replace("0x5000", "0x6000")), "differs"),
            ("flash_args", (FLASH_ARGS + "0x0 fauxcloud.ino.merged.bin\n").encode(), "outside the first-install"),
            ("flash_args", FLASH_ARGS.replace("0x10000 fauxcloud", "0x1E0000 fauxcloud").encode(), "expected"),
        ]
        for rel, data, words in cases:
            p = os.path.join(self.parts, rel)
            with open(p, "rb") as f:
                keep = f.read()
            self.put(p, data)
            rc, _, err = self.assemble()
            self.assertEqual(rc, 1, rel)
            self.assertIn(words, err)
            self.assertFalse(os.path.exists(self.out), "nothing is written before every check passed")
            self.put(p, keep)

    def test_a_dev_seeded_build_is_refused(self):
        # a build folder whose sketch/ copy holds secrets.h carries the developer's seed: never a release, never a preview
        os.makedirs(os.path.join(self.parts, "sketch"))
        self.put(os.path.join(self.parts, "sketch", "secrets.h"), b"")
        rc, _, err = self.assemble()
        self.assertEqual(rc, 1)
        self.assertIn("secrets.h", err)
        self.assertFalse(os.path.exists(self.out))
        with self.assertRaises(R.Fail) as e:
            R.factory_from_build(self.parts, self.csv)
        self.assertIn("without secrets.h", str(e.exception))
        os.remove(os.path.join(self.parts, "sketch", "secrets.h"))
        self.assertEqual(R.factory_from_build(self.parts, self.csv)[0], self.V)

    def test_missing_changelog_section_and_template(self):
        rc, _, err = self.assemble(version="1.0.1-rc.1")
        self.assertEqual(rc, 1)  # the images say 1.0.0 first
        os.remove(self.tpl)
        rc, _, err = self.assemble()
        self.assertEqual(rc, 1)
        self.assertIn("RELEASE_TEMPLATE.md", err)
        self.put(self.tpl, TEMPLATE.encode())
        self.put(self.log, b"# Changelog\n\n## [0.9.0]\n- old\n")
        rc, _, err = self.assemble()
        self.assertEqual(rc, 1)
        self.assertIn("no section for 1.0.0", err)

    def test_dry_run_takes_a_stand_in_changelog(self):
        # a tag release fails without its section; the manual dry run (--changelog-optional) uses a stand-in line
        self.put(self.log, b"# Changelog\n\n## [Unreleased]\n- next\n")
        self.assertEqual(self.assemble()[0], 1)
        self.assertFalse(os.path.exists(self.out))
        rc, _, err = self.assemble("--changelog-optional")
        self.assertEqual(rc, 0, err)
        self.assertIn("stand-in", err)
        with open(os.path.join(self.out, "body.md")) as f:
            self.assertIn(R.DRY_RUN_CHANGELOG % self.V, f.read())
        shutil.rmtree(self.out)
        os.remove(self.log)  # no CHANGELOG.md at all
        self.assertEqual(self.assemble()[0], 1)
        self.assertEqual(self.assemble("--changelog-optional")[0], 0)
        shutil.rmtree(self.out)
        self.put(self.log, CHANGELOG.encode())  # a real section always wins
        self.assertEqual(self.assemble("--changelog-optional")[0], 0)
        with open(os.path.join(self.out, "body.md")) as f:
            self.assertIn("- first release", f.read())

    def test_factory_readme_names_the_erase_box(self):
        self.assertEqual(self.assemble()[0], 0)
        with zipfile.ZipFile(os.path.join(self.out, "assets", "fauxcloud-1.0.0-factory.zip")) as z:
            readme = z.read("fauxcloud-1.0.0-factory/README.txt").decode()
        self.assertIn("unticked to keep the settings", readme)
        self.assertIn("parts.json", readme)

    def test_parts_json_pins_the_zip(self):
        # the installer reads parts.json only: each entry is a file of the zip with exactly that size and SHA-256, at
        # the offset flash_args and manifest.json give, in flash order, and the four parts are the only binaries
        self.assertEqual(self.assemble()[0], 0)
        stem = "fauxcloud-" + self.V
        with zipfile.ZipFile(os.path.join(self.out, "assets", stem + "-factory.zip")) as z:
            folder = stem + "-factory/"
            pj = json.loads(z.read(folder + "parts.json"))
            man = json.loads(z.read(folder + "manifest.json"))
            fa = z.read(folder + "flash_args").decode().splitlines()[1:]
            bins = sorted(n for n in z.namelist() if n.endswith(".bin"))
            for p in pj["parts"]:
                data = z.read(folder + p["name"])
                self.assertEqual((len(data), hashlib.sha256(data).hexdigest()), (p["size"], p["sha256"]), p["name"])
        self.assertEqual((pj["name"], pj["version"], pj["chip"]), ("fauxcloud", self.V, "ESP32"))
        self.assertEqual([p["role"] for p in pj["parts"]], ["bootloader", "partitions", "boot_app0", "app"])
        self.assertEqual([(p["name"], p["offset"]) for p in pj["parts"]],
                         [(m["path"], m["offset"]) for m in man["builds"][0]["parts"]])
        self.assertEqual(["0x%X %s" % (p["offset"], p["name"]) for p in pj["parts"]], fa)
        self.assertEqual(sorted(folder + p["name"] for p in pj["parts"]), bins)
        with open(os.path.join(self.out, "gate", "text", "parts.json")) as f:
            self.assertEqual(json.loads(f.read()), pj)

    def test_factory_from_a_build_folder(self):
        # build_site.py --local-firmware: the same zip from a development build folder, with the release's checks
        version, data = R.factory_from_build(self.parts, self.csv)
        self.assertEqual(version, self.V)
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            pj = json.loads(z.read("fauxcloud-1.0.0-factory/parts.json"))
        self.assertEqual([p["offset"] for p in pj["parts"]], [0x1000, 0x8000, 0xE000, 0x10000])
        self.put(os.path.join(self.parts, "fauxcloud.ino.partitions.bin"), table_bin(CSV.replace("0x5000", "0x6000")))
        with self.assertRaises(R.Fail):
            R.factory_from_build(self.parts, self.csv)
        self.put(os.path.join(self.parts, "fauxcloud.ino.partitions.bin"), table_bin(CSV))
        # a copy of the sketch built under another name (fc0.ino): the same zip, the release's part names
        other = os.path.join(self.tmp, "fc0-build")
        os.mkdir(other)
        for n in os.listdir(self.parts):
            src = os.path.join(self.parts, n)
            if os.path.isfile(src):
                with open(src, "rb") as f:
                    b = f.read()
                if n == "flash_args":
                    b = b.replace(b"fauxcloud.ino", b"fc0.ino")
                self.put(os.path.join(other, n.replace("fauxcloud.ino", "fc0.ino")), b)
        self.put(os.path.join(other, "fc0.ino.merged.bin"), b"not a part")
        self.assertEqual(R.factory_from_build(other, self.csv), (version, data))
        os.remove(os.path.join(self.parts, "boot_app0.bin"))
        with self.assertRaises(R.Fail):
            R.factory_from_build(self.parts, self.csv)


if __name__ == "__main__":
    unittest.main()
