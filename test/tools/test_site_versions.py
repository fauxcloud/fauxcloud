"""tools/site_versions.py -- versions.json for the installer from site/boards.json and a synthetic Releases API output
(site_releases_fixture.json): drafts, tags that are not versions and releases without the board's factory zip are left
out; newest first; a pre-release is marked; the SHA-256 comes from the API digest or the SHA256SUMS asset; --mirror
copies the checked zips and lists them at same-origin paths; no release at all gives the committed placeholder row."""
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import site_versions as V  # noqa: E402

with open(os.path.join(HERE, "site_releases_fixture.json"), encoding="utf-8") as _f:
    RELEASES = json.load(_f)
with open(os.path.join(ROOT, "site", "boards.json"), encoding="utf-8") as _f:
    BOARDS = json.load(_f)
SUMS_URL = "https://github.com/fauxcloud/fauxcloud/releases/download/v1.0.1/SHA256SUMS"
SUMS = ("8888888888888888888888888888888888888888888888888888888888888888  fauxcloud-1.0.1-factory.zip\n"
        "9999999999999999999999999999999999999999999999999999999999999999  fauxcloud-1.0.1.bin\n")


def fake_get(table):
    def get(url):
        if url not in table:
            raise AssertionError("unexpected download: " + url)
        return table[url]
    return get


