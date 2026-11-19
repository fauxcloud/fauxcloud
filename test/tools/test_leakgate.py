"""tools/leakgate.py -- every rule bites on a positive and stays quiet on its negative, the manifest classifies with the
first matching line, the allow-list drops only covered false positives, the deny-list loads from a file and from the
environment and never prints what it matched, the image metadata check reads PNG / JPEG / WebP, and the exit codes
follow the result. Fixtures are synthetic and built at run time, so this file itself passes the gate."""
import base64
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stderr, redirect_stdout

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import leakgate as L  # noqa: E402

# the patterns the gate looks for, assembled so that this file does not contain them
NOTES = "docs" + "/" + "internal"
DROP = "_" + "inbox"
CAPT = "capt" + "ures/"
PL = "\u0105"
DNUM = "D" + "12"
TAG = "b" + "12.3"
OWNER = "own" + "er"
DATE = "-".join(["2026", "11", "05"])


def ip(*o):
    return ".".join(str(x) for x in o)


def mac(*o):
    return ":".join("%02x" % x for x in o)


def png(chunks=()):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)
    body = b"".join(chunk(t, d) for t, d in chunks)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + body + chunk(b"IDAT", zlib.compress(b"\x00\x00")) + \
        chunk(b"IEND", b"")


def run(argv, env=None, stdin=None):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = L.main(argv, env=env if env is not None else {}, stdin=stdin)
    return rc, out.getvalue(), err.getvalue()


def git(cwd, *args):
    subprocess.run(["git", "-C", cwd] + list(args), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Tree(unittest.TestCase):
    """A temp folder (not a git repository: the gate walks it) with a manifest that ships everything."""

    MANIFEST = "review tools/pending.py\nprivate notes/**\nship **\nbinary img/*.png\n"

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="lg-test-")
        self.write("manifest.txt", self.MANIFEST)

    def tearDown(self):
        L.rmtree(self.tmp)  # git's read-only objects on Windows too

    def write(self, rel, data):
        p = os.path.join(self.tmp, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data.encode("utf-8") if isinstance(data, str) else data)
        return p

    def gate(self, *extra, env=None):
        return run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--json"] + list(extra), env)

    def hits(self, *extra, env=None):
        rc, out, _ = self.gate(*extra, env=env)
        d = json.loads(out)
        return rc, {(h["file"], h["line"], h["class"]) for h in d["hits"]}, d

    def classes(self, rel, text):
        self.write(rel, text)
        _, hs, _ = self.hits()
        return {c for f, _l, c in hs if f == rel}


class ManifestTests(Tree):
    def test_first_match_wins(self):
        m = L.Manifest.parse("review a/x.py\nprivate a/**\nship **\n")
        self.assertEqual(m.classify("a/x.py"), "review")
        self.assertEqual(m.classify("a/y.py"), "private")
        self.assertEqual(m.classify("b/y.py"), "ship")

    def test_glob_semantics(self):
        m = L.Manifest.parse("ship docs/*.md\nship site/\nprivate tools/x.*\n")
        self.assertEqual(m.classify("docs/a.md"), "ship")
        self.assertEqual(m.classify("docs/sub/a.md"), "unclassified")  # `*` stays inside one folder
        self.assertEqual(m.classify("site/a/b/c.css"), "ship")           # a trailing slash takes the whole folder
        self.assertEqual(m.classify("tools/x.c"), "private")

    def test_unclassified_and_review_refuse(self):
        self.write("manifest.txt", "ship manifest.txt\nship ok.txt\nreview pending.txt\n")
        self.write("ok.txt", "fine\n")
        self.write("pending.txt", "fine\n")
        self.write("stray.txt", "fine\n")
        rc, out, _ = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks"])
        self.assertEqual(rc, 1)
        self.assertIn("stray.txt: unclassified", out)
        self.assertIn("pending.txt: review", out)
        self.assertIn("unclassified 1", out)

    def test_clean_tree_passes(self):
        self.write("a.md", "plain words\n")
        rc, out, _ = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks"])
        self.assertEqual(rc, 0, out)
        self.assertIn("result: PASS", out)

    def test_bad_manifest_line_is_an_error(self):
        self.write("manifest.txt", "publish everything\n")
        rc, _, err = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks"])
        self.assertEqual(rc, 2)
        self.assertIn("expected", err)

    def test_private_file_is_refused_in_the_published_view(self):
        self.write("manifest.txt", "private **/secrets.*\n" + L.DEV_ONLY_BEGIN + "\nprivate notes/**\n" + L.DEV_ONLY_END +
                   "\nship manifest.txt\nship src/**\n")
        self.write("src/a.c", "int a;\n")
        self.write("src/secrets.h", "#define K 1\n")
        self.write("notes/n.md", "x\n")
        rc, hs, d = self.hits()
        self.assertEqual(rc, 1)
        self.assertIn(("src/secrets.h", 0, "private-file"), hs)
        self.assertIn("notes/n.md", d["unclassified"])  # the dev-only lines do not apply in the published view
        rc, hs, d = self.hits("--source-is-private")
        self.assertEqual(rc, 0, d)  # the export's source tree: private files expected and skipped
        self.assertFalse(hs)

    def test_private_paths_refused_whatever_the_manifest_order(self):
        self.write("manifest.txt", "ship **\n")  # a manifest that ships everything first
        refused = ["firmware/secrets.h", "a/b.secret", DROP + "/n.md", CAPT + "x.log", NOTES + "/plan.md"]
        allowed = ["firmware/secrets.h.example", "test/" + CAPT + "console/log.txt", "docs/api.md"]
        for rel in refused + allowed:
            self.write(rel, "plain\n")
        rc, hs, _ = self.hits()
        self.assertEqual(rc, 1)
        for rel in refused:
            self.assertIn((rel, 0, "private-file"), hs)
        for rel in allowed:
            self.assertNotIn((rel, 0, "private-file"), hs)
        rc, hs, d = self.hits("--source-is-private")  # the export's source: the manifest decides alone
        self.assertFalse([h for h in hs if h[2] == "private-file"], d)

    def test_glob_negated_class(self):
        m = L.Manifest.parse("ship a/[!x]*.c\n")
        self.assertEqual(m.classify("a/b.c"), "ship")
        self.assertEqual(m.classify("a/x.c"), "unclassified")

    def test_allow_line_needs_a_path_except_for_tags(self):
        with self.assertRaises(ValueError):
            L.AllowList.parse("dnum " + DNUM + "\n")
        self.assertEqual(len(L.AllowList.parse("tag b1\n").rules), 1)

    def test_dev_only_block_is_not_published(self):
        text = "ship **\n" + L.DEV_ONLY_BEGIN + "\nprivate " + NOTES + "/**\n" + L.DEV_ONLY_END + "\n"
        self.assertNotIn(NOTES, L.strip_dev_only(text))
        self.write("manifest.txt", text)
        _, hs, _ = self.hits()
        self.assertFalse([h for h in hs if h[0] == "manifest.txt"])  # the gate scans the published view


