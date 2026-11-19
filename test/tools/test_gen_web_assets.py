"""Tests for tools/gen_web_assets.py: the lazy-asset map, linking, budgets, terser.
Run: python -I -m unittest discover -s test/tools   (needs node + `npm ci --ignore-scripts` in tools/, like the generator)"""
import os
import pathlib
import re
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import gen_web_assets as g  # noqa: E402


class LazyMap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.built = g.assets()
        cls.by_name = {name: (gz, path) for name, gz, path, _ in cls.built}

    def test_every_page_asset_is_built(self):
        for page, deps in g.PAGES.items():
            for d in deps["initial"] + deps["lazy"]:
                self.assertIn(d, self.by_name, "%s needs %s" % (page, d))

    def test_lazy_assets_are_named_by_their_page_script_and_never_loaded_at_once(self):
        html = {p: g.page(g.WEB / p, self.built).decode("utf-8") for p in g.PAGES}
        for page, deps in g.PAGES.items():
            for lazy in deps["lazy"]:
                path = self.by_name[lazy][1]
                self.assertNotIn(path, html[page], "%s must not load %s at once" % (page, lazy))
                # the page's script carries the lazy asset's hashed path (FC.lazy at first use)
                scripts = [d for d in deps["initial"] if d.endswith(".js")]
                found = any(path in g.gzip.decompress(self.by_name[s][0]).decode("utf-8") for s in scripts)
                self.assertTrue(found, "%s: no initial script names %s" % (page, path))

    def test_dashboard_sheet_is_lazy(self):
        # the sheet machinery travels on the first sheet open; dash.js keeps only the stub
        self.assertIn("dash-sheet.js", g.PAGES["index.html"]["lazy"])
        status = g.gzip.decompress(self.by_name["status.js"][0]).decode("utf-8")
        self.assertNotIn("function windRose(", status)
        self.assertNotIn("function exportCsv(", status)

    def test_initial_assets_are_linked_by_the_shell(self):
        for page, deps in g.PAGES.items():
            text = g.page(g.WEB / page, self.built).decode("utf-8")
            for d in deps["initial"]:
                self.assertIn(self.by_name[d][1], text, "%s does not link %s" % (page, d))

    def test_hashed_paths(self):
        for name, gz, path, _ in self.built:
            self.assertRegex(path, r"^/a/[0-9a-f]{8}/" + re.escape(name) + "$")
            self.assertEqual(path.split("/")[2], g.hashlib.sha1(gz).hexdigest()[:8])

    def test_budgets(self):
        self.assertEqual(g.over_budget(self.built), [])
        for page, (shell, initial, total) in g.page_sizes(self.built).items():
            self.assertLessEqual(shell, initial)
            self.assertLessEqual(initial, total)

    def test_a_first_visit_in_a_language_has_its_own_budget(self):
        # the initial page plus its initial scripts' packs, per language: within its figure, one byte less fails, and a
        # language without a figure fails as well
        visits = g.cold_load(self.built)
        self.assertEqual(sorted(visits), sorted(g.BUDGET_FIRST))
        for (page, lang), n in visits.items():
            self.assertLessEqual(n, g.BUDGET_FIRST[(page, lang)])
            with mock.patch.dict(g.BUDGET_FIRST, {(page, lang): n - 1}):
                self.assertIn("%s: first visit in %s %d B > %d" % (page, lang, n, n - 1), g.over_budget(self.built))
        with mock.patch.dict(g.BUDGET_FIRST, clear=True):
            self.assertEqual(len([o for o in g.over_budget(self.built) if "first visit" in o]), len(visits))

    def test_each_page_names_only_its_own_packs(self):
        # the manifest in a face's main bundle lists the packs of the scripts that page loads, not the other face's
        for bundle, page in (("status.js", "index.html"), ("admin.js", "admin.html")):
            text = g.gzip.decompress(self.by_name[bundle][0]).decode("utf-8")
            named = set(re.findall(r'"([\w.-]+\.js)":"/a/[0-9a-f]{8}/', text))
            mine = {a for a in g.PAGES[page]["initial"] + g.PAGES[page]["lazy"] if a in g.lang_assets()}
            self.assertEqual(named, {a for a in mine if any(n.startswith(a[:-3] + ".") for n, _, _, _ in g.lang_packs())}, bundle)

    def test_a_lazy_file_over_the_cap_fails(self):
        big = ("big.js", b"x" * (g.LAZY_FILE_MAX + 1), "/a/00000000/big.js", "text/javascript")
        self.assertTrue(any("big.js" in x for x in g.over_budget(self.built + [big])))
        ok = ("ok.js", b"x" * g.LAZY_FILE_MAX, "/a/00000000/ok.js", "text/javascript")
        self.assertFalse(any("ok.js" in x for x in g.over_budget(self.built + [ok])))

    def test_the_flash_cap_is_enforced(self):
        self.assertEqual(g.WEB_FLASH_MAX, 230736)  # 149 307 code + 52 000 assets + 29 429 language packs (each with its 64 B hash-noise margin)
        # the split: each part within its own figure
        self.assertEqual((g.WEB_CODE_MAX, g.WEB_ASSET_MAX, g.WEB_LANG_MAX), (149307, 52000, 29429))
        code, assets, packs = g.flash_split(self.built)
        self.assertEqual(code + assets + packs, g.flash_total(self.built))
        self.assertLessEqual(code, g.WEB_CODE_MAX)
        self.assertLessEqual(assets, g.WEB_ASSET_MAX)
        self.assertLessEqual(packs, g.WEB_LANG_MAX)
        with mock.patch.object(g, "WEB_LANG_MAX", packs - 1):
            self.assertTrue(any(o.startswith("flash packs:") for o in g.over_budget(self.built)))
        with mock.patch.object(g, "WEB_CODE_MAX", code - 1):
            self.assertTrue(any(o.startswith("flash code:") for o in g.over_budget(self.built)))
        self.assertLessEqual(g.flash_total(self.built), g.WEB_FLASH_MAX)
        with mock.patch.object(g, "WEB_FLASH_MAX", g.flash_total(self.built) - 1):
            self.assertTrue(any(x.startswith("flash:") for x in g.over_budget(self.built)))

    def test_the_generator_fails_loudly_without_node(self):
        with mock.patch.dict(os.environ, {"FAUXCLOUD_NODE": ""}), mock.patch.object(g.shutil, "which", return_value=None):
            g._TERSER_CACHE.clear()
            with self.assertRaises(SystemExit) as e:
                g.terser("var a = 1;")
            self.assertIn("node not found", str(e.exception))

    def test_scripts_are_mangled_and_names_the_pages_use_survive(self):
        status = g.gzip.decompress(self.by_name["status.js"][0]).decode("utf-8")
        self.assertNotIn("\n  ", status)  # terser output, not the line-trimmed source
        self.assertIn("const FC=", status)  # top-level globals keep their names (classic scripts share them)
        self.assertRegex(status, r"const \$=(\w)=>document\.querySelector\(\1\)")  # `$` keeps its name; ids are strings
        self.assertIn(self.by_name["dash-sheet.js"][1], status)  # the hashed lazy path is a string terser keeps

    def test_licenses_are_served_and_linked_from_every_face(self):
        gz, path = self.by_name["licenses.txt"]
        text = g.gzip.decompress(gz).decode("utf-8")
        self.assertEqual(text, g.read(g.LICENSES))  # one source, copied as it is (static: no version in it)
        self.assertIn("ISC License", text)
        self.assertIn("SIL Open Font License 1.1", text)  # the one-line Nunito credit (the OFL text itself is no longer served)
        index = g.page(g.WEB / "index.html", self.built).decode("utf-8")  # static, below the Details panel
        admin = g.gzip.decompress(self.by_name["admin.js"][0]).decode("utf-8")  # home + sign-in views
        setup = g.page(g.WEB / "setup.html", self.built).decode("utf-8")
        for face in (index, admin, setup):
            self.assertIn(path, face)
        for page in ("index.html", "admin.html"):  # the long notices left the shells
            self.assertNotIn("Permission is hereby granted", g.page(g.WEB / page, self.built).decode("utf-8"))

    def test_a_stylesheet_per_face_carries_only_its_face(self):
        # the face stylesheets (sky.css is a lazy stylesheet of its own, no face's section)
        css = {n: g.gzip.decompress(gz).decode("utf-8") for n, (gz, _) in self.by_name.items() if n.startswith("app-") and n.endswith(".css")}
        for t in css.values():
            self.assertNotIn(":where(.f-", t)  # no scoping (one face per stylesheet)
            self.assertRegex(t, r"\[hidden\]\{display:\s*none\s*!important\}")  # the `all` section travels with every face

    def test_sky_effects_wiring(self):
        # sky.css names the moon, star and rain pictures; every face's script names sky.css (setSky links it when the sky is
        # not day); no page loads sky.css or a picture at once
        sky = g.gzip.decompress(self.by_name["sky.css"][0]).decode("utf-8")
        for pic in ("moon.svg", "stars.svg", "rain.svg"):
            self.assertIn(self.by_name[pic][1], sky)
        path = self.by_name["sky.css"][1]
        status = g.gzip.decompress(self.by_name["status.js"][0]).decode("utf-8")
        admin = g.gzip.decompress(self.by_name["admin.js"][0]).decode("utf-8")
        setup = g.page(g.WEB / "setup.html", self.built).decode("utf-8")
        for face in (status, admin, setup):
            self.assertIn(path, face)
        for p in g.PAGES:
            html = g.page(g.WEB / p, self.built).decode("utf-8")
            for name in ("moon.svg", "stars.svg", "rain.svg"):
                self.assertNotIn(self.by_name[name][1], html)
        self.assertNotIn("/*", sky)  # the stylesheet minify: no comment ships

    def test_dark_by_default_and_light_only_as_the_kept_choice(self):
        # no stylesheet, shell or script (source or served) reads the system theme: dark is the default, light lives only under
        # [data-theme="light"], which the toggle sets; each shell ships exactly one theme-color meta, the dark one
        for src in sorted(g.WEB.glob("*.css")) + sorted(g.WEB.glob("*.html")) + sorted(g.WEB.glob("*.js")):
            self.assertNotIn("prefers-color-scheme", src.read_text(encoding="utf-8"), src.name)
        light = re.compile(r'\[data-theme=["\']?light["\']?\]')
        for name, (gz, _) in self.by_name.items():
            if name.endswith(".css"):
                text = g.gzip.decompress(gz).decode("utf-8")
                self.assertNotIn("prefers-color-scheme", text, name)
                self.assertRegex(text, light, name)  # the light palette stays whole
        for p in g.PAGES:
            html = g.page(g.WEB / p, self.built).decode("utf-8")
            self.assertNotIn("prefers-color-scheme", html, p)
            self.assertEqual(re.findall(r'<meta name="theme-color"[^>]*>', html), ['<meta name="theme-color" content="#14366a">'], p)
            self.assertRegex(html, light, p)

    def test_unknown_asset_fails(self):
        with self.assertRaises(SystemExit):
            g.link("/*@asset nope.js*/", self.built)