class SiteVersions(unittest.TestCase):
    def test_versions_from_the_releases(self):
        doc = V.build(BOARDS, RELEASES, get=fake_get({SUMS_URL: SUMS.encode()}))
        b = doc["boards"]["esp32-wroom-32-4mb"]
        self.assertEqual([v["version"] for v in b["versions"]], ["1.1.0-rc.1", "1.0.1", "1.0.0"])
        self.assertEqual([v["prerelease"] for v in b["versions"]], [True, False, False])
        self.assertEqual([v["date"] for v in b["versions"]], ["2027-01-10", "2026-12-15", "2026-12-01"])
        self.assertEqual(b["versions"][0]["sha256"], "2" * 64)   # the API digest
        self.assertEqual(b["versions"][0]["size"], 1000001)      # the API size
        self.assertEqual(b["versions"][1]["sha256"], "8" * 64)   # no digest: the SHA256SUMS line
        self.assertTrue(b["versions"][2]["url"].endswith("/v1.0.0/fauxcloud-1.0.0-factory.zip"))
        for k in V.BOARD_KEYS:
            self.assertEqual(b[k], BOARDS["boards"]["esp32-wroom-32-4mb"][k])
        self.assertNotIn("asset", b)

    def test_keep(self):
        doc = V.build(BOARDS, RELEASES, get=fake_get({SUMS_URL: SUMS.encode()}), keep=2)
        self.assertEqual(len(doc["boards"]["esp32-wroom-32-4mb"]["versions"]), 2)
        # a version dropped by --keep is never hashed: 1.0.1 needs SHA256SUMS, and with keep=1 nothing is downloaded
        doc = V.build(BOARDS, RELEASES, get=fake_get({}), keep=1)
        self.assertEqual([v["version"] for v in doc["boards"]["esp32-wroom-32-4mb"]["versions"]], ["1.1.0-rc.1"])

    def test_same_day_releases_sort_by_semver(self):
        def rel(tag, pre=False):
            v = tag[1:]
            return {"tag_name": tag, "draft": False, "prerelease": pre, "published_at": "2027-03-01T10:00:00Z",
                    "assets": [{"name": "fauxcloud-%s-factory.zip" % v, "browser_download_url": "https://x.example/" + v,
                                "digest": "sha256:" + "1" * 64, "size": 10}]}
        doc = V.build(BOARDS, [rel("v0.1.9"), rel("v0.1.10"), rel("v0.1.10-rc.2", True), rel("v0.1.10-rc.10", True)])
        self.assertEqual([v["version"] for v in doc["boards"]["esp32-wroom-32-4mb"]["versions"]],
                         ["0.1.10", "0.1.10-rc.10", "0.1.10-rc.2", "0.1.9"])

    def test_a_release_asset_without_size_is_refused(self):
        rel = json.loads(json.dumps(RELEASES))
        for a in rel[0]["assets"]:
            a.pop("size")
        with self.assertRaises(V.Fail):
            V.build(BOARDS, rel, get=fake_get({SUMS_URL: SUMS.encode()}))

    def test_no_sha_is_refused(self):
        with self.assertRaises(V.Fail):
            V.build(BOARDS, RELEASES, get=fake_get({SUMS_URL: b"nothing\n"}))

    def test_no_release_gives_the_placeholder(self):
        doc = V.build(BOARDS, [])
        self.assertEqual(doc["boards"]["esp32-wroom-32-4mb"]["versions"], [V.PLACEHOLDER])
        with open(os.path.join(ROOT, "site", "firmware", "versions.json"), encoding="utf-8") as f:
            committed = json.load(f)
        self.assertEqual({k: v for k, v in committed.items() if k != "_note"}, {k: v for k, v in doc.items() if k != "_note"},
                         "the committed placeholder is what the tool writes with no release")

    def test_mirror_checks_and_copies(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        zips = {"1.1.0-rc.1": b"rc zip", "1.0.1": b"fix zip", "1.0.0": b"first zip"}
        rel = json.loads(json.dumps(RELEASES))
        table = {}
        for r in rel:
            for a in r["assets"]:
                v = r["tag_name"][1:]
                if a["name"] == "fauxcloud-%s-factory.zip" % v and v in zips:
                    a["digest"] = "sha256:" + hashlib.sha256(zips[v]).hexdigest()
                    a["size"] = len(zips[v])
                    table[a["browser_download_url"]] = zips[v]
        doc = V.build(BOARDS, rel, get=fake_get(table), mirror=tmp)
        rows = doc["boards"]["esp32-wroom-32-4mb"]["versions"]
        self.assertEqual([r["url"] for r in rows], ["/firmware/%s/fauxcloud-%s-factory.zip" % (v, v) for v in zips])
        for v, data in zips.items():
            with open(os.path.join(tmp, v, "fauxcloud-%s-factory.zip" % v), "rb") as f:
                self.assertEqual(f.read(), data)
        first = next(iter(table))
        table[first] = table[first][:-1] + b"X"   # same size, other bytes
        with self.assertRaises(V.Fail):
            V.build(BOARDS, rel, get=fake_get(table), mirror=tmp)
        table[first] = b"tampered, and longer than it was"
        with self.assertRaises(V.Fail):
            V.build(BOARDS, rel, get=fake_get(table), mirror=tmp)

    def test_bad_boards_are_refused(self):
        for bad in ({"boards": {}}, {"boards": {"Bad_Id": BOARDS["boards"]["esp32-wroom-32-4mb"]}},
                    {"boards": {"x": dict(BOARDS["boards"]["esp32-wroom-32-4mb"], asset="fauxcloud.bin")}}):
            with self.assertRaises(V.Fail):
                V.check_boards(bad)

    def test_cli(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        rel = os.path.join(tmp, "releases.json")
        with open(rel, "w") as f:
            json.dump([r for r in RELEASES if r["tag_name"] != "v1.0.1"], f)
        out = os.path.join(tmp, "versions.json")
        so, se = io.StringIO(), io.StringIO()
        with redirect_stdout(so), redirect_stderr(se):
            rc = V.main(["--releases", rel, "--out", out])
        self.assertEqual(rc, 0, se.getvalue())
        with open(out) as f:
            self.assertEqual([v["version"] for v in json.load(f)["boards"]["esp32-wroom-32-4mb"]["versions"]],
                             ["1.1.0-rc.1", "1.0.0"])


if __name__ == "__main__":
    unittest.main()