class RuleTests(Tree):
    def test_private_paths(self):
        self.assertIn("path", self.classes("a.md", "see " + NOTES + "/plan.md\n"))
        self.assertIn("path", self.classes("b.py", "x = 1  # the " + DROP + " notes\n"))
        self.assertIn("path", self.classes("c.ps1", "# logs go to " + CAPT + "x.log\n"))
        self.assertIn("path", self.classes("d.md", "the " + "brie" + "fs/x.md note\n"))
        self.assertEqual(set(), self.classes("e.md", "documentation, internal notes; the console captures data; "
                                                    "test/" + CAPT + "yt/ and a URL /research/x\n"))

    def test_secrets_files(self):
        self.assertIn("path", self.classes("a.md", "keys in secrets" + ".forwarding.json\n"))
        self.assertEqual(set(), self.classes("b.md", "copy secrets" + ".h.example to secrets" + ".h; secrets.*\n"))

    def test_decision_numbers_in_comments_and_strings(self):
        self.assertIn("dnum", self.classes("a.c", "int x;  // as decided in " + DNUM + "\n"))
        self.assertIn("dnum", self.classes("b.c", 'static const char* m = "' + DNUM + ': held";\n'))
        self.assertEqual(set(), self.classes("c.c", "int " + DNUM + " = 0;\n"))  # a code token
        self.assertEqual(set(), self.classes("d.json", '{"k": "' + DNUM + '"}\n'))  # data

    def test_allow_list_drops_only_the_covered_span(self):
        self.write("allow.txt", "dnum \\bD1 mini\\b @ *.md\n")
        self.write("a.md", "a D" + "1 mini board\n")
        self.write("b.md", "a D" + "1 decision\n")
        _, hs, _ = self.hits("--allow", os.path.join(self.tmp, "allow.txt"))
        self.assertNotIn(("a.md", 1, "dnum"), hs)
        self.assertIn(("b.md", 1, "dnum"), hs)

    def test_tags_report_only_unless_strict(self):
        self.write("a.py", "x = 1  # since " + TAG + "\n")
        rc, hs, d = self.hits()
        self.assertIn(("a.py", 1, "tag"), hs)
        self.assertEqual(rc, 0)
        self.assertTrue(d["ok"])
        rc, _, _ = self.hits("--strict-tags")
        self.assertEqual(rc, 1)

    def test_tags_not_in_string_literals(self):
        self.assertEqual(set(), self.classes("a.js", "const v = '0.1.0-" + TAG + "';\n"))

    def test_owner_note_needs_a_date(self):
        self.assertIn("owner-note", self.classes("a.py", "# the " + OWNER + " asked, " + DATE + "\n"))
        self.assertEqual(set(), self.classes("b.py", "# the " + OWNER + " of the board\n"))

    def test_polish_outside_the_data_folders(self):
        self.assertIn("polish", self.classes("a.c", "// m" + PL + "ka\n"))
        self.assertEqual(set(), self.classes("firmware/fauxcloud/web/data/pl.json", '{"k": "m' + PL + 'ka"}\n'))
        self.assertEqual(set(), self.classes("docs/i18n/pl.json", '{"k": "m' + PL + 'ka"}\n'))

    def test_lan_addresses(self):
        quiet = " ".join([ip(192, 168, 1, 5), ip(192, 168, 77, 1), ip(192, 168, 1) + ".x", ip(192, 0, 2, 9),
                          "version " + ip(10, 2, 3), ip(172, 32, 1, 1)])
        self.assertEqual(set(), self.classes("a.md", quiet + "\n"))
        self.assertIn("lan", self.classes("b.md", "at " + ip(10, 1, 2, 3) + "\n"))
        self.assertIn("lan", self.classes("c.md", "net " + ip(192, 168, 5) + ".x\n"))
        self.assertIn("lan", self.classes("d.md", "host " + ip(172, 20, 1, 1) + "\n"))

    def test_mac_addresses(self):
        self.assertEqual(set(), self.classes("a.md", "ex " + mac(2, 0, 0, 0x12, 0x34, 0x56) + " and "
                                             + "-".join(["65", "54", "10", "35", "72", "94"]) + "\n"))
        self.assertIn("mac", self.classes("b.md", "dev " + mac(0x24, 0x0A, 0xC4, 1, 2, 3) + "\n"))

    def test_coordinates(self):
        self.assertIn("coords", self.classes("a.json", '{"lat": ' + "51.12" + ', "lon": 7.5}\n'))
        self.assertIn("coords", self.classes("b.md", "at " + "51.1234" + ", " + "17.0321" + "\n"))
        self.assertEqual(set(), self.classes("c.md", "gain 0.25, 1.5 and lat 50.0 (a made-up point)\n"))

    def test_home_paths(self):
        self.assertIn("home-path", self.classes("a.md", "C:" + "\\" + "Users" + "\\" + "someone\\x\n"))
        self.assertIn("home-path", self.classes("b.sh", "cd /ho" + "me/someone/x\n"))
        self.assertEqual(set(), self.classes("c.sh", "cd /ho" + "me/runner/work\n"))

    def test_python_docstring_is_a_comment_and_hash_in_string_is_not(self):
        text = 'def f():\n    """As in ' + DNUM + '."""\n    s = "#' + TAG + '"\n'
        mask = L.scope_mask("hash", text).split("\n")
        self.assertIn(DNUM, mask[1])
        self.assertNotIn(TAG, mask[2])


class BinaryTests(Tree):
    def test_binary_outside_the_listed_kinds(self):
        self.write("img/ok.png", png())
        self.write("other/x.png", png())
        self.write("blob.dat", b"\x00\x01\x02")
        _, hs, _ = self.hits()
        self.assertNotIn(("img/ok.png", 0, "binary"), hs)
        self.assertIn(("other/x.png", 0, "binary"), hs)
        self.assertIn(("blob.dat", 0, "binary"), hs)

    def test_png_metadata(self):
        self.assertFalse(L.image_metadata(png()))
        self.assertTrue(L.image_metadata(png([(b"tEXt", b"Author\x00someone")])))
        self.assertTrue(L.image_metadata(png([(b"eXIf", b"MM\x00*")])))
        self.write("img/meta.png", png([(b"iTXt", b"XML:com.adobe.xmp\x00\x00\x00\x00\x00<x/>")]))
        _, hs, _ = self.hits()
        self.assertIn(("img/meta.png", 0, "metadata"), hs)

    def test_ico_with_a_png_inside(self):
        def ico(img):
            head = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", 1, 1, 0, 0, 1, 32, len(img), 22)
            return head + img
        self.assertFalse(L.image_metadata(ico(png())))
        self.assertTrue(L.image_metadata(ico(png([(b"tEXt", b"Author" + bytes([0]) + b"someone")]))))

    def test_jpeg_and_webp_metadata(self):
        exif = b"Exif\x00\x00MM"
        jpg = b"\xff\xd8" + b"\xff\xe1" + struct.pack(">H", len(exif) + 2) + exif + b"\xff\xda\x00\x02" + b"\xff\xd9"
        clean = b"\xff\xd8" + b"\xff\xe0" + struct.pack(">H", 7) + b"JFIF\x00" + b"\xff\xda\x00\x02\xff\xd9"
        self.assertTrue(L.image_metadata(jpg))
        self.assertFalse(L.image_metadata(clean))
        body = b"WEBP" + b"VP8L" + struct.pack("<I", 2) + b"\x00\x00" + b"EXIF" + struct.pack("<I", 2) + b"MM"
        webp = b"RIFF" + struct.pack("<I", len(body)) + body
        self.assertTrue(L.image_metadata(webp))
        self.assertFalse(L.image_metadata(webp[:4] + struct.pack("<I", 14) + body[:14]))

    def test_tree_and_bin_strings(self):
        site = os.path.join(self.tmp, "site")
        self.write("site/index.html", "<p>see " + NOTES + "</p>\n")
        self.write("site/a.png", png())
        binp = self.write("fw.bin", b"\x00\x01" + b"build tag " + DNUM.encode() + b" here\x00\xff")
        rc, out, _ = run(["--tree", site, "--bin", binp, "--no-gitleaks", "--root", self.tmp])
        self.assertEqual(rc, 1)
        self.assertIn("index.html:1: path", out)
        self.assertNotIn("a.png", out)
        self.assertIn("fw.bin:1: dnum", out)

    def test_bin_skip_drops_only_the_named_classes(self):
        # a release image: compressed assets read as noise for dnum / tag / polish; every other class still bites
        noise = ("x " + DNUM + " " + TAG + " " + PL + "xx").encode("utf-8")
        binp = self.write("fw.bin", b"\x00" + noise + b"\x00" + ("gw " + ip(10, 1, 2, 3)).encode() + b"\x00")
        rc, out, _ = run(["--bin", binp, "--bin-skip", "dnum,tag,polish", "--strict-tags", "--no-gitleaks",
                          "--root", self.tmp])
        self.assertEqual(rc, 1)
        self.assertIn("fw.bin:2: lan", out)
        for cls in ("dnum", "tag", "polish"):
            self.assertNotIn(": " + cls, out)
        clean = self.write("ok.bin", b"\x00" + noise + b"\x00")
        rc, _, _ = run(["--bin", clean, "--bin-skip", "dnum,tag,polish", "--strict-tags", "--no-gitleaks",
                        "--root", self.tmp])
        self.assertEqual(rc, 0)
        rc, _, err = run(["--bin", clean, "--bin-skip", "lan", "--no-gitleaks", "--root", self.tmp])
        self.assertEqual(rc, 2)
        self.assertIn("--bin-skip", err)