class PageVersion(unittest.TestCase):
    """The polled shells carry their own version on <html data-page>; the header's *_HTML_VERSION (the ETag, and the
    `page` the firmware puts in /api/live) is that same value, and it moves with the shell's source."""

    @classmethod
    def setUpClass(cls):
        cls.built = g.assets()
        cls.header = g.OUT.read_text(encoding="utf-8")

    def mark(self, shell):
        found = re.findall(rb'data-page="([0-9a-f]{8})"', shell)
        self.assertEqual(len(found), 1)
        self.assertRegex(shell, rb'<html lang="en" class="f-[a-z]+" data-page="[0-9a-f]{8}">')
        return found[0].decode()

    def test_the_shell_names_the_version_the_firmware_reports(self):
        for page in sorted(g.POLLED_PAGES):
            shell, version = g.page_version(g.WEB / page, self.built)
            const = re.search(r'const char %s_VERSION\[\] = "([0-9a-f]{8})";' % g.c_name(pathlib.Path(page)), self.header)
            self.assertEqual(self.mark(shell), version, page)
            self.assertEqual(const.group(1), version, page + ": web_assets.h is stale")
            # hashed over the shell with the placeholder, then written in its place (so it cannot hash itself)
            back = shell.replace(b'data-page="%s"' % version.encode(), g.PAGE_MARK.encode())
            self.assertEqual(g.hashlib.sha1(back).hexdigest()[:8], version)

    def test_the_version_changes_with_the_source(self):
        real = g.read

        def edited(path):
            text = real(path)
            return text.replace("<head>", "<head>\n<meta name=\"x\" content=\"1\">", 1) if path.name == "index.html" else text
        before, v0 = g.page_version(g.WEB / "index.html", self.built)
        with mock.patch.object(g, "read", edited):
            after, v1 = g.page_version(g.WEB / "index.html", self.built)
        self.assertNotEqual(v0, v1)
        self.assertEqual(self.mark(after), v1)
        self.assertEqual(self.mark(before), v0)

    def test_the_version_follows_a_script_the_shell_links(self):
        # the shell names its bundles by hashed path (link()), so a change in core.js moves the shell's version and ETag
        real = g.read

        def edited(path):
            text = real(path)
            return text + "\nconst C17_PROBE = 1;\n" if path.name == "core.js" else text
        _, v0 = g.page_version(g.WEB / "index.html", self.built)
        with mock.patch.object(g, "read", edited):
            built = g.assets()
        old = {n: p for n, _, p, _ in self.built}
        new = {n: p for n, _, p, _ in built}
        self.assertNotEqual(old["status.js"], new["status.js"])
        shell, v1 = g.page_version(g.WEB / "index.html", built)
        self.assertNotEqual(v0, v1)
        self.assertIn(new["status.js"].encode(), shell)
        self.assertEqual(self.mark(shell), v1)

    def test_setup_has_no_version_and_a_polled_shell_needs_one(self):
        self.assertNotIn(b"data-page", g.page(g.WEB / "setup.html", self.built))
        real = g.read
        with mock.patch.object(g, "read", lambda p: real(p).replace(g.PAGE_MARK, "") if p.name == "admin.html" else real(p)):
            with self.assertRaises(SystemExit):
                g.page(g.WEB / "admin.html", self.built)


if __name__ == "__main__":
    unittest.main()
