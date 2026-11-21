"""Tests for tools/build_site.py: the /try/ demo is the shipped dashboard's bytes plus two added blocks per shell (the
shim's script tag, the share card); the site's pages, links, share cards, tokens and words.
Run: python -I -m unittest discover -s test/tools   (needs node + terser + zopfli, like the asset generator)"""
import datetime
import gzip
import hashlib
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import build_site as b  # noqa: E402
import gen_web_assets as g  # noqa: E402


class Build(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = pathlib.Path(cls.tmp.name) / "site"
        cls.files, cls.table = b.emit(cls.out)
        cls.h_assets, cls.h_shells = b.header()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_each_shell_is_page_output_plus_two_blocks(self):
        for page, dest in b.SHELLS.items():
            shell = (self.out / dest).read_bytes()
            self.assertEqual(shell.count(b.DEMO_TAG), 1, dest)
            self.assertEqual(shell.count(b.TRY_OG[dest]), 1, dest)
            self.assertEqual(b.bare(shell, dest), g.page(g.WEB / page, self.table), dest)
            # exactly the two blocks: nothing else was added
            self.assertEqual(len(shell), len(g.page(g.WEB / page, self.table)) + len(b.DEMO_TAG) + len(b.TRY_OG[dest]))

    def test_each_shell_is_the_headers_page_plus_two_blocks(self):
        for page, dest in b.SHELLS.items():
            self.assertEqual(b.bare((self.out / dest).read_bytes(), dest), gzip.decompress(self.h_shells[page]), dest)

    def test_the_share_card_sits_right_before_head_end(self):
        for dest in b.SHELLS.values():
            shell = (self.out / dest).read_bytes()
            self.assertIn(b.TRY_OG[dest] + b"</head>", shell, dest)

    def test_the_tag_comes_after_the_inline_theme_script_and_before_every_bundle(self):
        for dest in b.SHELLS.values():
            lines = (self.out / dest).read_bytes().split(b"\n")
            scripts = [i for i, l in enumerate(lines) if l.startswith(b"<script")]
            tag = lines.index(b.DEMO_TAG.rstrip(b"\n"))
            self.assertTrue(lines[scripts[0]].startswith(b"<script>"), "the inline theme script stays first")
            self.assertEqual(scripts[1], tag, dest)
            self.assertTrue(all(lines[i].startswith(b'<script src="/a/') for i in scripts[2:]), dest)

    def test_assets_are_the_headers_paths_and_bytes(self):
        emitted = {"/" + p.relative_to(self.out).as_posix() for p in (self.out / "a").rglob("*") if p.is_file()}
        self.assertEqual(emitted, set(self.h_assets))
        for path, gzb in self.h_assets.items():
            self.assertEqual(hashlib.sha1(gzb).hexdigest()[:8], path.split("/")[2], path)
            self.assertEqual((self.out / path.lstrip("/")).read_bytes(), gzip.decompress(gzb), path)

    def test_every_shell_link_is_served(self):
        import re
        for dest in b.SHELLS.values():
            for path in re.findall(rb'"(/a/[0-9a-f]{8}/[^"]+)"', (self.out / dest).read_bytes()):
                self.assertTrue((self.out / path.decode().lstrip("/")).is_file(), path)

    def test_extras_and_fixtures_are_copied_as_they_are(self):
        for dest, src in b.EXTRAS.items():
            self.assertEqual((self.out / dest).read_bytes(), src.read_bytes(), dest)
        rel = [r for r, _ in b.fixture_files()]
        self.assertIn("index.json", rel)
        for part in ("history/series.json", "history/daily.json", "static/diag.json", "admin/settings.json"):
            self.assertIn(part, rel)
        self.assertFalse(any(r.startswith("snap/") for r in rel), "the shim derives every body; snapshots stay in demo/")
        for r, src in b.fixture_files():
            self.assertEqual((self.out / "try" / "data" / r).read_bytes(), src.read_bytes(), r)

    def test_check_passes_on_a_clean_tree(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(b.check(pathlib.Path(t) / "site"), [])

    def test_check_names_a_drifted_shell(self):
        real = g.page

        def drifted(path, table):
            return real(path, table).replace(b"<title>", b"<title>x", 1)
        g.page = drifted
        try:
            with tempfile.TemporaryDirectory() as t:
                errors = b.check(pathlib.Path(t) / "site")
        finally:
            g.page = real
        self.assertTrue(any("header" in e for e in errors), errors)

    def test_check_names_a_drifted_asset(self):
        real = g.assets

        def drifted():
            t = real()
            name, gzb, path, ctype = t[0]
            return [(name, gzip.compress(gzip.decompress(gzb) + b" "), path, ctype)] + t[1:]
        g.assets = drifted
        try:
            with tempfile.TemporaryDirectory() as t:
                errors = b.check(pathlib.Path(t) / "site")
        finally:
            g.assets = real
        self.assertTrue(any("emitted bytes differ" in e for e in errors), errors)

    def test_the_build_writes_its_marker(self):
        self.assertTrue((self.out / b.MARKER).is_file())

    def test_a_folder_that_is_not_a_build_is_never_emptied(self):
        with tempfile.TemporaryDirectory() as t:
            keep = pathlib.Path(t) / "mine.txt"
            keep.write_text("x")
            with self.assertRaises(SystemExit):
                b.clear(pathlib.Path(t))
            self.assertTrue(keep.is_file())
            with self.assertRaises(SystemExit):
                b.clear(keep)  # a file is not a build folder
            self.assertTrue(keep.is_file())

    def test_an_earlier_build_or_an_empty_folder_is_emptied(self):
        with tempfile.TemporaryDirectory() as t:
            out = pathlib.Path(t) / "site"
            (out / "try").mkdir(parents=True)
            (out / b.MARKER).write_text("")
            (out / "try" / "old.js").write_text("x")
            b.clear(out)
            self.assertFalse(out.exists())
            out.mkdir()
            b.clear(out)
            self.assertFalse(out.exists())

    def test_out_needs_a_value(self):
        with self.assertRaises(SystemExit):
            b.main(["--out"])

    def test_a_shell_without_a_bundle_script_is_refused(self):
        with self.assertRaises(SystemExit):
            b.with_tag(b"<html>\n<script>inline()</script>\n</html>\n")


class Site(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = pathlib.Path(cls.tmp.name) / "site"
        cls.files, _ = b.emit(cls.out)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_the_pages_are_emitted_at_clean_urls(self):
        want = ["index.html", "install/index.html", "docs/index.html", "docs/all-pages/index.html", "404.html", "favicon.svg",
                b.SOCIAL]
        want += ["docs/%s/index.html" % slug for slug, *_ in b.DOCS]
        for p in want:
            self.assertIn(p, self.files, p)
        self.assertIn('src="/try/#frost-night"', self.files["index.html"].decode("utf-8"), "the live demo on the landing")

    def test_pages_without_a_sidebar_use_the_one_column_layout(self):
        for p in ("install/index.html", "404.html"):
            page = self.files[p].decode("utf-8")
            self.assertNotIn('class="docs"', page, p)
            self.assertIn('<div class="single">', page, p)
        for p in ("docs/index.html", "docs/flash/index.html"):
            self.assertIn('<div class="docs">', self.files[p].decode("utf-8"), p)

    def test_the_docs_root_opens_get_started_and_the_map_has_its_own_url(self):
        self.assertEqual(self.files["docs/index.html"], self.files["docs/get-started/index.html"])
        root = self.files["docs/index.html"].decode("utf-8")
        self.assertIn('<link rel="canonical" href="%s/docs/get-started/">' % b.SITE_URL, root)
        self.assertIn('<a href="/docs/all-pages/">All pages</a>', root)  # the sidebar's Index entry
        self.assertIn("<h1>All pages</h1>", self.files["docs/all-pages/index.html"].decode("utf-8"))

    def test_the_footer_and_the_404_page_link_to_the_map(self):
        for p in ("index.html", "docs/flash/index.html", "404.html"):
            self.assertIn('<a href="/docs/all-pages/">All pages</a>', self.files[p].decode("utf-8"), p)
        self.assertIn('<a href="/docs/all-pages/">all pages</a>', self.files["404.html"].decode("utf-8"))

    def test_the_sidebar_lists_h3s_only_on_a_page_of_few_sections(self):
        def sub_list(slug):
            page = self.files["docs/%s/index.html" % slug].decode("utf-8")
            start = page.index('<ul class="sub"')
            return page[start:page.index("</nav>", start)]
        self.assertIn('</a><ul><li><a href="#what-fauxcloud-does-with-this-console"', sub_list("compatibility"))
        self.assertIn('</a><ul><li><a href="#what-to-buy"', sub_list("boards"))
        self.assertNotIn("</a><ul>", sub_list("troubleshooting"))
        self.assertNotIn("</a><ul>", sub_list("flash"))  # four h2s: the h3s stay in the page

    def test_the_sidebar_never_repeats_a_word_on_top_of_itself(self):
        """/docs/get-started/ on a phone showed "Get started" three times (the sheet's summary, the group label, the
        row). The group is "Start"; a group named like one of its pages (Boards, Home Assistant) has no label; the
        current page's label is marked, and the phone sheet hides it where it sits right under its summary."""
        def side(p):
            page = self.files[p].decode("utf-8")
            start = page.index('<nav aria-label="Docs">')
            return page[start:page.index("</details>", start)]
        self.assertEqual(b.GROUPS[0], "Start")
        self.assertEqual([g for s, _, g, *_ in b.DOCS if s in ("get-started", "flash")], ["Start", "Start"])
        pages = [k for k in self.files if k.startswith("docs/") and k.endswith("index.html")]
        self.assertGreater(len(pages), 10)
        for p in pages:
            nav = side(p)
            for grp in ("Boards", "Home Assistant", "Get started"):
                self.assertNotRegex(nav, r"<h4[^>]*>%s</h4>" % grp, p)
            for grp in ("Start", "Use it", "Online, if you want it", "Help and reference", "Project", "Index"):
                self.assertRegex(nav, r"<h4( class=\"here\")?>%s</h4>" % grp, p)
            self.assertEqual(nav.count(">Get started</a>"), 1, p)
            self.assertLessEqual(nav.count('class="here"'), 1, p)
        for slug in ("get-started", "flash"):
            page = self.files["docs/%s/index.html" % slug].decode("utf-8")
            self.assertIn('<summary>Start <span>All pages</span></summary>', page)
            self.assertTrue(side("docs/%s/index.html" % slug).startswith('<nav aria-label="Docs">\n        <h4 class="here">Start</h4>'))
        nav = side("docs/dashboard/index.html")
        self.assertIn('<h4>Start</h4>', nav)
        self.assertIn('<h4 class="here">Use it</h4>', nav)
        self.assertNotIn('class="here"', side("docs/all-pages/index.html"))
        css = (pathlib.Path(__file__).resolve().parents[2] / "site" / "site.css").read_text("utf-8")
        self.assertIn("@media (max-width: 899px) { .side nav > h4.here:first-child { display: none; } }", css)

    def test_the_header_links_to_the_installer_on_every_page(self):
        for p in ("index.html", "docs/get-started/index.html", "docs/all-pages/index.html", "install/index.html", "404.html"):
            page = self.files[p].decode("utf-8")
            bar = page[page.index('<header class="bar'):page.index("</header>")]
            self.assertEqual(bar.count('href="/install/"'), 2, p)  # the bar and the phone menu
        bar = self.files["install/index.html"].decode("utf-8")
        self.assertIn('<a href="/install/" aria-current="page">Install</a>', bar)
        self.assertNotIn('<a href="/install/" aria-current="page">', self.files["index.html"].decode("utf-8"))

    def test_docs_photos_carry_every_variant_and_their_size(self):
        page = self.files["docs/boards/index.html"].decode("utf-8")
        self.assertIn('<figure class="ph tall"><img src="/assets/board-300.jpg" srcset="/assets/board-300.jpg 300w, '
                      '/assets/board-600.jpg 600w" sizes="200px" width="300" height="402"', page)
        self.assertIn('<figure class="ph"><img src="/assets/case-340.jpg" srcset="/assets/case-340.jpg 340w, '
                      '/assets/case-680.jpg 680w" sizes="(min-width: 600px) 340px, calc(100vw - 32px)" width="340" '
                      'height="255"', page)
        self.assertIn('<a class="btn primary" href="https://makerworld.com/en/models/3640408-', page)
        docs = b.Docs()
        for bad in ("../site/assets/social.png", "../site/assets/board-x.jpg", "../README.md"):
            with self.assertRaises(SystemExit, msg=bad):
                docs.figure("docs/boards.md", "a", bad)
        for data in (b"\xff\xd8\x00", b"\xff\xd8\xff\xe0\x00\x10" + b"\x00" * 14):
            with self.assertRaises(SystemExit):
                b.jpeg_size(data)
        self.assertEqual(b.jpeg_size((b.ROOT / "site" / "assets" / "phone-200.jpg").read_bytes()), (200, 268))

    def test_every_photo_can_open_its_largest_variant(self):
        # the popout (site.js) wraps each figure image in a button and opens the largest srcset candidate in a <dialog>
        import re
        js = next(v for k, v in self.files.items() if re.match(r"assets/site-[0-9a-f]{8}\.js$", k)).decode("utf-8")
        for needle in ("figure.ph img, figure.shot img", ".showModal()", "Enlarge: ", "HTMLDialogElement"):
            self.assertIn(needle, js)
        self.assertRegex(js, r"""addEventListener\(["']close["']""")
        self.assertRegex(js, r"""setAttribute\(["']aria-label["']""")
        n = 0
        for p, d in b.site_pages(self.files).items():
            for fig in re.findall(r"<figure class=\"(?:ph|shot)[^\"]*\">.*?</figure>", d, re.S):
                cands = re.findall(r"(/assets/[\w.-]+) (\d+)w", fig)
                self.assertTrue(cands, "%s: a figure image without a srcset: %s" % (p, fig[:120]))
                big = max(cands, key=lambda c: int(c[1]))[0]
                self.assertIn(big.lstrip("/"), self.files, "%s: %s" % (p, big))
                n += 1
        self.assertGreaterEqual(n, 10)
        page = self.files["docs/dashboard/index.html"].decode("utf-8")
        self.assertIn('/assets/shot-dashboard-750.jpg 750w"', page)
        self.assertIn("<figcaption>", page)

    def test_a_wide_table_names_its_columns_for_the_phone_cards(self):
        page = self.files["docs/compatibility/index.html"].decode("utf-8")
        self.assertIn('<td role="cell" data-label="Status">verified</td>', page)
        self.assertIn('<table role="table">', page)

    def test_the_dashboard_has_the_phone_section_with_its_photo(self):
        page = self.files["docs/dashboard/index.html"].decode("utf-8")
        self.assertIn('<h2 id="on-your-phone">On your phone</h2>', page)
        self.assertIn('<figure class="ph tall"><img src="/assets/phone-200.jpg" srcset="/assets/phone-200.jpg 200w, '
                      '/assets/phone-400.jpg 400w"', page)
        self.assertIn('href="/docs/dashboard/#on-your-phone"', self.files["docs/get-started/index.html"].decode("utf-8"))
        for n in ("phone-200.jpg", "phone-400.jpg"):  # the source carries an APP11 block: a variant keeps only JFIF
            d, i, apps = self.files["assets/" + n], 2, []
            while d[i] == 0xFF and d[i + 1] != 0xDA:
                if 0xE1 <= d[i + 1] <= 0xEF or d[i + 1] == 0xFE:
                    apps.append(hex(d[i + 1]))
                i += 2 + int.from_bytes(d[i + 2:i + 4], "big")
            self.assertEqual(apps, [], n)

    def test_plans_sits_between_contributing_and_changelog(self):
        slugs = [slug for slug, _, group, *_ in b.DOCS if group == "Project"]
        self.assertEqual(slugs, ["security", "contributing", "plans", "changelog"])

    def test_every_page_carries_one_share_card(self):
        self.assertEqual(b.page_checks(self.files), [])
        for p, d in b.site_pages(self.files).items():
            self.assertEqual(d.count('<meta name="twitter:card" content="summary_large_image">'), 1, p)
            self.assertIn('content="%s/%s"' % (b.SITE_URL, b.SOCIAL), d, p)

    def test_the_share_card_is_1280_by_640_and_under_1_mb(self):
        import struct
        png = self.files[b.SOCIAL]
        self.assertEqual(png[:4], bytes([0x89, 0x50, 0x4E, 0x47]))
        self.assertEqual(struct.unpack(">II", png[16:24]), (1280, 640))
        self.assertLess(len(png), 1024 * 1024)

    def test_every_internal_link_resolves(self):
        errors, external = b.link_check(self.files)
        self.assertEqual(errors, [])
        self.assertFalse([u for u in external if b.SHOPS.search(u)])

    def test_the_link_check_names_a_dead_link_and_a_dead_anchor(self):
        files = dict(self.files)
        files["docs/x/index.html"] = b'<a href="/docs/nope/">a</a><a href="/docs/get-started/#no-such-id">b</a>'
        errors, _ = b.link_check(files)
        self.assertTrue(any("/docs/nope/" in e for e in errors), errors)
        self.assertTrue(any("no-such-id" in e for e in errors), errors)

    def test_the_link_check_refuses_an_unlisted_host(self):
        errors, _ = b.link_check({"docs/x/index.html": b'<a href="https://makerworld.com/en/">a</a><a href="https://x.example/">b</a>'})
        self.assertEqual(errors, ["docs/x/index.html: a link to a host not in HOSTS: https://x.example/"])

    def test_the_word_checks_catch_internal_words(self):
        page = self.files["install/index.html"].decode("utf-8")
        # the bad words are joined from parts, so this test file does not trip the leak gate itself
        for bad in ("See " + "D" + "17.", "the owner said", b.PRIVATE[0] + "/plan.md", "block b34.3", "za" + chr(0x17C) + "le"):
            files = dict(self.files)
            files["install/index.html"] = page.replace("Install fauxcloud</h1>", "Install fauxcloud %s</h1>" % bad).encode("utf-8")
            self.assertTrue(b.page_checks(files), bad)

    def test_the_theme_is_dark_unless_light_was_chosen(self):
        page = self.files["index.html"].decode("utf-8")
        self.assertNotIn("prefers-color-scheme", page)
        css = next(v for k, v in self.files.items() if k.startswith("assets/site-") and k.endswith(".css")).decode("utf-8")
        self.assertNotIn("prefers-color-scheme", css)
        self.assertIn("localStorage.getItem('fc-theme')==='light'", page)
        self.assertIn('<meta name="theme-color" content="#0c1430">', page)

    def test_tokens_are_generated_from_app_css_and_committed(self):
        t = b.tokens()
        self.assertIn("--ink: #f3f5fb", t)
        self.assertIn(':root[data-theme="light"]', t)
        self.assertNotIn("prefers-color-scheme", t)
        self.assertEqual(b.TOKENS.read_text(encoding="utf-8").replace("\r\n", "\n"), t,
                         "run build_site.py and commit site/tokens.css")

    def test_check_names_a_token_drift(self):
        real = g.css_sections

        def drifted():
            s = real()
            s["dash"] = s["dash"].replace("--ink: #f3f5fb", "--ink: #f3f5fc", 1)
            return s
        g.css_sections = drifted
        try:
            with tempfile.TemporaryDirectory() as t:
                errors = b.check(pathlib.Path(t) / "site")
        finally:
            g.css_sections = real
        self.assertTrue(any("tokens.css" in e for e in errors), errors)

    def test_budgets(self):
        for name, (n, cap) in b.budgets(self.files).items():
            self.assertLessEqual(n, cap, name)

    def test_security_txt_and_robots_are_served_as_they_are(self):
        self.assertEqual(self.files[".well-known/security.txt"], b.SECURITY_TXT.read_bytes())
        self.assertEqual(self.files["robots.txt"], b.ROBOTS.read_bytes())
        self.assertTrue((self.out / ".well-known" / "security.txt").is_file())
        self.assertEqual(b.security_txt_checks(self.files[".well-known/security.txt"].decode("utf-8"),
                                               datetime.datetime.now(datetime.timezone.utc)), [])
        self.assertEqual(b.robots_checks(self.files["robots.txt"].decode("utf-8")), [])

    def test_the_security_txt_check_names_each_fault(self):
        now = datetime.datetime(2030, 6, 1, tzinfo=datetime.timezone.utc)
        good = ("Contact: mailto:security@fauxcloud.org\nExpires: 2031-01-01T00:00:00.000Z\n"
                "Canonical: https://fauxcloud.org/.well-known/security.txt\nPolicy: https://github.com/x/y/security/policy\n")
        self.assertEqual(b.security_txt_checks(good, now), [])
        self.assertEqual(b.security_txt_checks("# a comment\n\n" + good, now), [])
        for text, word in ((good.replace("2031-01-01", "2030-05-31"), "has passed"),
                           (good.replace("2031-01-01", "2031-06-03"), "more than a year"),
                           (good.replace("Expires: 2031-01-01T00:00:00.000Z\n", ""), "exactly once"),
                           (good.replace("2031-01-01T00:00:00.000Z", "2031-01-01"), "not YYYY"),
                           (good.replace("mailto:security@fauxcloud.org", "security@fauxcloud.org"), "Contact"),
                           (good.replace("Contact: mailto:security@fauxcloud.org\n", ""), "Contact"),
                           (good.replace("https://fauxcloud.org/.well-known", "https://example.org/.well-known"), "Canonical"),
                           (good.replace("Policy: https:", "Policy: http:"), "Policy"),
                           (good + "Contcat: mailto:a@b.c\n", "not a known"),
                           (good + "# see D" + "12\n", "a decision number")):  # in parts: the export gate reads this file
            errors = b.security_txt_checks(text, now)
            self.assertTrue(any(word in e for e in errors), (word, errors))

    def test_the_robots_check_refuses_a_closed_site(self):
        line = "\nSitemap: https://fauxcloud.org/sitemap.xml\n"
        self.assertEqual(b.robots_checks("User-agent: *\nAllow: /\n" + line), [])
        self.assertTrue(b.robots_checks("User-agent: *\nDisallow: /\n" + line))
        self.assertTrue(b.robots_checks("User-agent: GPTBot\nAllow: /\n" + line))
        self.assertTrue(b.robots_checks("User-agent: *\nAllow: /\n"), "no Sitemap line")

    def test_the_sitemap_lists_every_page_meant_for_search_and_nothing_else(self):
        # independent of the build's own table: every HTML page whose canonical URL is its own and that is not noindex
        import re
        want = set()
        for p, d in self.files.items():
            if not p.endswith(".html") or p.startswith("a/"):
                continue
            head = d.decode("utf-8").split("</head>")[0]
            url = "/" + p[:-len("index.html")] if p.endswith("index.html") else "/" + p
            if 'name="robots" content="noindex"' not in head and ('<link rel="canonical" href="%s%s">' % (b.SITE_URL, url)) in head:
                want.add(b.SITE_URL + url)
        sm = self.files[b.SITEMAP].decode("utf-8")
        self.assertTrue(sm.startswith('<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">'))
        locs = re.findall(r"<url><loc>([^<]+)</loc><lastmod>(\d{4}-\d{2}-\d{2})</lastmod></url>", sm)
        self.assertEqual(len(locs), sm.count("<url>"), "every row has a loc and a lastmod")
        self.assertEqual(len(locs), len({u for u, _ in locs}), "no URL twice")
        self.assertEqual({u for u, _ in locs}, want)
        # the pages the search should find, and the ones it should not
        for url in ("/", "/install/", "/docs/get-started/", "/docs/home-assistant/", "/docs/all-pages/", "/try/"):
            self.assertIn(b.SITE_URL + url, want)
        for url in ("/docs/", "/404.html", "/try/admin.html"):
            self.assertNotIn(b.SITE_URL + url, want)
        self.assertNotIn("priority", sm)
        self.assertEqual(b.seo_checks(self.files), [])

    def test_the_sitemap_lastmod_comes_from_git(self):
        self.assertRegex(b.git_date([b.ROOT / "docs" / "get-started.md"]) or "", r"^\d{4}-\d{2}-\d{2}$")
        self.assertIsNone(b.git_date([b.ROOT / "docs" / "no-such-page.md"]))  # never committed: the build date is used

    def test_the_search_tags_of_each_page(self):
        for p, d in {**b.site_pages(self.files), **{k: self.files[k].decode("utf-8") for k in b.SHELLS.values()}}.items():
            titles, descs, canon, noindex = b.head_tags(d)
            self.assertEqual(len(titles), 1, p)
            self.assertLessEqual(len(titles[0]), b.TITLE_MAX, p)
            self.assertEqual(len(descs), 1, p)
            self.assertLessEqual(len(descs[0]), b.DESC_MAX, p)
            self.assertEqual(len(canon) + noindex, 1, p)
        # the docs root is a copy of Get started: its canonical URL names the page
        self.assertEqual(b.head_tags(self.files["docs/index.html"].decode("utf-8"))[2], [b.SITE_URL + "/docs/get-started/"])
        self.assertTrue(b.head_tags(self.files["404.html"].decode("utf-8"))[3])
        self.assertTrue(b.head_tags(self.files["try/admin.html"].decode("utf-8"))[3])
        self.assertEqual(b.head_tags(self.files["try/index.html"].decode("utf-8"))[2], [b.SITE_URL + "/try/"])

    def test_the_landing_describes_itself_for_search(self):
        import json
        import re
        page = self.files["index.html"].decode("utf-8")
        blocks = re.findall(r'<script type="application/ld\+json">(.*?)</script>', page, re.S)
        self.assertEqual(len(blocks), 1)
        ld = json.loads(blocks[0])
        self.assertEqual((ld["@type"], ld["name"], ld["url"], ld["operatingSystem"]),
                         ("SoftwareApplication", "fauxcloud", b.SITE_URL + "/", "ESP32"))
        self.assertIn("GPL-3.0-or-later", ld["license"])
        self.assertEqual(ld["offers"]["price"], "0")
        self.assertNotIn("softwareVersion", ld)  # a version here would go stale at the next release
        self.assertEqual(sum(d.count("application/ld+json") for d in b.site_pages(self.files).values()), 1, "landing only")

    def test_the_search_checks_name_each_fault(self):
        page = self.files["docs/boards/index.html"].decode("utf-8")
        title = b.head_tags(page)[0][0]
        desc = b.head_tags(page)[1][0]
        cases = [
            (page.replace("<title>%s</title>" % title, "<title>%s</title>" % ("x" * 61)), "title of at most"),
            (page.replace('content="%s">' % desc, 'content="%s">' % ("y" * 161), 1), "description of at most"),
            (page.replace('<link rel="canonical" href="%s/docs/boards/">' % b.SITE_URL, ""), "canonical URL or noindex"),
            (page.replace('<link rel="canonical" href="%s/docs/boards/">' % b.SITE_URL,
                          '<link rel="canonical" href="%s/docs/nope/">' % b.SITE_URL), "not a page of the site"),
            (self.files["docs/flash/index.html"].decode("utf-8").replace(
                '"%s/docs/flash/"' % b.SITE_URL, '"%s/docs/boards/"' % b.SITE_URL), "the same title"),
        ]
        for bad, word in cases:
            files = dict(self.files)
            files["docs/boards/index.html"] = bad.encode("utf-8")
            errors = b.seo_checks(files)
            self.assertTrue(any(word in e for e in errors), (word, errors))
        files = dict(self.files)
        files[b.SITEMAP] = self.files[b.SITEMAP].replace(b"<url><loc>%s/try/</loc>" % b.SITE_URL.encode(), b"<url><loc>x</loc>")
        self.assertTrue(any("sitemap.xml" in e for e in b.seo_checks(files)))

    def test_private_sources_are_refused(self):
        for rel in (b.PRIVATE[0] + "/plan.md", b.PRIVATE[1] + "/x.md", b.PRIVATE[2] + "/a.txt", "firmware/fauxcloud/secrets.h"):
            with self.assertRaises(SystemExit):
                b.repo_source(rel)


class Markdown(unittest.TestCase):
    def render(self, text):
        import site_md
        return site_md.render(text, "t.md", lambda t: "/L/" + t)

    def test_front_matter_steps_callouts_tables_code(self):
        src = ("---@LF@title: T@LF@order: 1@LF@lede: L@LF@---@LF@## Steps@LF@@LF@1. Do it.@LF@@LF@   **You should see** it.@LF@"
               "2. Next.@LF@@LF@> [!WARNING]@LF@> **Careful**@LF@> Text.@LF@@LF@| A | B |@LF@|---|---|@LF@| `a|b` | [x](y.md) |@LF@@LF@"
               '```text title="One"@LF@<b>&@LF@```@LF@')
        meta, out, toc, _ = self.render(src.replace("@LF@", chr(10)))
        self.assertEqual(meta["title"], "T")
        self.assertEqual(toc, [("steps", "Steps")])
        self.assertIn('<ol class="steps">', out)
        self.assertIn('<p class="see"><b>You should see</b> it.</p>', out)
        self.assertIn('<div class="note" role="note">', out)
        self.assertIn("<b>Careful</b>", out)
        self.assertIn('<td data-label="A"><code>a|b</code></td>', out)  # each cell names its column
        self.assertIn('<a href="/L/y.md">x</a>', out)
        self.assertIn("<span>One</span>", out)
        self.assertIn("&lt;b&gt;&amp;", out)

    def test_an_autolink_cannot_open_an_attribute(self):
        _, out, _, _ = self.render('See <https://x.example/"onmouseover="alert(1)> now.' + chr(10))
        import re
        self.assertFalse(re.search(r'<a [^>]*onmouseover', out), out)  # left as text, never inside the tag
        _, out, _, _ = self.render("See https://x.example/a?b=1&c=2 now." + chr(10))
        self.assertIn('<a href="https://x.example/a?b=1&amp;c=2">', out)

    def test_a_url_as_a_link_label_is_one_anchor(self):
        _, out, _, _ = self.render("[https://a.example/](https://a.example/) and [**b**](c.md)" + chr(10))
        self.assertEqual(out.count("<a "), 2, out)
        self.assertIn('<a href="/L/https://a.example/">https://a.example/</a>', out)
        self.assertIn('<a href="/L/c.md"><b>b</b></a>', out)

    def test_a_bold_link_alone_is_a_button(self):
        _, out, _, _ = self.render("[**Flash it**](https://fauxcloud.org/install/)" + chr(10) + chr(10) + "A [**b**](c.md) in text." + chr(10))
        self.assertIn('<p class="ctas"><a class="btn primary" href="/L/https://fauxcloud.org/install/">Flash it</a></p>', out)
        self.assertIn('A <a href="/L/c.md"><b>b</b></a> in text.', out)

    def test_troubleshooting_labels_become_a_labelled_entry(self):
        lf = chr(10)
        _, out, _, _ = self.render("**Cause.** It is off." + lf + lf + "**Fix.**" + lf + lf + "- One." + lf + "- Two." + lf + lf
                                   + "After." + lf + lf + "**fauxcloud shows this as** **Offline**." + lf)
        self.assertIn('<dl class="lab"><dt>Cause</dt><dd><p>It is off.</p></dd></dl>', out)
        self.assertIn('<dl class="lab"><dt>Fix</dt><dd><ul>', out)  # a Fix. alone takes the list after it
        self.assertIn("</ul>" + lf + "<p>After.</p></dd></dl>", out)  # and the plain paragraphs after that list
        self.assertIn("<dt>fauxcloud shows this as</dt><dd><p><b>Offline</b>.</p></dd>", out)
        _, out, _, _ = self.render("**Cause** without a stop, and **Fix.** inside a line." + lf)
        self.assertNotIn("<dl", out)
        _, out, _, _ = self.render("**Fix.**" + lf + lf + "- A." + lf + lf + "B." + lf + lf + "## Next" + lf + lf + "C." + lf)
        self.assertIn("<p>B.</p></dd></dl>" + lf + '<h2 id="next">Next</h2>' + lf + "<p>C.</p>", out)
        _, out, _, _ = self.render("**Fix.**" + lf + lf + "- A." + lf + lf + "**fauxcloud shows this as** X." + lf)
        self.assertEqual(out.count("<dl"), 2, out)
        for bare in ("**Fix.**" + lf + lf + "Text." + lf, "**Cause.**" + lf):
            with self.assertRaises(SystemExit):
                self.render(bare)

    def test_h4_groups_and_the_outline(self):
        import site_md
        lf, outline = chr(10), []
        _, out, toc, _ = site_md.render("## A" + lf + "### B" + lf + "#### C" + lf + "Text." + lf, "t.md", lambda t: t, outline)
        self.assertEqual(toc, [("a", "A")])
        self.assertEqual(outline, [(2, "a", "A"), (3, "b", "B")])
        self.assertIn('<h4 id="c">C</h4>', out)
        with self.assertRaises(SystemExit):
            self.render("##### E" + lf)

    def test_raw_html_is_escaped_and_images_refused(self):
        _, out, _, _ = self.render("A <script>x</script> line." + chr(10))
        self.assertIn("&lt;script&gt;", out)
        with self.assertRaises(SystemExit):
            self.render("![a](b.png)" + chr(10))  # no figure hook: refused

    def test_a_photo_paragraph_goes_to_the_figure_hook(self):
        import site_md
        lf = chr(10)
        hook = lambda alt, t, c="": "<FIG %s|%s|%s>" % (alt, t, c)  # noqa: E731
        _, out, _, _ = site_md.render("Text." + lf + lf + "![A & b](../x-600.jpg)" + lf + lf + "More." + lf, "t.md",
                                      lambda t: t, None, hook)
        self.assertIn("<FIG A & b|../x-600.jpg|>", out)
        _, out, _, _ = site_md.render('![a](../x-600.jpg "The *view* now")' + lf, "t.md", lambda t: t, None, hook)
        self.assertIn("<FIG a|../x-600.jpg|The <em>view</em> now>", out)
        # photos one after another: one row, in their order; a photo alone stays a figure of its own
        _, out, _, _ = site_md.render("![a](../x-600.jpg)" + lf + lf + "![b](../y-600.jpg)" + lf + lf + "![c](../z-600.jpg)" + lf + lf
                                      + "Text." + lf + lf + "![d](../w-600.jpg)" + lf, "t.md", lambda t: t, None, hook)
        self.assertIn('<div class="phrow"><FIG a|../x-600.jpg|><FIG b|../y-600.jpg|><FIG c|../z-600.jpg|></div>', out)
        self.assertEqual(out.count('class="phrow"'), 1)
        self.assertIn("<FIG d|../w-600.jpg|>", out)
        for bad in ("A ![a](b.jpg) inside a line." + lf, "![a](b.jpg)" + lf + "text on the next line" + lf):
            with self.assertRaises(SystemExit):
                site_md.render(bad, "t.md", lambda t: t, None, hook)

    def test_a_table_row_must_match_its_header(self):
        lf = chr(10)
        _, out, _, _ = self.render("| A | B |" + lf + "|---|---|" + lf + "| 1 | 2 |" + lf)
        self.assertIn('<td data-label="B">2</td>', out)
        self.assertNotIn("role=", out)  # a two-column table stays a plain table
        for row in ("| 1 | 2 | 3 |", "| 1 |"):
            with self.assertRaises(SystemExit) as e:
                self.render("x" + lf + lf + "| A | B |" + lf + "|---|---|" + lf + "| 0 | 0 |" + lf + row + lf)
            self.assertIn("t.md:6:", str(e.exception))
        _, out, _, _ = self.render("| A | B | C |" + lf + "|---|---|---|" + lf + "| 1 | 2 | 3 |" + lf)
        self.assertIn('<table role="table">', out)
        self.assertIn('<th scope="col" role="columnheader">C</th>', out)
        self.assertIn('<tr role="row"><td role="cell" data-label="A">1</td>', out)

    def test_a_photo_only_at_the_top_level(self):
        import site_md
        lf = chr(10)
        hook = lambda alt, t, c="": "<FIG>"  # noqa: E731
        for bad in ("- Item." + lf + lf + "  ![a](x-1.jpg)" + lf, "> [!TIP]" + lf + "> ![a](x-1.jpg)" + lf):
            with self.assertRaises(SystemExit) as e:
                site_md.render(bad, "t.md", lambda t: t, None, hook)
            self.assertIn("top level", str(e.exception))

    def test_note_and_tip_are_the_quiet_callout(self):
        lf = chr(10)
        _, out, _, _ = self.render("> [!TIP]" + lf + "> Try it." + lf + lf + "> [!NOTE]" + lf + "> Mind it." + lf)
        self.assertEqual(out.count('<div class="note info" role="note">'), 2, out)
        self.assertIn("<b>Tip</b><p>Try it.</p>", out)
        self.assertIn("<b>Note</b><p>Mind it.</p>", out)


class Installer(unittest.TestCase):
    """/install/: its script and the vendored bundle only there, versions.json / boards.json in step, SOURCES.md pins
    the bundle, the erase box unticked, --local-firmware lands in the build only."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = pathlib.Path(cls.tmp.name) / "site"
        cls.files, _ = b.emit(cls.out)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_the_installer_checks_pass(self):
        self.assertEqual(b.installer_checks(self.files), [])

    def test_script_and_bundle_only_on_install(self):
        inst = [k for k in self.files if k.startswith("assets/install-") and k.endswith(".js")]
        self.assertEqual(len(inst), 1)
        for p, d in b.site_pages(self.files).items():
            self.assertEqual(("/" + inst[0]) in d, p == "install/index.html", p)
            self.assertNotIn("esptool-bundle", d, p)  # the page never names it: install.js imports it on Connect
        self.assertIn(b"/assets/esptool-bundle.js", self.files[inst[0]])
        files = dict(self.files)
        files["404.html"] = files["404.html"].replace(b"</main>", b'<script type="module" src="/assets/esptool-bundle.js"></script></main>')
        self.assertTrue(any("404.html" in e for e in b.installer_checks(files)))

    def test_sources_pins_the_bundle(self):
        rows = b.sources_rows()
        bundle = (b.ROOT / "site" / b.BUNDLE).read_bytes()
        self.assertEqual(rows["site/" + b.BUNDLE], hashlib.sha256(bundle).hexdigest(),
                         "site/SOURCES.md must carry the bundle's SHA-256 (and .gitattributes keeps it -text)")
        self.assertEqual(self.files[b.BUNDLE], bundle)
        notices = self.files["install/third-party-notices.txt"].decode("utf-8")
        for words in ("esptool-js 0.7.1", "Apache License", "pako 2.1.0", "Jean-loup Gailly", "atob-lite"):
            self.assertIn(words, notices)
        self.assertIn("site/assets/esptool-bundle.js -text", (b.ROOT / ".gitattributes").read_text(encoding="utf-8"))

    def test_versions_and_boards_cross_reference(self):
        import json
        doc = json.loads(self.files["firmware/versions.json"])
        boards = json.loads(b.BOARDS.read_text(encoding="utf-8"))["boards"]
        self.assertEqual(set(doc["boards"]), set(boards))
        for edit, words in ((lambda d: d["boards"]["esp32-wroom-32-4mb"].update(label="Other"), "label differs"),
                            (lambda d: d["boards"]["esp32-wroom-32-4mb"].update(photo_size=[300, 400]), "photo_size"),
                            (lambda d: d["boards"]["esp32-wroom-32-4mb"]["versions"].insert(0, {
                                "version": "9.9.9", "url": "/firmware/9.9.9/x.zip", "sha256": "0" * 64, "date": None,
                                "prerelease": False}), "not built"),
                            (lambda d: d["boards"]["esp32-wroom-32-4mb"]["versions"].insert(0, {
                                "version": "9.9.9", "url": "http://x.example/x.zip", "sha256": "0" * 64, "date": None,
                                "prerelease": False}), "neither same-origin nor https"),
                            (lambda d: d["boards"].update(extra=dict(d["boards"]["esp32-wroom-32-4mb"])), "differ from boards.json")):
            d = json.loads(self.files["firmware/versions.json"])
            edit(d)
            files = dict(self.files, **{"firmware/versions.json": json.dumps(d).encode()})
            self.assertTrue(any(words in e for e in b.installer_checks(files)), words)

    def test_the_erase_box_is_unticked_and_says_what_it_does(self):
        page = self.files["install/index.html"].decode("utf-8")
        import re
        box = re.search(r'<input type="checkbox" id="fl-erase"[^>]*>', page).group(0)
        self.assertNotIn("checked", box)
        self.assertIn("Erase everything, settings included: for a clean start only", page)
        js = b.INSTALL_JS.read_text(encoding="utf-8")
        self.assertIn("eraseAll: erase, compress: true", js)
        self.assertIn("erase = env.erase()", js)
        self.assertNotIn("eraseAll: true", js)

    def test_local_firmware_lands_in_the_build_only(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        import test_release_assets as TR
        before = sorted(p.relative_to(b.FIRMWARE).as_posix() for p in b.FIRMWARE.rglob("*"))
        placeholder = (b.FIRMWARE / "versions.json").read_bytes()
        with tempfile.TemporaryDirectory() as t:
            fw = pathlib.Path(t) / "fw"
            fw.mkdir()
            parts = {"fauxcloud.ino.bin": TR.image("1.2.3-dev.1"), "fauxcloud.ino.bootloader.bin": b"\xe9" * 2000,
                     "fauxcloud.ino.partitions.bin": TR.table_bin(b.PARTITIONS_CSV.read_text(encoding="utf-8")),
                     "boot_app0.bin": b"\xff" * 0x2000, "flash_args": TR.FLASH_ARGS.encode()}
            for n, data in parts.items():
                (fw / n).write_bytes(data)
            out = pathlib.Path(t) / "site"
            files, _ = b.emit(out, fw)
            self.assertIn("firmware/local/fauxcloud-1.2.3-dev.1-factory.zip", files)
            import json
            v = json.loads(files["firmware/versions.json"])["boards"]["esp32-wroom-32-4mb"]["versions"]
            self.assertEqual((v[0]["version"], v[0]["local"], v[0]["url"]),
                             ("1.2.3-dev.1", True, "/firmware/local/fauxcloud-1.2.3-dev.1-factory.zip"))
            self.assertEqual(b.installer_checks(files), [])
            self.assertTrue((out / "firmware" / "local" / "fauxcloud-1.2.3-dev.1-factory.zip").is_file())
            with self.assertRaises(SystemExit):  # never into a committed folder of site/
                b.main(["--local-firmware", str(fw), "--out", str(b.FIRMWARE / "x")])
            for inside in (b.ROOT / "tools" / "x", b.ROOT / "docs" / "x"):  # nor anywhere else in the repository
                with self.assertRaises(SystemExit):
                    b.main(["--local-firmware", str(fw), "--out", str(inside)])
                self.assertFalse(inside.exists())
            (fw / "sketch").mkdir()  # a build with the developer's secrets.h seed is refused
            (fw / "sketch" / "secrets.h").write_bytes(b"")
            with self.assertRaises(SystemExit) as e:
                b.emit(pathlib.Path(t) / "seeded", fw)
            self.assertIn("without secrets.h", str(e.exception))
        self.assertEqual(sorted(p.relative_to(b.FIRMWARE).as_posix() for p in b.FIRMWARE.rglob("*")), before)
        self.assertEqual((b.FIRMWARE / "versions.json").read_bytes(), placeholder)
        self.assertNotIn("local", placeholder.decode("utf-8").split('"boards"')[1])

    def test_the_site_workflow_lists_the_releases_and_never_the_local_build(self):
        yml = (b.ROOT / ".github" / "workflows" / "pages.yml").read_text(encoding="utf-8")
        self.assertNotIn("--local-firmware", yml)
        self.assertLess(yml.index("tools/site_versions.py"), yml.index("tools/build_site.py"))
        self.assertIn('--mirror "$RUNNER_TEMP/firmware"', yml)
        # versions -> build -> leak gate -> the zips join -> the full check of the uploaded folder -> upload
        order = ["tools/site_versions.py", "python -I tools/build_site.py\n", "tools/leakgate.py --tree site/_build",
                 'cp -r "$RUNNER_TEMP/firmware/." site/_build/firmware/',
                 'build_site.py --check --firmware-extra "$RUNNER_TEMP/firmware" --served site/_build',
                 "upload-pages-artifact"]
        at = [yml.index(x) for x in order]
        self.assertEqual(at, sorted(at), order)

    def test_the_served_check_sees_the_mirrored_zips(self):
        import json
        with tempfile.TemporaryDirectory() as t:
            t = pathlib.Path(t)
            extra = t / "fw"
            (extra / "1.0.0").mkdir(parents=True)
            data = b"PK a release zip stand-in"
            (extra / "1.0.0" / "fauxcloud-1.0.0-factory.zip").write_bytes(data)
            row = {"version": "1.0.0", "url": "/firmware/1.0.0/fauxcloud-1.0.0-factory.zip",
                   "sha256": hashlib.sha256(data).hexdigest(), "size": len(data), "date": "2026-12-01", "prerelease": False}
            doc = json.loads((b.FIRMWARE / "versions.json").read_text(encoding="utf-8"))
            doc["boards"]["esp32-wroom-32-4mb"]["versions"] = [row]
            real = b.FIRMWARE
            fake = t / "firmware"
            fake.mkdir()
            (fake / "versions.json").write_text(json.dumps(doc), encoding="utf-8")
            b.FIRMWARE = fake
            try:
                served = t / "served"
                files, _ = b.emit(served, None, extra)
                self.assertEqual(b.installer_checks(files), [])
                self.assertEqual(b.served_diff(files, served), [])
                (served / "firmware" / "1.0.0" / "fauxcloud-1.0.0-factory.zip").write_bytes(data + b"x")
                (served / "extra.txt").write_text("x")
                diff = b.served_diff(files, served)
                self.assertTrue(any("differs" in e for e in diff) and any("extra.txt" in e for e in diff), diff)
                for edit, words in (({"size": len(data) + 1}, "the list says"), ({"sha256": "0" * 64}, "SHA-256"),
                                    ({"size": None}, "no size")):
                    bad = dict(files)
                    d2 = json.loads(json.dumps(doc))
                    d2["boards"]["esp32-wroom-32-4mb"]["versions"][0].update(edit)
                    bad["firmware/versions.json"] = json.dumps(d2).encode()
                    self.assertTrue(any(words in e for e in b.installer_checks(bad)), words)
                files2, _ = b.emit(t / "nozip")
                self.assertTrue(any("is not built" in e for e in b.installer_checks(files2)))
            finally:
                b.FIRMWARE = real


if __name__ == "__main__":
    unittest.main()