class DenyListTests(Tree):
    SECRET = "Hidden" + "Garden" + "Net"

    def deny_text(self):
        return ("# test deny-list\n@category ssid\n" + self.SECRET.lower() + "\n"
                "@category airport\nw:ZZQQ\n@category lan\nre:" + "192" + r"\.168\.4[0-9]\." + "\n")

    def test_deny_from_file_never_prints_the_text(self):
        deny = self.write("deny.txt", self.deny_text())
        self.write("a.md", "join " + self.SECRET + " now\nnear ZZQQ\nnot zzqqx\n")
        self.write("b.c", "// gw " + ip(192, 168, 42, 1) + "\n")
        rc, out, _ = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--deny", deny])
        self.assertEqual(rc, 1)
        self.assertIn("a.md:1: deny:ssid", out)
        self.assertIn("a.md:2: deny:airport", out)
        self.assertNotIn("a.md:3:", out)  # whole word only
        self.assertIn("b.c:1: deny:lan", out)
        for secret in (self.SECRET, self.SECRET.lower(), "ZZQQ", "42"):
            self.assertNotIn(secret, out)

    def test_deny_from_env_base64(self):
        self.write("a.md", "join " + self.SECRET + "\n")
        env = {"LEAKGATE_DENY": base64.b64encode(self.deny_text().encode()).decode()}
        rc, hs, d = self.hits(env=env)
        self.assertEqual(rc, 1)
        self.assertIn(("a.md", 1, "deny:ssid"), hs)
        self.assertTrue(d["deny"].startswith("env"))

    def test_deny_matches_file_names_and_never_prints_them(self):
        self.write("x-" + self.SECRET + ".txt", "see " + NOTES + "\n")
        self.write("manifest.txt", "ship manifest.txt\nship deny.txt\nship x-*.txt\n")
        self.write("y-" + self.SECRET + ".md", "plain\n")  # unclassified: its listing is redacted too
        deny = self.write("deny.txt", self.deny_text())
        for fmt in ([], ["--json"], ["--github"]):
            rc, out, _ = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--deny", deny] + fmt)
            self.assertEqual(rc, 1)
            self.assertNotIn(self.SECRET, out)
            self.assertNotIn(self.SECRET.lower(), out.lower())
            self.assertIn("<denied-name #", out)
        _, hs, _ = self.hits("--deny", deny)
        self.assertIn(("<denied-name #1>", 0, "deny:ssid"), hs)
        self.assertIn(("<denied-name #1>", 1, "path"), hs)  # the file's other hits carry the placeholder

    def test_deny_file_with_bom(self):
        deny = self.write("deny.txt", b"\xef\xbb\xbf" + ("w:" + self.SECRET + "\n").encode())
        self.write("a.md", "join " + self.SECRET + "\n")
        _, hs, _ = self.hits("--deny", deny)
        self.assertIn(("a.md", 1, "deny:other"), hs)
        env = {"LEAKGATE_DENY": base64.b64encode(b"\xef\xbb\xbf" + ("w:" + self.SECRET + "\n").encode()).decode()}
        _, hs, _ = self.hits(env=env)
        self.assertIn(("a.md", 1, "deny:other"), hs)

    def test_bin_short_literal_and_raw_bytes(self):
        nul = bytes([0])
        binp = self.write("fw.bin", nul + b"ZZQQ" + nul + bytes([255]) * 3)
        deny = self.write("deny.txt", self.deny_text())
        rc, out, _ = run(["--bin", binp, "--no-gitleaks", "--root", self.tmp, "--deny", deny])
        self.assertEqual(rc, 1)
        self.assertIn("fw.bin:", out)
        self.assertIn("deny:airport", out)
        self.assertNotIn("ZZQQ", out)
        # --bin-skip never touches the deny-list
        rc, out, _ = run(["--bin", binp, "--bin-skip", "dnum,tag,polish", "--no-gitleaks", "--root", self.tmp,
                          "--deny", deny])
        self.assertEqual(rc, 1)
        self.assertIn("deny:airport", out)
        # a word inside a binary asset's raw bytes (a private PNG chunk, not a text chunk)
        self.write("img/p.png", png([(b"prVt", b"x" + self.SECRET.encode() + b"x")]))
        _, hs, _ = self.hits("--deny", deny)
        self.assertIn(("img/p.png", 0, "deny:ssid"), hs)
        self.assertNotIn(("img/p.png", 0, "metadata"), hs)

    def test_bad_regex_is_reported_without_the_pattern(self):
        deny = self.write("deny.txt", "re:(unclosed" + self.SECRET + "\n")
        rc, out, err = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--deny", deny])
        self.assertEqual(rc, 2)
        self.assertNotIn(self.SECRET, out + err)
        self.assertIn("line 1", err)

    def test_deny_hits_cannot_be_allow_listed(self):
        with self.assertRaises(ValueError):
            L.AllowList.parse("deny:ssid .+\n")


class GitBase(Tree):
    """A temp git repository and the helpers of the push tests."""

    def setUp(self):
        super().setUp()
        if not shutil.which("git"):
            self.skipTest("git not on PATH")
        git(self.tmp, "init", "-q")
        git(self.tmp, "config", "user.name", "test")
        git(self.tmp, "config", "user.email", "test@example.invalid")
        git(self.tmp, "config", "commit.gpgsign", "false")

    def commit_all(self, msg):
        git(self.tmp, "add", "-A")
        git(self.tmp, "commit", "-q", "-m", msg)
        return self.sha("HEAD")

    def sha(self, rev):
        return subprocess.run(["git", "-C", self.tmp, "rev-parse", rev], stdout=subprocess.PIPE,
                              check=True).stdout.decode().strip()

    def outside(self, name, text):
        d = tempfile.mkdtemp(prefix="lg-out-")
        self.addCleanup(L.rmtree, d)
        p = os.path.join(d, name)
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
        return p

    def bare_remote(self):
        bare = tempfile.mkdtemp(prefix="lg-bare-")
        self.addCleanup(L.rmtree, bare)
        subprocess.run(["git", "init", "-q", "--bare", bare], check=True, stdout=subprocess.DEVNULL)
        git(self.tmp, "remote", "add", "origin", bare)
        return bare

    SECRET = "Hidden" + "Garden" + "Net"


class GitTests(GitBase):
    def test_rev_scans_the_commit_not_the_working_tree(self):
        self.write("a.md", "clean words\n")
        git(self.tmp, "add", "-A")
        git(self.tmp, "commit", "-q", "-m", "first")
        self.write("a.md", "see " + NOTES + "\n")  # an uncommitted change
        rc, out, _ = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--rev", "HEAD"])
        self.assertEqual(rc, 0, out)
        rc, out, _ = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks"])
        self.assertEqual(rc, 1)

    def test_commit_messages(self):
        self.write("a.md", "clean words\n")
        git(self.tmp, "add", "-A")
        git(self.tmp, "commit", "-q", "-m", "fix the clock\n\nas decided in " + DNUM)
        rc, out, _ = run(["--root", self.tmp, "--no-gitleaks", "--messages", "HEAD"])
        self.assertEqual(rc, 1)
        self.assertRegex(out, r"commit:[0-9a-f]{12}:3: dnum")

    def test_install_hook(self):
        deny = self.write("deny.txt", "# empty\n")
        rc, _, err = run(["--root", self.tmp, "--install-hook"])
        self.assertEqual(rc, 2)
        self.assertIn("--deny", err)
        rc, out, _ = run(["--root", self.tmp, "--install-hook", "--deny", deny])
        self.assertEqual(rc, 0, out)
        hook = os.path.join(self.tmp, ".git", "hooks", "pre-push")
        with open(hook, encoding="utf-8") as f:
            body = f.read()
        self.assertTrue(body.startswith("#!/bin/sh"))
        self.assertIn("--pre-push", body)
        self.assertIn("--strict-tags", body)  # a block tag refuses the push too
        self.assertIn(os.path.abspath(deny).replace("\\", "/"), body)  # stored absolute
        rc, _, err = run(["--root", self.tmp, "--install-hook", "--deny", deny])
        self.assertEqual(rc, 2)  # an existing hook is kept without --force
        rc, _, _ = run(["--root", self.tmp, "--install-hook", "--deny", deny, "--force"])
        self.assertEqual(rc, 0)

    def test_pre_push_scans_every_pushed_commit(self):
        deny = self.outside("deny.txt", "@category ssid\nw:" + self.SECRET + "\n")
        self.bare_remote()
        self.write("a.md", "clean\n")
        c0 = self.commit_all("base")
        git(self.tmp, "push", "-q", "origin", "HEAD:refs/heads/main")
        self.write("a.md", "join " + self.SECRET + "\n")
        c1 = self.commit_all("leak")
        self.write("a.md", "clean again\n")
        c2 = self.commit_all("unleak")
        base = ["--root", self.tmp, "--manifest", "manifest.txt", "--deny", deny, "--no-gitleaks"]
        rc, _, _ = run(base + ["--rev", c2])
        self.assertEqual(rc, 0)  # the tip alone is clean: a tip-only check would let the push through
        rc, out, _ = run(base + ["--pre-push", "origin"],
                         stdin=io.StringIO("refs/heads/main %s refs/heads/main %s\n" % (c2, c0)))
        self.assertEqual(rc, 1, out)
        self.assertIn("a.md@%s:1: deny:ssid" % c1[:12], out)
        self.assertNotIn(self.SECRET, out)
        # a new branch: every commit that no remote ref has
        rc, out, _ = run(base + ["--pre-push", "origin"],
                         stdin=io.StringIO("refs/heads/topic %s refs/heads/topic %s\n" % (c2, "0" * 40)))
        self.assertEqual(rc, 1)
        # nothing new: passes
        rc, out, _ = run(base + ["--pre-push", "origin"],
                         stdin=io.StringIO("refs/heads/main %s refs/heads/main %s\n" % (c0, c0)))
        self.assertEqual(rc, 0, out)

    def test_pre_push_messages_and_annotated_tags(self):
        deny = self.outside("deny.txt", "@category ssid\nw:" + self.SECRET + "\n")
        self.bare_remote()
        self.write("a.md", "clean\n")
        c0 = self.commit_all("base")
        git(self.tmp, "push", "-q", "origin", "HEAD:refs/heads/main")
        self.write("a.md", "clean too\n")
        c1 = self.commit_all("tuned on " + self.SECRET)
        base = ["--root", self.tmp, "--manifest", "manifest.txt", "--deny", deny, "--no-gitleaks", "--pre-push", "origin"]
        rc, out, _ = run(base, stdin=io.StringIO("refs/heads/main %s refs/heads/main %s\n" % (c1, c0)))
        self.assertEqual(rc, 1)
        self.assertIn("commit:%s:1: deny:ssid" % c1[:12], out)
        git(self.tmp, "commit", "-q", "--amend", "-m", "tuned")
        git(self.tmp, "tag", "-a", "v1", "-m", "release for " + self.SECRET)
        rc, out, _ = run(base, stdin=io.StringIO("refs/tags/v1 %s refs/tags/v1 %s\n" % (self.sha("v1"), "0" * 40)))
        self.assertEqual(rc, 1)
        self.assertIn("tag:refs/tags/v1:", out)
        self.assertNotIn(self.SECRET, out)

    def test_pre_push_redacts_ref_names(self):
        deny = self.outside("deny.txt", "@category ssid\nw:" + self.SECRET + "\n")
        self.bare_remote()
        self.write("a.md", "clean\n")
        c0 = self.commit_all("base")
        git(self.tmp, "tag", "-a", "v-" + self.SECRET, "-m", "a release")
        tag = self.sha("v-" + self.SECRET)
        base = ["--root", self.tmp, "--manifest", "manifest.txt", "--deny", deny, "--no-gitleaks", "--pre-push", "origin"]
        lines = ("refs/heads/x-%s %s refs/heads/x-%s %s\n" % (self.SECRET, c0, self.SECRET, "0" * 40) +
                 "refs/tags/v-%s %s refs/tags/v-%s %s\n" % (self.SECRET, tag, self.SECRET, "0" * 40))
        for fmt in ([], ["--json"]):
            rc, out, _ = run(base + fmt, stdin=io.StringIO(lines))
            self.assertEqual(rc, 1)  # a deny-matching ref name is a hit
            self.assertNotIn(self.SECRET, out)
            self.assertNotIn(self.SECRET.lower(), out.lower())
            self.assertIn("<denied-name #", out)

    def test_hook_refuses_a_real_push(self):
        """The installed hook end to end: git push to a local bare repository, leak added and removed again."""
        if not shutil.which("sh") and os.name != "nt":
            self.skipTest("no sh")
        deny = self.outside("deny.txt", "@category ssid\nw:" + self.SECRET + "\n")
        bare = self.bare_remote()
        os.makedirs(os.path.join(self.tmp, "tools"))
        shutil.copyfile(os.path.join(ROOT, "tools", "leakgate.py"), os.path.join(self.tmp, "tools", "leakgate.py"))
        self.write("a.md", "clean\n")
        self.commit_all("base")
        rc, out, _ = run(["--root", self.tmp, "--install-hook", "--deny", deny, "--manifest", "manifest.txt"])
        self.assertEqual(rc, 0, out)
        env = dict(os.environ, LEAKGATE_DENY="")
        push = ["git", "-C", self.tmp, "push", "-q", "origin", "HEAD:refs/heads/main"]
        r = subprocess.run(push, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.assertEqual(r.returncode, 0, r.stderr.decode(errors="replace")[-800:])
        self.write("a.md", "join " + self.SECRET + "\n")
        self.commit_all("leak")
        self.write("a.md", "clean again\n")
        tip = self.commit_all("unleak")
        r = subprocess.run(push, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn(self.SECRET, (r.stdout + r.stderr).decode(errors="replace"))
        remote_tip = subprocess.run(["git", "-C", bare, "rev-parse", "refs/heads/main"], stdout=subprocess.PIPE,
                                    check=True).stdout.decode().strip()
        self.assertNotEqual(remote_tip, tip)

    def test_rev_reads_raw_blobs(self):
        git(self.tmp, "config", "core.autocrlf", "true")
        self.write(".gitattributes", "hidden.md export-ignore\n")
        self.write("a.md", "line one\nline two\n")
        self.write("hidden.md", "see " + NOTES + "\n")
        self.commit_all("lf")
        blobs, special, _ids = L.read_tree(self.tmp, "HEAD")
        self.assertEqual(blobs["a.md"], b"line one\nline two\n")  # no CRLF from a checkout or an archive
        rc, out, _ = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--rev", "HEAD"])
        self.assertEqual(rc, 1)
        self.assertIn("hidden.md:1: path", out)  # export-ignore hides nothing from the gate

    def test_rev_refuses_symlinks(self):
        self.write("a.md", "clean\n")
        git(self.tmp, "add", "-A")
        blob = subprocess.run(["git", "-C", self.tmp, "hash-object", "-w", "--stdin"], input=b"a.md",
                              stdout=subprocess.PIPE, check=True).stdout.decode().strip()
        git(self.tmp, "update-index", "--add", "--cacheinfo", "120000,%s,link.md" % blob)
        git(self.tmp, "commit", "-q", "-m", "link")
        rc, out, _ = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--rev", "HEAD"])
        self.assertEqual(rc, 1)
        self.assertIn("link.md:0: special-entry", out)


class OutputTests(Tree):
    def test_json_shape(self):
        self.write("a.md", "see " + NOTES + "\n")
        self.write("tools/pending.py", "x = 1\n")
        rc, hs, d = self.hits()
        self.assertEqual(rc, 1)
        self.assertFalse(d["ok"])
        self.assertEqual(d["review"], ["tools/pending.py"])
        self.assertEqual(d["counts"].get("path"), 1)
        self.assertEqual(set(d["hits"][0]), {"file", "line", "class"})

    def test_gitleaks_report_parsing(self):
        rep = [{"File": "/x/src/a.py", "StartLine": 3, "RuleID": "generic-api-key", "Secret": "REDACTED"},
               {"File": "/x/src/notes/b.md", "StartLine": 1, "RuleID": "aws"}]
        got = L.parse_gitleaks_report(rep, "/x/src", keep={"a.py"})
        self.assertEqual(got, [("a.py", 3, "gitleaks:generic-api-key")])

    def test_text_output_lines(self):
        self.write("a.md", "ok\nsee " + NOTES + "\n")
        rc, out, _ = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks"])
        self.assertEqual(rc, 1)
        self.assertIn("a.md:2: path\n", out)
        self.assertNotIn(NOTES, out)


# --- secrets (--deny-secrets) and short patterns vs binaries --------------------------------------------------------
# made-up values, assembled at run time
SEC_SSID = "Quartz" + "Lantern" + "Seven"
SEC_PASS = "Mango" + '"' + "Tide/42&x"          # an escaped quote; its URL-encoded form differs
SEC_KEY = "Back" + chr(92) + "slash" + "Key99"   # an escaped backslash; 15 bytes = five whole base64 groups
SEC_NOTE = "two" + chr(10) + "lines-here"
SEC_SHORT = "short7" + "x"                       # 7 characters: below the floor
PLACEHOLDER = "change-me-" + "8+chars"
BS = chr(92)
NULB = bytes([0])


def c_escape(v):
    return v.replace(BS, BS + BS).replace('"', BS + '"').replace(chr(10), BS + "n")


def secrets_fixture():
    return "\n".join([
        "#pragma once",
        '#define STA_SSID "%s"        // the home network' % c_escape(SEC_SSID),
        '#define STA_PASS "%s"' % c_escape(SEC_PASS),
        '#  define WU_KEY "%s" /* a key */' % c_escape(SEC_KEY),
        '#define NOTE_TXT "%s"' % c_escape(SEC_NOTE),
        '#define AP_PASS "%s"     // a placeholder of the example' % PLACEHOLDER,
        '#define ADMIN_PASS "%s"' % SEC_SHORT,
        '#define MQTT_HOST "localhost"',
        "#define MQTT_PORT 1883",
        '#define MQTT_USER ""',
        "// #define OLD_PASS is a comment line",
        "",
    ])


def every_form(v):
    raw = v.encode()
    return [v, L.urllib.parse.quote(v, safe=""), base64.b64encode(raw[:len(raw) - len(raw) % 3]).decode()]


def jpeg(segments=(), pixels=bytes([0, 0x11, 0x22]), between=None):
    """A minimal JPEG; `between` (segments) adds a second scan after them, as a progressive JPEG has."""
    def seg(marker, body):
        return bytes([0xFF, marker]) + struct.pack(">H", len(body) + 2) + body
    head = seg(0xE0, b"JFIF" + bytes([0, 1, 1, 0, 0, 1, 0, 1, 0, 0]))
    sos = seg(0xDA, bytes([1, 1, 0, 0, 0x3F, 0]))
    out = bytes([0xFF, 0xD8]) + head + b"".join(seg(m, b) for m, b in segments) + sos + pixels
    if between is not None:
        out += b"".join(seg(m, b) for m, b in between) + sos + pixels
    return out + bytes([0xFF, 0xD9])


def webp(chunks):
    body = b"WEBP" + b"".join(c + struct.pack("<I", len(d)) + d + (NULB if len(d) & 1 else b"") for c, d in chunks)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def gif(extensions=(), pixels=NULB):
    out = b"GIF89a" + struct.pack("<HHBBB", 1, 1, 0, 0, 0)
    for label, body in extensions:
        out += bytes([0x21, label, len(body)]) + body + NULB
    out += bytes([0x2C]) + struct.pack("<HHHHB", 0, 0, 1, 1, 0) + bytes([2, len(pixels)]) + pixels + NULB
    return out + bytes([0x3B])


class SecretsTests(Tree):
    def secrets(self, folder="fw", example=True, text=None):
        p = self.write(folder + "/secrets.h", text if text is not None else secrets_fixture())
        if example:
            self.write(folder + "/secrets.h.example", '#define AP_PASS "%s"\n#define STA_SSID "your-home-wifi"\n'
                       % PLACEHOLDER)
        return p

    def test_values_floor_skip_list_and_unescaping(self):
        got = dict(L.read_secrets(self.secrets()))
        self.assertEqual(got, {"STA_SSID": SEC_SSID, "STA_PASS": SEC_PASS, "WU_KEY": SEC_KEY, "NOTE_TXT": SEC_NOTE})
        # no example next to the file: the built-in copy of the placeholders
        got = dict(L.read_secrets(self.secrets("other", example=False)))
        self.assertNotIn("AP_PASS", got)
        self.assertIn("STA_SSID", got)

    def test_hits_name_the_define_and_never_print_a_value(self):
        sec = self.secrets()
        self.write("manifest.txt", "ship manifest.txt\nship a.md\nship b.md\nship c.md\nship img/**\nbinary img/*\n")
        self.write("a.md", "join " + SEC_SSID.lower() + "\n")
        self.write("b.md", "url https://example.invalid/up?pw=" + L.urllib.parse.quote(SEC_PASS, safe="") + "\n")
        self.write("c.md", "auth " + base64.b64encode(SEC_KEY.encode() + b"tail").decode() + "\n")
        self.write("img/p.png", png([(b"prVt", NULB + SEC_SSID.encode() + NULB)]))
        fd, summary = tempfile.mkstemp(suffix=".md")
        os.close(fd)
        self.addCleanup(os.unlink, summary)
        argv = ["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--deny-secrets", sec]
        for fmt in ([], ["--json"], ["--github"]):
            rc, out, err = run(argv + fmt, env={"GITHUB_STEP_SUMMARY": summary})
            self.assertEqual(rc, 1)
            with open(summary, encoding="utf-8") as f:
                text = (out + err + f.read()).lower()
            for v in (SEC_SSID, SEC_PASS, SEC_KEY, SEC_NOTE):
                for form in every_form(v):
                    self.assertNotIn(form.lower(), text)
        _, hs, d = self.hits("--deny-secrets", sec)
        self.assertIn(("a.md", 1, "deny:secrets:STA_SSID"), hs)
        self.assertIn(("b.md", 1, "deny:secrets:STA_PASS"), hs)
        self.assertIn(("c.md", 1, "deny:secrets:WU_KEY"), hs)
        self.assertIn(("img/p.png", 0, "deny:secrets:STA_SSID"), hs)
        self.assertEqual(d["deny"], "absent + secrets (4 values, 3 skipped)")

    def test_a_value_in_a_file_name_is_redacted(self):
        sec = self.secrets()
        self.write("x-" + SEC_SSID + ".md", "plain\n")
        rc, out, _ = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--deny-secrets", sec])
        self.assertEqual(rc, 1)
        self.assertNotIn(SEC_SSID.lower(), out.lower())
        self.assertIn("<denied-name #1>:0: deny:secrets:STA_SSID", out)

    def test_missing_or_unreadable_file_is_an_error(self):
        rc, out, err = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks",
                            "--deny-secrets", os.path.join(self.tmp, "nope.h")])
        self.assertEqual(rc, 2)
        self.assertIn("secrets file not found", err)
        bad = self.secrets("bad", text='#define K "' + SEC_SSID + '" "' + SEC_KEY + '"\n')
        rc, out, err = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--deny-secrets", bad])
        self.assertEqual(rc, 2)
        self.assertIn("line 1", err)
        for v in (SEC_SSID, SEC_KEY):
            self.assertNotIn(v, out + err)

    def test_deny_label(self):
        deny = L.DenyList.parse("@category a\nlit:" + "zq" + "x\nw:" + "qzxv" + "\nre:" + BS + "bzq" + BS + "b\n" +
                                "Hidden" + "Garden" + "\n")
        self.assertEqual(L.deny_label(deny), "file (4 patterns, 3 text-only)")
        L.add_secrets_file(deny, self.secrets(), log=io.StringIO())
        self.assertEqual(L.deny_label(deny), "file (4 patterns, 3 text-only) + secrets (4 values, 3 skipped)")
        # the environment's list (CI) never says how many of its patterns are short
        env = L.DenyList.parse("lit:" + "zq" + "x\nHidden" + "Garden" + "\n", "env")
        self.assertEqual(L.deny_label(env), "env (2 patterns; short patterns: text and strings only)")

    def test_every_skip_is_printed_by_name_only(self):
        rc, out, err = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks",
                            "--deny-secrets", self.secrets()])
        for line in ("AP_PASS: equals the example's value, not a secret, skipped",
                     "MQTT_HOST: a generic host name, not a secret, skipped",
                     "ADMIN_PASS: shorter than 8 characters, skipped"):
            self.assertIn("leakgate: secrets: " + line, err)
        self.assertNotIn("MQTT_USER", err)  # an empty string is no value
        for v in (SEC_SSID, SEC_PASS, SEC_KEY, SEC_SHORT, PLACEHOLDER):
            self.assertNotIn(v, out + err)

    def test_example_value_skips_the_same_define_only(self):
        text = '#define STA_PASS "%s"\n#define AP_PASS "%s"\n' % (PLACEHOLDER, PLACEHOLDER)
        got = dict(L.read_secrets(self.secrets("cross", text=text)))
        self.assertEqual(got, {"STA_PASS": PLACEHOLDER})  # the example's AP_PASS placeholder, used as STA_PASS

    def test_deny_secrets_skip(self):
        sec = self.secrets()
        self.write("a.md", "join " + SEC_SSID + " with " + SEC_KEY + "\n")
        base = ["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--json", "--deny-secrets", sec]
        rc, out, err = run(base + ["--deny-secrets-skip", "STA_SSID"])
        hs = {(h["file"], h["line"], h["class"]) for h in json.loads(out)["hits"]}
        self.assertNotIn(("a.md", 1, "deny:secrets:STA_SSID"), hs)
        self.assertIn(("a.md", 1, "deny:secrets:WU_KEY"), hs)
        self.assertIn("STA_SSID: named by --deny-secrets-skip, skipped", err)
        rc, out, err = run(base + ["--deny-secrets-skip", "STA_SSID, WU_KEY", "--deny-secrets-skip", "NOTE_TXT"])
        self.assertFalse([h for h in json.loads(out)["hits"] if h["file"] == "a.md"])
        self.assertIn("+ secrets (1 values, 6 skipped)", json.loads(out)["deny"])
        # a name the file does not define is counted, never echoed (it may be a value typed by mistake)
        rc, out, err = run(base + ["--deny-secrets-skip", SEC_SSID])
        self.assertIn("1 --deny-secrets-skip name(s) not defined", err)
        self.assertNotIn(SEC_SSID, out + err)
        # an argument that is not a define name is refused unechoed
        rc, out, err = run(base + ["--deny-secrets-skip", SEC_PASS])
        self.assertEqual(rc, 2)
        self.assertIn("takes define names", err)
        self.assertNotIn(SEC_PASS, out + err)
        rc, _out, err = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks",
                             "--deny-secrets-skip", "STA_SSID"])
        self.assertEqual(rc, 2)
        self.assertIn("needs --deny-secrets", err)

    def test_a_public_word_in_the_example_text_needs_a_named_skip(self):
        # the live-run shape: a value the code itself uses, also in the example's comment, with an empty example value
        word = "Lumen" + "harbor"
        sec = self.secrets("word", text='#define MQTT_USER "%s"\n' % word, example=False)
        self.write("word/secrets.h.example", '#define MQTT_USER ""   // e.g. "%s"\n' % word)
        self.write("src/app.c", 'const char* user = "%s";\n' % word)
        argv = ["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--json", "--deny-secrets", sec]
        rc, out, _err = run(argv)
        self.assertIn(("src/app.c", 1, "deny:secrets:MQTT_USER"),
                      {(h["file"], h["line"], h["class"]) for h in json.loads(out)["hits"]})
        rc, out, err = run(argv + ["--deny-secrets-skip", "MQTT_USER"])
        self.assertFalse([h for h in json.loads(out)["hits"] if h["class"].startswith("deny:")])
        self.assertIn("MQTT_USER: named by --deny-secrets-skip, skipped", err)

    def test_define_shapes_parsed_or_refused(self):
        ok = ['#define A ("%s")' % SEC_SSID, '#define B u8"%s"  // utf-8' % SEC_KEY.replace(BS, BS + BS),
              '#define C L"%s"' % SEC_PASS.replace('"', BS + '"'), "#define PORT 1883  // a \"quoted\" note",
              '#define D ' + BS + '\n    "%s"' % SEC_SSID,                  # the literal on a continued line
              '#define E /* a "quoted" note */ "%s" /* end' % SEC_SSID,      # comments before and after it
              '/* #define F "%s" */' % SEC_SSID]                             # a commented-out define
        got = dict(L.read_secrets(self.secrets("shapes", text="\n".join(ok) + "\n")))
        self.assertEqual(got, {"A": SEC_SSID, "B": SEC_KEY, "C": SEC_PASS, "D": SEC_SSID, "E": SEC_SSID})
        refused = ['#define K F("%s")', '#define K R"(%s)"', '#define K "%s" "tail-of-it"',
                   '#define K "%s" ' + BS + '\n  "tail-of-it"', '#define K("%s")', '#define K { "%s" }']
        for k, shape in enumerate(refused):
            path = self.secrets("bad%d" % k, text="// header\n" + shape % SEC_SSID + "\n")
            rc, out, err = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks",
                                "--deny-secrets", path])
            self.assertEqual(rc, 2, shape)
            self.assertIn("line 2", err)
            self.assertNotIn(SEC_SSID, out + err)

    def test_form_encoded_twin(self):
        value = "Plum " + "Harbor 7!x"
        sec = self.secrets("form", text='#define STA_PASS "%s"\n' % value)
        self.write("a.md", "body pw=" + L.urllib.parse.quote_plus(value, safe="") + "&x=1\n")
        rc, hs, _ = self.hits("--deny-secrets", sec)
        self.assertEqual(rc, 1)
        self.assertIn(("a.md", 1, "deny:secrets:STA_PASS"), hs)

    def test_non_utf8_file_is_an_error(self):
        path = self.write("enc/secrets.h", bytes([0xFF, 0xFE]) + ('#define K "' + SEC_SSID + '"\n').encode("utf-16-le"))
        rc, out, err = run(["--root", self.tmp, "--manifest", "manifest.txt", "--no-gitleaks", "--deny-secrets", path])
        self.assertEqual(rc, 2)
        self.assertIn("not UTF-8", err)
        self.assertNotIn(SEC_SSID, out + err)


class ShortPatternTests(Tree):
    MANIFEST = "ship **" + chr(10) + "binary img/*" + chr(10) + "binary *.dat" + chr(10)
    SHORT = "zq" + "x"
    LONG = "qua" + "rtzly"   # 7 characters

    def gate_hits(self):
        deny = self.write("deny.txt", "@category person\nlit:" + self.SHORT + "\n@category place\nlit:" + self.LONG +
                          "\n")
        _, hs, _ = self.hits("--deny", deny)
        return hs

    def test_jpeg_pixels_pass_and_the_comment_segment_fails(self):
        self.write("img/pix.jpg", jpeg(pixels=bytes([1]) + self.SHORT.encode() + bytes([2])))
        self.write("img/com.jpg", jpeg([(0xFE, b"by " + self.SHORT.encode())]))
        self.write("img/long.jpg", jpeg(pixels=bytes([1]) + self.LONG.encode() + bytes([2])))
        hs = self.gate_hits()
        self.assertFalse([h for h in hs if h[0] == "img/pix.jpg"])
        self.assertIn(("img/com.jpg", 0, "deny:person"), hs)
        self.assertIn(("img/com.jpg", 0, "metadata"), hs)
        self.assertIn(("img/long.jpg", 0, "deny:place"), hs)   # a 6+ pattern still reads every byte

    def test_png_compressed_text_and_gif_comment(self):
        ztxt = b"Comment" + NULB + NULB + zlib.compress(b"by " + self.SHORT.encode())
        self.write("img/z.png", png([(b"zTXt", ztxt)]))
        self.write("img/c.gif", gif([(0xFE, b"by " + self.SHORT.encode())]))
        self.write("img/p.gif", gif(pixels=self.SHORT.encode()))
        hs = self.gate_hits()
        self.assertIn(("img/z.png", 0, "deny:person"), hs)
        self.assertIn(("img/c.gif", 0, "deny:person"), hs)
        self.assertIn(("img/c.gif", 0, "metadata"), hs)
        self.assertFalse([h for h in hs if h[0] == "img/p.gif"])
        self.assertFalse(L.image_metadata(gif()))

    def test_text_files_and_binary_strings_keep_short_patterns(self):
        self.write("a.md", "name " + self.SHORT + "\n")
        self.write("raw.dat", bytes([0, 1]) + self.SHORT.encode() + bytes([0, 2]))       # not a printable string
        self.write("str.dat", bytes([0, 1]) + b"by " + self.SHORT.encode() + bytes([0, 2]))  # a printable string
        self.write("long.dat", bytes([0, 1]) + self.LONG.encode() + bytes([0, 2]))
        self.write("img/fake.png", "not a png: " + self.SHORT + "\n")                    # no signature
        hs = self.gate_hits()
        self.assertIn(("a.md", 1, "deny:person"), hs)
        self.assertNotIn(("raw.dat", 0, "deny:person"), hs)
        self.assertIn(("str.dat", 0, "deny:person"), hs)
        self.assertIn(("long.dat", 0, "deny:place"), hs)
        self.assertIn(("img/fake.png", 0, "deny:person"), hs)  # a .png is binary: its strings are read

    def test_every_byte_but_the_pixels(self):
        sh = self.SHORT.encode()
        icc = b"ICC_PROFILE" + NULB + bytes([1, 1]) + b"desc by " + sh
        cases = {  # each a place outside the pixel data that holds text
            "img/icc.jpg": jpeg([(0xE2, icc)]),
            "img/xmpext.jpg": jpeg([(0xE1, b"http://ns.adobe.com/xmp/extension/" + NULB + b"by " + sh)]),
            "img/trailer.jpg": jpeg() + b"appended " + sh,
            "img/com2.jpg": jpeg(between=[(0xFE, b"by " + sh)]),
            "img/iccp.png": png([(b"iCCP", sh + b" profile" + NULB + NULB + zlib.compress(b"plain"))]),
            "img/iccp2.png": png([(b"iCCP", b"icc" + NULB + NULB + zlib.compress(b"desc by " + sh))]),
            "img/trailer.png": png() + b"appended " + sh,
            "img/plain.gif": gif([(0x01, b"text " + sh)]),
            "img/icc.webp": webp([(b"VP8 ", b"pix"), (b"ICCP", b"by " + sh)]),
            "img/broken.jpg": bytes([0xFF, 0xD8, 0xFF, 0xE0, 0x7F, 0xFF]) + sh,  # does not parse: read in full
        }
        quiet = {  # the pattern only in pixel data
            "img/scans.jpg": jpeg(pixels=bytes([1, 0xFF, 0, 0xFF, 0xD0]) + sh, between=[(0xC4, b"tables")]),
            "img/twice.jpg": jpeg() + jpeg(pixels=sh),
            "img/idat.png": png([(b"IDAT", sh + zlib.compress(b"x"))]),
            "img/pix.webp": webp([(b"VP8L", b"x" + sh), (b"ALPH", sh + b"y")]),
            "img/pix.gif": gif(pixels=b"ab" + sh),
        }
        for rel, data in list(cases.items()) + list(quiet.items()):
            self.write(rel, data)
        hs = self.gate_hits()
        for rel in cases:
            self.assertIn((rel, 0, "deny:person"), hs, rel)
        for rel in quiet:
            self.assertNotIn((rel, 0, "deny:person"), hs, rel)

    def test_regex_width_decides(self):
        d = L.DenyList.parse("re:" + BS + "bzq" + BS + "b\nre:" + "192" + BS + ".168" + BS + ".4[0-9]" + BS + ".\nw:" +
                             self.SHORT + "\n")
        self.assertEqual([t for _c, _rx, t in d.patterns], [True, False, True])


# the hook body before --deny-secrets existed: without the option the hook stays byte for byte this
OLD_HOOK = ("#!/bin/sh\n"
            "# pre-push: the leak gate on every pushed commit (written by tools/leakgate.py --install-hook)\n"
            "deny=%s\nmanifest=%s\n"
            '[ -f "$deny" ] || { echo "leak gate: the deny-list $deny is missing; push refused" >&2; exit 1; }\n'
            "py=\n"
            'for c in python3 python; do "$c" -c "" >/dev/null 2>&1 && { py=$c; break; }; done\n'
            '[ -n "$py" ] || { echo "leak gate: no python; push refused" >&2; exit 1; }\n'
            'exec "$py" -I tools/leakgate.py --pre-push "$1" --manifest "$manifest" --deny "$deny" --strict-tags\n')


class SecretsGitTests(GitBase):
    """The secrets in the hook and in commit messages (GitTests' own tests are not repeated here)."""

    def setUp(self):
        super().setUp()
        d = tempfile.mkdtemp(prefix="lg-sec-")
        self.addCleanup(L.rmtree, d)
        self.sec = os.path.join(d, "secrets.h")
        with open(self.sec, "w", encoding="utf-8") as f:
            f.write(secrets_fixture())
        self.deny = self.outside("deny.txt", "# empty\n")

    def hook_body(self):
        with open(os.path.join(self.tmp, ".git", "hooks", "pre-push"), encoding="utf-8") as f:
            return f.read()

    def test_hook_body_with_and_without_the_option(self):
        rc, out, _ = run(["--root", self.tmp, "--install-hook", "--deny", self.deny])
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.hook_body(), OLD_HOOK % (L.sh_quote(os.path.abspath(self.deny).replace(BS, "/")),
                                                       L.sh_quote(L.MANIFEST_DEFAULT)))
        rc, out, _ = run(["--root", self.tmp, "--install-hook", "--deny", self.deny, "--deny-secrets", self.sec,
                          "--force"])
        self.assertEqual(rc, 0, out)
        body = self.hook_body()
        self.assertIn("secrets=" + L.sh_quote(os.path.abspath(self.sec).replace(BS, "/")) + "\n", body)
        self.assertIn('[ -f "$secrets" ] || { echo "leak gate: the secrets file $secrets is missing; push refused"',
                      body)
        self.assertIn('--deny "$deny" --deny-secrets "$secrets" --strict-tags', body)
        for v in (SEC_SSID, SEC_PASS, SEC_KEY):
            self.assertNotIn(v, body + out)
        # the skip names ride in the hook; never without the secrets file, never a non-name
        rc, out, err = run(["--root", self.tmp, "--install-hook", "--deny", self.deny, "--deny-secrets", self.sec,
                            "--deny-secrets-skip", "STA_SSID,WU_KEY", "--force"])
        self.assertEqual(rc, 0, err)
        self.assertIn('--deny-secrets "$secrets" --deny-secrets-skip ' + "'STA_SSID,WU_KEY' --strict-tags",
                      self.hook_body())
        self.assertIn("STA_SSID: named by --deny-secrets-skip, skipped", err)
        rc, _, err = run(["--root", self.tmp, "--install-hook", "--deny", self.deny, "--force",
                          "--deny-secrets-skip", "STA_SSID"])
        self.assertEqual(rc, 2)
        self.assertIn("needs --deny-secrets", err)
        rc, _, err = run(["--root", self.tmp, "--install-hook", "--deny", self.deny, "--force",
                          "--deny-secrets", self.sec, "--deny-secrets-skip", "a b"])
        self.assertEqual(rc, 2)
        rc, _, err = run(["--root", self.tmp, "--install-hook", "--deny", self.deny, "--force",
                          "--deny-secrets", self.sec + ".missing"])
        self.assertEqual(rc, 2)
        self.assertIn("secrets file not found", err)
        rc, _, _ = run(["--root", self.tmp, "--install-hook", "--deny", self.deny, "--force",
                        "--deny-secrets", self.sec, "--deny-secrets", self.sec])
        self.assertEqual(rc, 2)

    def load_export(self):
        path = os.path.join(ROOT, *(NOTES + "/publish/export.py").split("/"))
        if not os.path.isfile(path):
            self.skipTest("the export script is not in this tree")
        import importlib.util
        spec = importlib.util.spec_from_file_location("export_under_test", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod

    def test_export_dry_run_passes_the_secrets_through(self):
        E = self.load_export()
        os.makedirs(os.path.join(self.tmp, "tools"))
        shutil.copyfile(os.path.join(ROOT, "tools", "leakgate.py"), os.path.join(self.tmp, "tools", "leakgate.py"))
        self.write("tools/publish-manifest.txt", "ship README.md\nship manifest.txt\nship tools/**\n")
        self.write("README.md", "join " + SEC_SSID + "\n")
        self.commit_all("base")
        deny = self.outside("deny2.txt", "@category place\nw:" + "Hidden" + "Garden" + "\n")
        ok, _r, _c = E.dry_run(self.tmp, "HEAD", deny, gitleaks=False, quiet=True, env={})
        self.assertTrue(ok)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stderr(err):
            ok, result, _c = E.dry_run(self.tmp, "HEAD", deny, gitleaks=False, env={}, out=out, secrets=[self.sec])
        self.assertFalse(ok)
        self.assertIn(("README.md", 1, "deny:secrets:STA_SSID"), {(h.file, h.line, h.cls) for h in result.hits})
        self.assertIn("+ secrets (4 values, 3 skipped)", out.getvalue())
        self.assertIn("AP_PASS: equals the example's value", err.getvalue())
        self.assertNotIn(SEC_SSID, out.getvalue() + err.getvalue())
        # the user names a define whose value is a public word: the dry run is clean again and says so
        out, err = io.StringIO(), io.StringIO()
        with redirect_stderr(err):
            ok, _r, _c = E.dry_run(self.tmp, "HEAD", deny, gitleaks=False, env={}, out=out, secrets=[self.sec],
                                   secrets_skip=["STA_SSID"])
        self.assertTrue(ok, out.getvalue())
        self.assertIn("STA_SSID: named by --deny-secrets-skip, skipped", err.getvalue())
        self.assertIn("+ secrets (3 values, 4 skipped)", out.getvalue())
        err = io.StringIO()
        with redirect_stderr(err):
            rc = E.main(["--commit", "--root", self.tmp, "--deny", deny, "--deny-secrets", self.sec])
        self.assertEqual(rc, 2)
        self.assertIn("dry-run option", err.getvalue())

    def test_commit_message(self):
        self.write("a.md", "clean words\n")
        self.commit_all("tuned on " + SEC_SSID)
        rc, out, _ = run(["--root", self.tmp, "--no-gitleaks", "--messages", "HEAD", "--deny-secrets", self.sec])
        self.assertEqual(rc, 1)
        self.assertRegex(out, r"commit:[0-9a-f]{12}:1: deny:secrets:STA_SSID")
        self.assertNotIn(SEC_SSID, out)

    def test_hook_with_secrets_refuses_a_real_push(self):
        if not shutil.which("sh") and os.name != "nt":
            self.skipTest("no sh")
        self.bare_remote()
        os.makedirs(os.path.join(self.tmp, "tools"))
        shutil.copyfile(os.path.join(ROOT, "tools", "leakgate.py"), os.path.join(self.tmp, "tools", "leakgate.py"))
        self.write("a.md", "clean\n")
        self.commit_all("base")
        rc, out, _ = run(["--root", self.tmp, "--install-hook", "--deny", self.deny, "--deny-secrets", self.sec,
                          "--manifest", "manifest.txt"])
        self.assertEqual(rc, 0, out)
        env = dict(os.environ, LEAKGATE_DENY="")
        push = ["git", "-C", self.tmp, "push", "-q", "origin", "HEAD:refs/heads/main"]
        r = subprocess.run(push, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.assertEqual(r.returncode, 0, r.stderr.decode(errors="replace")[-800:])
        self.write("a.md", "pw " + SEC_PASS + "\n")
        self.commit_all("leak")
        self.write("a.md", "clean again\n")
        self.commit_all("unleak")
        r = subprocess.run(push, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.assertNotEqual(r.returncode, 0)
        text = (r.stdout + r.stderr).decode(errors="replace")
        self.assertIn("deny:secrets:STA_PASS", text)
        self.assertNotIn(SEC_PASS, text)
        # the secrets file gone: the hook refuses even a clean push (fail closed)
        git(self.tmp, "reset", "-q", "--hard", "HEAD~2")
        self.write("a.md", "clean too\n")
        self.commit_all("clean")
        os.unlink(self.sec)
        r = subprocess.run(push, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("secrets file", r.stderr.decode(errors="replace"))


if __name__ == "__main__":
    unittest.main()
