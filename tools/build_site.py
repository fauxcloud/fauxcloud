"""build_site.py -- the static site build (fauxcloud.org), into site/_build/ (gitignored).

Two halves, one output folder:
  1. The /try/ demo is the shipped dashboard, never a copy: this build imports tools/gen_web_assets.py and emits the
     bytes a board of the same commit serves, decompressed for a static host:
       - /a/<hash8>/<name> for every asset of the header, at the same hashed path (the hash of the gzip bytes);
       - /try/index.html and /try/admin.html = page() of web/index.html and web/admin.html with exactly two added
         blocks: DEMO_TAG before the first bundle script (the shim must be installed before the page scripts run)
         and the share-card tags (og_tags) right before </head>;
       - /try/demo.js and /try/demo.css (the extras, site/try/), /try/replay.js (demo/replay.js as it is) and
         /try/data/ (the fixtures the shim reads, copied as they are: index.json, history/, learn/, static/, admin/).
  2. The site's own pages, from the templates in site/templates/ and the public Markdown (docs/*.md, SECURITY.md,
     CONTRIBUTING.md, CHANGELOG.md), at clean URLs: / (the landing), /docs/ (all pages), /docs/<slug>/, /install/,
     /404.html, /sitemap.xml (every page meant for search, lastmod from git); one stylesheet (site/tokens.css + site/site.css), one script (site/site.js), the images of
     site/assets/ and the favicon of the dashboard. /install/ alone also gets site/install/install.js and, through it,
     the vendored /assets/esptool-bundle.js (site/SOURCES.md); /firmware/ is site/firmware/ (versions.json, the list
     the installer reads, and any release zips tools/site_versions.py --mirror copied there); /robots.txt and
     /.well-known/security.txt are site/robots.txt and site/.well-known/security.txt as they are. site/tokens.css is
     generated from the dashboard's app.css (its dash-face token blocks) and committed; a drift is a failing --check.
The check compares the emitted shells and assets with page() and with the committed header (web_assets.h), so a demo
that drifts from the firmware's pages is a failing build; it also runs the site checks: every internal link and
anchor resolves, every page carries the share card and its search tags (the sitemap lists exactly the pages meant for
search), no internal or private words, the budgets of site/README.md.

Usage (stdlib only, run isolated; needs the generator's node + terser + zopfli, see firmware/README.md):
    python -I tools/build_site.py            # build site/_build/ (and refresh site/tokens.css)
    python -I tools/build_site.py --check    # CI: build into a temporary folder, compare and check, exit 1 on a finding
    python -I tools/build_site.py --out DIR  # build into DIR: absent, empty, or an earlier build of this tool (its marker)
    python -I tools/build_site.py --links    # print the external links of the build
    python -I tools/build_site.py --local-firmware [DIR]   # + /firmware/local/: this computer's build as an installable
                                             # version (DIR: firmware/fauxcloud/build/default/); a dev preview only
Serve locally: python -m http.server -d site/_build 8000, then open http://localhost:8000/
"""
import argparse
import datetime
import gzip
import hashlib
import html
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS = pathlib.Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))  # python -I does not put the script's folder on the path
import gen_web_assets as g  # noqa: E402
import release_assets  # noqa: E402
import site_md  # noqa: E402

ROOT = g.ROOT
SITE = ROOT / "site"
OUT = SITE / "_build"
TRY_SRC = SITE / "try"
TEMPLATES = SITE / "templates"
ASSETS = SITE / "assets"
TOKENS = SITE / "tokens.css"
INSTALL_JS = SITE / "install" / "install.js"
BUNDLE = "assets/esptool-bundle.js"  # vendored (site/SOURCES.md): loaded by install.js on /install/ only
SOURCES = SITE / "SOURCES.md"
NOTICES = SITE / "THIRD_PARTY_NOTICES_SITE.txt"
# served at /.well-known/security.txt (RFC 9116); its Expires is renewed before it passes (--check refuses an expired one)
SECURITY_TXT = SITE / ".well-known" / "security.txt"
ROBOTS = SITE / "robots.txt"  # allow all; served at /robots.txt
FIRMWARE = SITE / "firmware"  # versions.json (a placeholder until the first release) + mirrored release zips
BOARDS = SITE / "boards.json"
LOCAL_FW = ROOT / "firmware" / "fauxcloud" / "build" / "default"
PARTITIONS_CSV = ROOT / "firmware" / "fauxcloud" / "partitions.csv"
FIXTURES = ROOT / "demo" / "fixtures"
REPLAY = ROOT / "demo" / "replay.js"
HEADER = g.OUT
# the one line the build adds to each shell before its bundles (page() output is one tag per line)
DEMO_TAG = b'<script src="/try/demo.js"></script>\n'
# the shells of the demo: source page -> the emitted path under the build root
SHELLS = {"index.html": "try/index.html", "admin.html": "try/admin.html"}
# the extras, copied as they are: build path -> source
EXTRAS = {"try/demo.js": TRY_SRC / "demo.js", "try/demo.css": TRY_SRC / "demo.css", "try/replay.js": REPLAY}
# the fixture folders the shim reads (snap/ holds the replay test snapshots: the shim derives every body itself)
FIXTURE_PARTS = ["index.json", "history", "learn", "static", "admin"]
# written at the root of every build: the one sign that a folder may be emptied for the next build (a given path that is
# not empty and lacks it is refused, never deleted)
MARKER = ".fauxcloud-site-build"
FIRST_BUNDLE = re.compile(rb'^<script src="/a/[0-9a-f]{8}/[A-Za-z0-9_.-]+"', re.M)

SITE_URL = "https://fauxcloud.org"
REPO_URL = "https://github.com/fauxcloud/fauxcloud"
TAGLINE = "local weather bridge for Wi-Fi weather stations"
SOCIAL = "assets/social.png"  # 1280 x 640, made from a private source (site/README.md, "The share card")
NAV = [("Get started", "/docs/get-started/"), ("Install", "/install/"), ("Compatibility", "/docs/compatibility/"),
       ("Home Assistant", "/docs/home-assistant/"), ("Docs", "/docs/"), ("Demo", "/try/"), ("GitHub", REPO_URL)]
GROUPS = ["Start", "Boards", "Use it", "Home Assistant", "Online, if you want it", "Help and reference", "Project"]
# the docs pages in reading order: slug, source (repo path), sidebar group, and the title / lede of a file that has no
# front matter (the root files stay plain GitHub Markdown)
DOCS = [
    ("get-started", "docs/get-started.md", "Start", None, None),
    ("flash", "docs/flash.md", "Start", None, None),
    ("boards", "docs/boards.md", "Boards", None, None),
    ("dashboard", "docs/dashboard.md", "Use it", None, None),
    ("outlook", "docs/outlook.md", "Use it", None, None),
    ("home-assistant", "docs/home-assistant.md", "Home Assistant", None, None),
    ("online-services", "docs/online-services.md", "Online, if you want it", None, None),
    ("airport-weather", "docs/airport-weather.md", "Online, if you want it", None, None),
    ("troubleshooting", "docs/troubleshooting.md", "Help and reference", None, None),
    ("compatibility", "docs/station-profiles.md", "Help and reference", "Compatibility",
     "If your console's setup page has a Weather Underground box, it may work (not tested yet; a console report tells "
     "us). If it pairs only through an app, it will not."),
    ("security", "SECURITY.md", "Project", "Security", "How to report a vulnerability, and what is in scope."),
    ("contributing", "CONTRIBUTING.md", "Project", "Contributing",
     "How to help: console reports, bug reports and code, and the checks a change passes."),
    ("plans", "docs/plans.md", "Project", None, None),
    ("changelog", "CHANGELOG.md", "Project", "Changelog", "What changed in each release."),
]
# the search-result text of a page whose lede is too long or too vague for one (<= 160 characters, a searcher's words;
# vendor names only to describe what works); every other docs page uses its lede
DESCRIPTIONS = {
    "home-assistant": "Get a Wi-Fi weather station such as the Vevor 7-in-1 into Home Assistant locally, with no cloud: MQTT "
                      "discovery makes a device with wind, rain and pressure.",
    "online-services": "Send your readings to Windy, Weather Underground and other weather services (up to three) and show "
                       "Weather Underground's forecast. Off until you turn them on.",
    "airport-weather": "Show the official report (METAR) and forecast (TAF) of up to three nearby airports beside your own "
                       "readings, and see when your console's pressure looks off.",
    "compatibility": "Which Wi-Fi weather stations work with fauxcloud. Verified on one Vevor 7-in-1 (YT60234); a console "
                     "whose setup page has a Weather Underground box may work.",
    "security": "How to report a security problem in fauxcloud, and what is in scope.",
    "changelog": "What changed in each fauxcloud release, newest first.",
}
# search limits the site checks hold every page to (longer ones are cut in a result)
TITLE_MAX, DESC_MAX = 60, 160
# the docs' own URLs that are not a page of the table: the root opens Get started (a copy, its canonical URL names the
# page), the site map has its own address
DOCS_ROOT, ALL_PAGES = "get-started", "/docs/all-pages/"
# a page of few sections with many sub-sections (Boards, Compatibility) lists its h3s in the sidebar too
H3_SIDEBAR = {"max_h2": 3, "min_h3": 5}
# the hosts the site may link to; a new one is a deliberate change here (the link check refuses any other)
HOSTS = {"developercertificate.org", "docs.espressif.com", "github.com", "keepachangelog.com", "makerworld.com",
         "semver.org", "stations.windy.com", "wow.meteo.be", "www.pwsweather.com", "www.silabs.com", "www.wch-ic.com",
         "www.wunderground.com"}
# a docs photo's slot (site.css, "docs photos"): a portrait 200 px wide, a landscape 340 px or the column on a phone
PHOTO_SIZES = {"tall": "200px", "wide": "(min-width: 600px) 340px, calc(100vw - 32px)"}
# pages whose h2s are not sections to list (the changelog's are its versions)
NO_SECTIONS = {"changelog"}
# never a source of the site, whatever a table or a link says
# (the private folder names are joined from parts, so this file does not spell the paths the leak gate looks for)
PRIVATE = ("docs/" + "internal", "_" + "inbox", "cap" + "tures")
REFUSED = re.compile(r"(^|/)(%s)(/|$)|(^|/)secrets[^/]*$" % "|".join(PRIVATE))
# budgets (site/README.md): the landing's and the installer's HTML + CSS + JS gzipped (the vendored bundle apart), and
# the images one page loads at most
LANDING_GZ_MAX = 30 * 1024
INSTALL_GZ_MAX = 30 * 1024
PAGE_IMAGES_MAX = 450 * 1024
# words and patterns that never reach a public page of the site (the /try/ shells are the firmware's own bytes)
LEAKS = [
    (re.compile("|".join(PRIVATE[:2]) + r"|\b" + PRIVATE[2] + "/"), "an internal path"),
    (re.compile(r"\bD\d{1,2}\b"), "a decision number"),
    (re.compile(r"\b(ER|UX-|R-)\d{1,3}\b|\bb\d{1,2}(\.\d+)+\b"), "an internal id"),
    (re.compile(r"\bowner\b", re.I), "the word 'owner'"),
    (re.compile(r"\bsupervisor\b|\bbriefs?/", re.I), "a process word"),
    (re.compile("[" + "".join(map(chr, (0x105, 0x107, 0x119, 0x142, 0x144, 0x15B, 0x17A, 0x17C, 0x104, 0x106, 0x118, 0x141,
                                         0x143, 0x15A, 0x179, 0x17B))) + "]"), "Polish text"),
]
NON_AFFILIATION = "not affiliated with or endorsed by any company or service named on this site"
SHOPS = re.compile(r"(amazon\.|aliexpress|ebay\.|allegro\.|temu\.|vevor\.|succebuy|banggood|alibaba)", re.I)


# ---------------------------------------------------------------- the /try/ demo
def og_tags(path, title, description):
    """The share card of one page (Open Graph + the large-image card): absolute URLs, as the scrapers want them."""
    e = lambda s: html.escape(s, quote=True)  # noqa: E731
    return ('<meta property="og:type" content="website">\n'
            '<meta property="og:site_name" content="fauxcloud">\n'
            '<meta property="og:title" content="%s">\n'
            '<meta property="og:description" content="%s">\n'
            '<meta property="og:url" content="%s%s">\n'
            '<meta property="og:image" content="%s/%s">\n'
            '<meta property="og:image:width" content="1280">\n'
            '<meta property="og:image:height" content="640">\n'
            '<meta property="og:image:alt" content="fauxcloud: %s">\n'
            '<meta name="twitter:card" content="summary_large_image">\n'
            % (e(title), e(description), SITE_URL, path, SITE_URL, SOCIAL, TAGLINE))


TRY_TITLE = "fauxcloud demo: the dashboard on demo data"
TRY_DESC = "Click through the fauxcloud dashboard and settings page in your browser, on demo data. No board needed."
# the demo's page for search; its settings page is reached from it and kept out of results (noindex)
TRY_INDEXED = {"try/index.html"}


def try_block(dest):
    """The second block the build adds to a shell, right before </head>: the search tags (description, canonical or
    noindex) and the share card. The shells are the firmware's bytes, so the build adds what a search needs here."""
    path = "/" + dest.replace("index.html", "")
    seo = '<meta name="description" content="%s">\n' % html.escape(TRY_DESC, quote=True)
    seo += ('<link rel="canonical" href="%s%s">\n' % (SITE_URL, path) if dest in TRY_INDEXED
            else '<meta name="robots" content="noindex">\n')
    return (seo + og_tags(path, TRY_TITLE, TRY_DESC)).encode("utf-8")


TRY_OG = {dest: try_block(dest) for dest in SHELLS.values()}


def with_tag(shell, og=b""):
    """The shell with DEMO_TAG on its own line before the first bundle script, and `og` right before </head>."""
    m = FIRST_BUNDLE.search(shell)
    if not m:
        raise SystemExit("build_site: no bundle script in a shell")
    shell = shell[:m.start()] + DEMO_TAG + shell[m.start():]
    if og:
        if shell.count(b"</head>") != 1:
            raise SystemExit("build_site: a shell without exactly one </head>")
        shell = shell.replace(b"</head>", og + b"</head>", 1)
    return shell


def bare(shell, dest):
    """A demo shell minus the two added blocks: what page() must have made."""
    return shell.replace(DEMO_TAG, b"", 1).replace(TRY_OG[dest], b"", 1)


def fixture_files():
    """[(relative path under data/, source path)] in a stable order."""
    out = []
    for part in FIXTURE_PARTS:
        src = FIXTURES / part
        if src.is_file():
            out.append((part, src))
        elif src.is_dir():
            out += [(f.relative_to(FIXTURES).as_posix(), f) for f in sorted(src.rglob("*")) if f.is_file()]
        else:
            raise SystemExit("build_site: missing fixture %s" % src)
    return out


def clear(out):
    """Empty `out` for a build: only an absent or empty folder, or one holding MARKER; anything else is refused."""
    if not out.exists():
        return
    if not out.is_dir():
        raise SystemExit("build_site: %s is a file, not a build folder" % out)
    if any(out.iterdir()) and not (out / MARKER).is_file():
        raise SystemExit("build_site: %s is not empty and holds no %s: not a build of this tool, so it is left as it is"
                         " (pick an empty or new folder)" % (out, MARKER))
    shutil.rmtree(out)


# ---------------------------------------------------------------- tokens
def css_block(text, head):
    """The `head { ... }` block that starts a line of `text`, braces matched, as written."""
    m = re.search(r"^" + re.escape(head) + r"\s*\{", text, re.M)
    if not m:
        raise SystemExit("build_site: app.css has no %s block in the dash face" % head)
    depth, i = 0, m.end() - 1
    while i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[m.start():i + 1]
        i += 1
    raise SystemExit("build_site: unbalanced %s block in app.css" % head)


def tokens():
    """site/tokens.css: the dash face's token blocks of app.css, dark (:root) and light ([data-theme="light"])."""
    dash = g.css_sections()["dash"].replace("\r\n", "\n")
    return ("/* tokens.css -- GENERATED by tools/build_site.py from firmware/fauxcloud/web/app.css (face dash): the dark\n"
            "   palette (:root) and the light one ([data-theme=\"light\"]). Do not edit; --check fails on a drift. */\n"
            + css_block(dash, ":root") + "\n" + css_block(dash, ':root[data-theme="light"]') + "\n")


# ---------------------------------------------------------------- the site's pages
def template(name):
    return (TEMPLATES / name).read_text(encoding="utf-8").replace("\r\n", "\n")


def fill(text, values):
    """{{> partial}} includes, then {{key}} values; a placeholder left over is a build error."""
    text = re.sub(r"\{\{> ([a-z0-9-]+)\}\}", lambda m: template(m.group(1) + ".html").rstrip("\n"), text)
    keys = set(re.findall(r"\{\{([a-z0-9-]+)\}\}", text))
    if keys != set(values):
        raise SystemExit("build_site: placeholders %s vs values %s" % (sorted(keys), sorted(values)))
    # one pass, so a value (a docs page with {{ }} in a code block) is never read as a placeholder
    return re.sub(r"\{\{([a-z0-9-]+)\}\}", lambda m: values[m.group(1)], text)


def nav(path):
    def current(href):
        if href == path:
            return True
        return href == "/docs/" and path.startswith("/docs/") and path not in [h for _, h in NAV]
    return "".join('<a href="%s"%s>%s</a>' % (h, ' aria-current="page"' if current(h) else "", t) for t, h in NAV)


def jpeg_size(data):
    """(width, height) of a JPEG from its first frame header (SOF0-SOF15 except DHT, JPG and DAC)."""
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            raise SystemExit("build_site: not a JPEG frame marker at %d" % i)
        marker, seg = data[i + 1], int.from_bytes(data[i + 2:i + 4], "big")
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            return int.from_bytes(data[i + 7:i + 9], "big"), int.from_bytes(data[i + 5:i + 7], "big")
        i += 2 + seg
    raise SystemExit("build_site: no JPEG frame header")


def repo_source(rel):
    rel = rel.replace("\\", "/")
    if REFUSED.search(rel):
        raise SystemExit("build_site: %s is never a source of the site" % rel)
    return ROOT / rel


class Docs:
    """The docs pages: parsed once, then rendered with the sidebar and previous / next."""

    def __init__(self):
        self.by_source = {src: slug for slug, src, *_ in DOCS}
        self.pages = []
        for slug, src, group, title, lede in DOCS:
            path = repo_source(src)
            if not path.is_file():
                raise SystemExit("build_site: missing docs source %s" % src)
            outline = []
            meta, body, toc, ids = site_md.render(path.read_text(encoding="utf-8"), src, lambda t, s=src: self.link(s, t),
                                                  outline, lambda a, t, c="", s=src: self.figure(s, a, t, c))
            self.pages.append({"slug": slug, "src": src, "group": group, "title": title or meta.get("title") or slug,
                               "lede": lede or meta.get("lede", ""), "body": body, "toc": toc, "ids": ids,
                               "outline": outline, "url": "/docs/%s/" % slug})

    def link(self, source, target):
        """A Markdown link target -> an href: docs files to their pages, other repo files to GitHub, the site to /."""
        if target.startswith(SITE_URL + "/") or target == SITE_URL:
            return target[len(SITE_URL):] or "/"
        if re.match(r"^[a-z]+:", target) or target.startswith("#") or target.startswith("/"):
            return target
        path, _, frag = target.partition("#")
        rel = (pathlib.PurePosixPath(source).parent / path).as_posix()
        parts = []
        for p in rel.split("/"):
            if p == "..":
                if not parts:
                    raise SystemExit("build_site: %s links outside the repository: %s" % (source, target))
                parts.pop()
            elif p not in ("", "."):
                parts.append(p)
        rel = "/".join(parts)
        if REFUSED.search(rel):
            raise SystemExit("build_site: %s links to a private path: %s" % (source, target))
        if rel in self.by_source:
            return "/docs/%s/" % self.by_source[rel] + ("#" + frag if frag else "")
        if not (ROOT / rel).exists():
            raise SystemExit("build_site: %s links to a missing file: %s" % (source, target))
        kind = "tree" if (ROOT / rel).is_dir() else "blob"
        return "%s/%s/main/%s" % (REPO_URL, kind, rel) + ("#" + frag if frag else "")

    def figure(self, source, alt, target, caption=""):
        """A docs photo: the Markdown names one variant under site/assets/ (GitHub shows that file); the page gets every
        width of it, the file's own size, and a narrow portrait or a wider landscape slot."""
        m = re.match(re.escape(REPO_URL) + r"/blob/main/site/assets/([a-z]+(?:-[a-z]+)*)-\d+\.jpg$", self.link(source, target))
        if not m:
            raise SystemExit("build_site: %s: a photo must be a variant under site/assets/: %s" % (source, target))
        name = m.group(1)
        vs = sorted((int(f.stem.rsplit("-", 1)[1]), f) for f in (ROOT / "site" / "assets").glob(name + "-*.jpg")
                    if re.fullmatch(re.escape(name) + r"-\d+", f.stem))
        w, h = jpeg_size(vs[0][1].read_bytes())
        tall = h > w
        return ('<figure class="ph%s"><img src="/assets/%s" srcset="%s" sizes="%s" width="%d" height="%d" loading="lazy" '
                'decoding="async" alt="%s">%s</figure>' % (
                    " tall" if tall else "", vs[0][1].name, ", ".join("/assets/%s %dw" % (f.name, vw) for vw, f in vs),
                    PHOTO_SIZES["tall" if tall else "wide"], w, h, html.escape(alt, quote=True),
                    "<figcaption>%s</figcaption>" % caption if caption else ""))

    def sidebar(self, current, here=None):
        """A label and a list per group. A group named like one of its pages (Boards, Home Assistant) has no label:
        that page's row says it. The label of the current page's group (`here`, the phone sheet's summary) is marked,
        so the sheet can hide it where it would sit right under that summary (site.css)."""
        out = []
        for grp in GROUPS + ["Index"]:
            items = [p for p in self.pages if p["group"] == grp]
            if grp == "Index":
                items = [{"url": ALL_PAGES, "title": "All pages"}]
            if not items:
                continue
            label = "" if any(p["title"] == grp for p in items) else "        <h4%s>%s</h4>\n" % (
                ' class="here"' if grp == here else "", grp)
            out.append("%s        <ul>%s</ul>" % (label, "".join(self.entry(p, current) for p in items)))
        return "\n".join(out)

    def entry(self, p, current):
        """One sidebar row; the current page's row opens to its sections (h2s), static, no script."""
        if p["url"] != current:
            return '<li><a href="%s">%s</a></li>' % (p["url"], html.escape(p["title"]))
        a = lambda i, t: '<a href="#%s" title="%s">%s</a>' % (i, html.escape(t, quote=True), html.escape(t))  # noqa: E731
        secs = "" if p.get("slug") in NO_SECTIONS else "".join('<li>%s%s</li>' % (a(i, t), "<ul>%s</ul>" % "".join(
            "<li>%s</li>" % a(j, u) for j, u in subs) if subs else "") for i, t, subs in self.sections(p))
        return '<li><a href="%s" aria-current="page">%s</a>%s</li>' % (
            p["url"], html.escape(p["title"]), '<ul class="sub" aria-label="On this page">%s</ul>' % secs if secs else "")

    @staticmethod
    def sections(p):
        """[(id, text, [(id, text)])]: the page's h2s, with their h3s when the page has few h2s and many h3s."""
        ol = p.get("outline", [])
        h2 = sum(1 for lv, _, _ in ol if lv == 2)
        deep = h2 <= H3_SIDEBAR["max_h2"] and len(ol) - h2 >= H3_SIDEBAR["min_h3"]
        out = []
        for lv, i, t in ol:
            if lv == 2:
                out.append((i, t, []))
            elif deep and out:
                out[-1][2].append((i, t))
        return out

    def ordered(self):
        return sorted(self.pages, key=lambda p: GROUPS.index(p["group"]))

    def page(self, p):
        seq = self.ordered()
        i = seq.index(p)
        pn = []
        if i > 0:
            pn.append('<div class="pv"><small>Previous</small><a class="btn quiet" href="%s">%s</a></div>'
                      % (seq[i - 1]["url"], html.escape(seq[i - 1]["title"])))
        if i + 1 < len(seq):
            pn.append('<div class="nx"><small>Next</small><a class="btn primary" href="%s">%s</a></div>'
                      % (seq[i + 1]["url"], html.escape(seq[i + 1]["title"])))
        return fill(template("docs.html"), {
            "group": html.escape(p["group"]), "sidebar": self.sidebar(p["url"], p["group"]), "heading": html.escape(p["title"]),
            "lede": '      <p class="lede">%s</p>' % site_md.Inline(lambda t: self.link(p["src"], t))(p["lede"]) if p["lede"] else "",
            "body": p["body"], "pn": '<nav class="pn" aria-label="Previous and next">%s</nav>' % "".join(pn)})

    def index(self):
        """The site map: a block per sidebar group, a row per page (title, description, its sections one per line)."""
        blocks = []
        for grp in GROUPS:
            items = [p for p in self.pages if p["group"] == grp]
            if not items:
                continue
            rows = []
            for p in items:
                secs = "" if p["slug"] in NO_SECTIONS else "".join(
                    '<li><a href="%s#%s">%s</a></li>' % (p["url"], i, html.escape(t)) for i, t in p["toc"])
                rows.append('<div class="row"><h3><a href="%s">%s</a></h3>%s%s</div>' % (
                    p["url"], html.escape(p["title"]), "<p>%s</p>" % html.escape(strip_md(p["lede"])) if p["lede"] else "",
                    '<ul class="secs">%s</ul>' % secs if secs else ""))
            blocks.append('<section class="map"><h2 id="%s">%s</h2>\n%s\n</section>' % (
                site_md.slugify(grp), html.escape(grp), "\n".join(rows)))
        return fill(template("docs.html"), {
            "group": "All pages", "sidebar": self.sidebar(ALL_PAGES), "heading": "All pages",
            "lede": '      <p class="lede">Every page of the documentation and its sections. Your browser\'s find (Ctrl+F) searches this list.</p>',
            "body": "\n".join(blocks), "pn": ""})


def strip_md(s):
    return re.sub(r"[*`]", "", re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", s))


def text_of(html_):
    return html.unescape(re.sub(r"<[^>]+>", " ", html_))


def shell_page(path, title, description, main, css, js, sprite, index=True, extra=""):
    """A page of the site. `index`: the page is for search (its canonical URL) or not (noindex); `extra`: more head tags."""
    head = ('<link rel="canonical" href="%s%s">' % (SITE_URL, path) if index else '<meta name="robots" content="noindex">')
    return fill(template("base.html"), {
        "title": html.escape(title), "description": html.escape(description, quote=True), "head": head + extra,
        "og": og_tags(path, title, description).rstrip("\n"), "css": css, "js": js, "sprite": sprite.rstrip("\n"),
        "nav": nav(path), "main": main})


LANDING_TITLE = "fauxcloud: " + TAGLINE
# the query words people use: an ESP32, Weather Underground, a Wi-Fi weather station by name, local, Home Assistant
LANDING_DESC = ("Free ESP32 firmware that stands in for Weather Underground: your Wi-Fi weather station, e.g. a Vevor "
                "7-in-1, uploads to it. Local dashboard and Home Assistant.")
# what the landing is, for search engines and AI answers (schema.org); no version, so it never goes stale
LANDING_LD = json.dumps({
    "@context": "https://schema.org", "@type": "SoftwareApplication", "name": "fauxcloud", "url": SITE_URL + "/",
    "description": LANDING_DESC, "applicationCategory": "HomeApplication", "operatingSystem": "ESP32",
    "license": "https://spdx.org/licenses/GPL-3.0-or-later.html", "sameAs": REPO_URL,
    "offers": {"@type": "Offer", "price": "0", "priceCurrency": "EUR"}}, separators=(",", ":")).replace("</", "<\\/")


def site_files():
    """{build path: bytes} of the site's own pages and files (everything but /a/ and /try/)."""
    files = {}
    css = g.minify_css(tokens() + (SITE / "site.css").read_text(encoding="utf-8"))
    js = g.terser((SITE / "site.js").read_text(encoding="utf-8"))
    css_b, js_b = css.encode("utf-8"), js.encode("utf-8")
    css_path = "/assets/site-%s.css" % hashlib.sha1(css_b).hexdigest()[:8]
    js_path = "/assets/site-%s.js" % hashlib.sha1(js_b).hexdigest()[:8]
    files[css_path[1:]] = css_b
    files[js_path[1:]] = js_b
    inst_b = g.terser(INSTALL_JS.read_text(encoding="utf-8")).encode("utf-8")
    inst_path = "/assets/install-%s.js" % hashlib.sha1(inst_b).hexdigest()[:8]
    files[inst_path[1:]] = inst_b
    sprite = template("sprite.svg")
    page = lambda path, title, desc, main, **kw: shell_page(path, title, desc, main, css_path, js_path, sprite, **kw).encode("utf-8")  # noqa: E731
    files["index.html"] = page("/", LANDING_TITLE, LANDING_DESC, fill(template("landing.html"), {}),
                               extra='\n<script type="application/ld+json">%s</script>' % LANDING_LD)
    files["install/index.html"] = page("/install/", "Install fauxcloud", "Install fauxcloud on an ESP32 board from "
                                       "Chrome or Edge on a computer, over USB: pick the board and the version, "
                                       "connect, install.", fill(template("install.html"), {"install-js": inst_path}))
    # site-wide (the footer links it on every page); the installer page keeps its own link to the same bytes
    files["third-party-notices.txt"] = NOTICES.read_bytes()
    files["install/third-party-notices.txt"] = NOTICES.read_bytes()
    files[".well-known/security.txt"] = SECURITY_TXT.read_bytes()
    files["robots.txt"] = ROBOTS.read_bytes()
    for f in sorted(FIRMWARE.rglob("*")):
        if f.is_file():
            files["firmware/" + f.relative_to(FIRMWARE).as_posix()] = f.read_bytes()
    docs = Docs()
    files[ALL_PAGES[1:] + "index.html"] = page(ALL_PAGES, "All pages · fauxcloud docs",
                                               "Every page of the fauxcloud documentation and its sections, in one list.",
                                               docs.index())
    for p in docs.pages:
        desc = DESCRIPTIONS.get(p["slug"]) or strip_md(p["lede"]) or text_of(p["body"]).strip()[:DESC_MAX]
        files["docs/%s/index.html" % p["slug"]] = page(p["url"], "%s · fauxcloud docs" % p["title"], " ".join(desc.split()),
                                                       docs.page(p))
    files["docs/index.html"] = files["docs/%s/index.html" % DOCS_ROOT]
    files["404.html"] = page("/404.html", "Page not found · fauxcloud", "This address does not lead to a page.",
                             template("notfound.html"), index=False)
    files["favicon.svg"] = (g.WEB / "favicon.svg").read_bytes()
    for f in sorted(ASSETS.iterdir()):
        if f.is_file():
            files["assets/" + f.name] = f.read_bytes()
    if SOCIAL not in files:
        raise SystemExit("build_site: missing site/%s (the share card)" % SOCIAL)
    files[SITEMAP] = sitemap()
    return files


SITEMAP = "sitemap.xml"


def sitemap_rows():
    """[(URL path, [the repo files its content comes from])]: every page meant for search, in reading order."""
    rows = [("/", [TEMPLATES / "landing.html"]), ("/install/", [TEMPLATES / "install.html", INSTALL_JS])]
    rows += [("/docs/%s/" % slug, [ROOT / src]) for slug, src, *_ in DOCS]
    rows += [(ALL_PAGES, [ROOT / src for _, src, *_ in DOCS]), ("/try/", [g.WEB / "index.html", TRY_SRC / "demo.js"])]
    return rows


def git_date(paths):
    """The newest commit date (YYYY-MM-DD) among `paths`, or None (no git, never committed). A shallow checkout gives
    its one commit's date, so the site workflow checks out the full history."""
    rels = [pathlib.Path(p).resolve().relative_to(ROOT).as_posix() for p in paths]
    try:
        r = subprocess.run(["git", "log", "-1", "--format=%cs", "--"] + rels, cwd=ROOT, capture_output=True, text=True,
                           timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    out = r.stdout.strip()
    return out if r.returncode == 0 and re.match(r"^\d{4}-\d{2}-\d{2}$", out) else None


def sitemap():
    """/sitemap.xml (sitemaps.org 0.9): absolute URLs, lastmod from git (the build date when git has none), no priorities."""
    today = datetime.datetime.now(datetime.timezone.utc).date().isoformat()
    rows = ["<url><loc>%s%s</loc><lastmod>%s</lastmod></url>" % (SITE_URL, path, git_date(srcs) or today)
            for path, srcs in sitemap_rows()]
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
            '%s\n</urlset>\n' % "\n".join(rows)).encode("utf-8")


def local_firmware(files, build_dir):
    """--local-firmware: this computer's build as the newest version of every board, for a rehearsal from the preview.
    The zip is made by release_assets.py from the build folder with the release's checks (marker version, table,
    layout) and lands in /firmware/local/ of the build only; the build's versions.json lists it first."""
    try:
        version, data = release_assets.factory_from_build(str(build_dir), str(PARTITIONS_CSV))
    except (release_assets.Fail, OSError) as e:
        raise SystemExit("build_site: --local-firmware: %s" % e)
    name = "fauxcloud-%s-factory.zip" % version
    files["firmware/local/" + name] = data
    doc = json.loads(files["firmware/versions.json"])
    row = {"version": version, "url": "/firmware/local/" + name, "sha256": hashlib.sha256(data).hexdigest(),
           "size": len(data), "date": None, "prerelease": False, "local": True}
    for b in doc["boards"].values():
        b["versions"] = [row] + [v for v in b["versions"] if v.get("url")]
    files["firmware/versions.json"] = json.dumps(doc, indent=2).encode("utf-8") + b"\x0a"
    return version


def emit(out, local=None, extra=None):
    """Build into `out` (see clear()). Returns ({build path: bytes} of everything written, the asset table).
    `local`: a firmware build folder to offer as /firmware/local/ (--local-firmware), never part of a deploy.
    `extra`: a folder whose files join /firmware/ (--firmware-extra: the release zips tools/site_versions.py --mirror
    put aside in the site workflow)."""
    clear(out)
    for src in EXTRAS.values():
        if not src.is_file():
            raise SystemExit("build_site: missing %s" % src)
    table = g.assets()
    files = {}
    for name, gzb, path, _ in table:
        files[path.lstrip("/")] = gzip.decompress(gzb)
    for page, dest in SHELLS.items():
        files[dest] = with_tag(g.page(g.WEB / page, table), TRY_OG[dest])
    for dest, src in EXTRAS.items():
        files[dest] = src.read_bytes()
    for rel, src in fixture_files():
        files["try/data/" + rel] = src.read_bytes()
    files.update(site_files())
    if extra is not None:
        if not extra.is_dir():
            raise SystemExit("build_site: --firmware-extra %s is not a folder" % extra)
        for f in sorted(extra.rglob("*")):
            if f.is_file():
                files["firmware/" + f.relative_to(extra).as_posix()] = f.read_bytes()
    if local is not None:
        print("build_site: /firmware/local/ = fauxcloud %s from %s (preview only)" % (local_firmware(files, local), local))
    out.mkdir(parents=True, exist_ok=True)
    (out / MARKER).write_text("built by tools/build_site.py; this folder is emptied by the next build\n", encoding="utf-8")
    for rel, data in files.items():
        p = out / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return files, table


# ---------------------------------------------------------------- the committed header, read back
ARRAY = re.compile(r"const uint8_t ([A-Z0-9_]+)\[\] = \{(.*?)\};", re.S)
ENTRY = re.compile(r'\{"(/a/[0-9a-f]{8}/[^"]+)", "([^"]+)", ([A-Z0-9_]+), (\d+)\},')


def header():
    """(assets {path: gzip bytes}, shells {page: gzip bytes}) from web_assets.h."""
    text = HEADER.read_text(encoding="utf-8")
    arrays = {n: bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", body)) for n, body in ARRAY.findall(text)}
    assets = {}
    for path, _, name, n in ENTRY.findall(text):
        if len(arrays[name]) != int(n):
            raise SystemExit("build_site: %s length differs from its table row" % name)
        assets[path] = arrays[name]
    shells = {p: arrays[g.c_name(pathlib.Path(p)) + "_GZ"] for p in SHELLS}
    return assets, shells


# ---------------------------------------------------------------- the site checks
URL_ATTR = re.compile(r'\s(?:href|src)="([^"]*)"')
SRCSET = re.compile(r'\ssrcset="([^"]*)"')


def urls(page):
    found = URL_ATTR.findall(page)
    for s in SRCSET.findall(page):
        found += [c.strip().split(" ")[0] for c in s.split(",") if c.strip()]
    return [html.unescape(u) for u in found]


def resolve(files, path):
    """The build path a URL path serves, or None."""
    p = path.lstrip("/")
    for cand in (p, p + "index.html" if p.endswith("/") or not p else p + "/index.html"):
        if cand in files:
            return cand
    return None


def site_pages(files):
    return {p: d.decode("utf-8") for p, d in files.items() if p.endswith(".html") and not p.startswith(("a/", "try/"))}


def link_check(files):
    """(errors, external links): every internal href/src resolves, every #anchor exists on its page."""
    errors, external = [], set()
    pages = {p: d.decode("utf-8", "replace") for p, d in files.items() if p.endswith(".html")}
    ids = {p: set(re.findall(r'\sid="([^"]+)"', d)) for p, d in pages.items()}
    for p, d in pages.items():
        for u in urls(d):
            if u.startswith(SITE_URL):
                u = u[len(SITE_URL):] or "/"
            if re.match(r"^(https?:)?//", u):
                external.add(u)
                host = re.match(r"^(?:https?:)?//([^/:?#]+)", u).group(1).lower()
                if host not in HOSTS:
                    errors.append("%s: a link to a host not in HOSTS: %s" % (p, u))
                if SHOPS.search(u):
                    errors.append("%s: a link to a shop: %s" % (p, u))
                continue
            if re.match(r"^[a-z]+:", u):
                errors.append("%s: an unexpected scheme: %s" % (p, u))
                continue
            path, _, frag = u.partition("#")
            path = path.split("?")[0]
            if p.startswith("try/") and not path.startswith(("/a/", "/try/")):
                continue  # the shells' own links (/admin, /) are the board's routes; the shim maps them in the demo
            if not path:
                target = p
            elif path.startswith("/"):
                target = resolve(files, path)
            else:
                errors.append("%s: a relative link (the site uses root paths): %s" % (p, u))
                continue
            if target is None:
                errors.append("%s: %s does not resolve" % (p, u))
                continue
            if frag and target in ids and not target.startswith("try/") and frag not in ids[target]:
                errors.append("%s: #%s is not an id of %s" % (p, frag, target))
    return errors, sorted(external)


def page_checks(files):
    errors = []
    for p, d in {**site_pages(files), **{k: files[k].decode("utf-8") for k in SHELLS.values()}}.items():
        for tag in ('property="og:title"', 'property="og:description"', 'property="og:image" content="%s/%s"' % (SITE_URL, SOCIAL),
                    'name="twitter:card" content="summary_large_image"'):
            if d.count(tag) != 1:
                errors.append("%s: the share card tag %s is missing or doubled" % (p, tag.split('"')[1]))
    for p, d in site_pages(files).items():
        if NON_AFFILIATION not in d:
            errors.append("%s: no non-affiliation line" % p)
        visible = text_of(re.sub(r"<svg.*?</svg>", " ", d, flags=re.S))
        attrs = " ".join(re.findall(r'(?:href|src|content|alt|title|aria-label)="([^"]*)"', d))
        for rx, what in LEAKS:
            for m in rx.finditer(visible + " " + attrs):
                errors.append("%s: %s: %r" % (p, what, m.group(0)))
        if re.search(r"https?://(?!fauxcloud\.org)[^\"'\s]*\.(css|js|woff2?|ttf)\b", d) or "<iframe src=\"http" in d:
            errors.append("%s: a resource from another host" % p)
    return errors


def own_url(p):
    """The URL path a build path is served at (a folder's index.html at the folder)."""
    return "/" + (p[:-len("index.html")] if p.endswith("index.html") else p)


def head_tags(page):
    """(titles, descriptions, canonical URLs, noindex?) of one page's head, unescaped."""
    head = page[:page.find("</head>")]
    un = html.unescape
    return ([un(t) for t in re.findall(r"<title[^>]*>(.*?)</title>", head, re.S)],
            [un(d) for d in re.findall(r'<meta name="description" content="([^"]*)">', head)],
            [un(c) for c in re.findall(r'<link rel="canonical" href="([^"]*)">', head)],
            bool(re.search(r'<meta name="robots" content="[^"]*noindex', head)))


def indexed_pages(files):
    """{URL path: build path} of the pages meant for search: their canonical URL is their own, no noindex."""
    out = {}
    for p in sorted(site_pages(files)) + sorted(SHELLS.values()):
        _, _, canon, noindex = head_tags(files[p].decode("utf-8"))
        if not noindex and canon == [SITE_URL + own_url(p)]:
            out[own_url(p)] = p
    return out


def seo_checks(files):
    """What a search engine reads: one title (<= 60) and one description (<= 160) per page, unique among the pages
    meant for search; a canonical URL that is served, or noindex (never both); the sitemap = the pages meant for search."""
    errors, seen = [], {}
    pages = {**site_pages(files), **{k: files[k].decode("utf-8") for k in SHELLS.values()}}
    indexed = indexed_pages(files)
    for p, d in sorted(pages.items()):
        titles, descs, canon, noindex = head_tags(d)
        if len(titles) != 1 or not titles[0].strip() or len(titles[0]) > TITLE_MAX:
            errors.append("%s: needs one title of at most %d characters (%s)" % (p, TITLE_MAX, titles))
        if len(descs) != 1 or not descs[0].strip() or len(descs[0]) > DESC_MAX:
            errors.append("%s: needs one description of at most %d characters (%s)" % (
                p, DESC_MAX, [len(x) for x in descs]))
        if len(canon) + noindex != 1:
            errors.append("%s: needs a canonical URL or noindex, exactly one of them" % p)
        for c in canon:
            if not c.startswith(SITE_URL + "/") or resolve(files, c[len(SITE_URL):]) is None:
                errors.append("%s: the canonical URL %s is not a page of the site" % (p, c))
        if own_url(p) in indexed:
            for kind, vals in (("title", titles), ("description", descs)):
                for v in vals:
                    if (kind, v) in seen:
                        errors.append("%s: the same %s as %s" % (p, kind, seen[(kind, v)]))
                    seen.setdefault((kind, v), p)
    listed = re.findall(r"<loc>%s([^<]*)</loc><lastmod>\d{4}-\d{2}-\d{2}</lastmod>" % re.escape(SITE_URL),
                        files.get(SITEMAP, b"").decode("utf-8"))
    if len(listed) != len(set(listed)) or set(listed) != set(indexed):
        errors.append("sitemap.xml: lists %s, the pages meant for search are %s" % (sorted(listed), sorted(indexed)))
    return errors


SECURITY_FIELDS = {"Acknowledgments", "Canonical", "Contact", "CSAF", "Encryption", "Expires", "Hiring", "Policy",
                   "Preferred-Languages"}


def security_txt_checks(text, now):
    """RFC 9116 shape of /.well-known/security.txt at `now` (an aware UTC datetime): one or more Contact (mailto: or
    https:), one Expires in the future and at most a year out, the Canonical URL of this site, known field names only."""
    errors, seen = [], {}
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        m = re.match(r"^([A-Za-z-]+): (\S.*)$", line)
        if not m or m.group(1) not in SECURITY_FIELDS:
            errors.append("security.txt line %d: not a known 'Field: value' line: %r" % (n, line))
            continue
        seen.setdefault(m.group(1), []).append(m.group(2).strip())
    for c in seen.get("Contact", []) or [None]:
        if c is None or not re.match(r"^(mailto:[^@\s]+@[^@\s]+|https://\S+)$", c):
            errors.append("security.txt: a Contact must be mailto: or https:// (got %r)" % c)
    exp = seen.get("Expires", [])
    if len(exp) != 1:
        errors.append("security.txt: Expires must appear exactly once")
    else:
        try:
            when = datetime.datetime.strptime(exp[0].replace(".000Z", "Z"), "%Y-%m-%dT%H:%M:%SZ").replace(
                tzinfo=datetime.timezone.utc)
        except ValueError:
            errors.append("security.txt: Expires is not YYYY-MM-DDTHH:MM:SS[.000]Z: %r" % exp[0])
        else:
            if when <= now:
                errors.append("security.txt: Expires %s has passed: renew it (a year out)" % exp[0])
            elif when > now + datetime.timedelta(days=366):
                errors.append("security.txt: Expires %s is more than a year out" % exp[0])
    if seen.get("Canonical") != [SITE_URL + "/.well-known/security.txt"]:
        errors.append("security.txt: Canonical must be %s/.well-known/security.txt" % SITE_URL)
    for p in seen.get("Policy", []):
        if not p.startswith("https://"):
            errors.append("security.txt: Policy must be an https:// URL: %r" % p)
    for rx, what in LEAKS:
        for m in rx.finditer(text):
            errors.append("security.txt: %s: %r" % (what, m.group(0)))
    return errors


def robots_checks(text):
    """robots.txt names every agent and never shuts the whole site."""
    errors = []
    if not re.search(r"^User-agent: \*\s*$", text, re.M):
        errors.append("robots.txt: no 'User-agent: *' group")
    if re.search(r"^Disallow:\s*/\s*$", text, re.M):
        errors.append("robots.txt: 'Disallow: /' shuts the whole site")
    if not re.search(r"^Sitemap: %s/%s\s*$" % (re.escape(SITE_URL), SITEMAP), text, re.M):
        errors.append("robots.txt: no 'Sitemap: %s/%s' line" % (SITE_URL, SITEMAP))
    return errors


def sources_rows():
    """{repo path: sha256} of the table in site/SOURCES.md."""
    return {m.group(1): m.group(2) for m in re.finditer(r"^\| `([^`]+)` \|.*\| `([0-9a-f]{64})` \|$",
                                                         SOURCES.read_text(encoding="utf-8"), re.M)}


def installer_checks(files):
    """The installer: its script and the vendored bundle on /install/ only; versions.json valid, in step with
    boards.json, every same-origin url built; the bundle's bytes as site/SOURCES.md pins them."""
    errors = []
    inst = [k for k in files if re.match(r"assets/install-[0-9a-f]{8}\.js$", k)]
    if len(inst) != 1:
        return ["installer: expected one assets/install-<hash>.js, found %d" % len(inst)]
    for p, d in site_pages(files).items():
        has = ("/" + inst[0]) in d or "esptool-bundle" in d
        if p == "install/index.html" and ('<script src="/%s" defer></script>' % inst[0]) not in d:
            errors.append("install/index.html: does not load /%s" % inst[0])
        elif p != "install/index.html" and has:
            errors.append("%s: loads the installer's script or bundle (only /install/ may)" % p)
    if ("/" + BUNDLE).encode() not in files[inst[0]]:
        errors.append("installer: install.js does not import /%s" % BUNDLE)
    if BUNDLE not in files:
        errors.append("installer: /%s is not built" % BUNDLE)
    rows = sources_rows()
    if "site/" + BUNDLE not in rows:
        errors.append("site/SOURCES.md: no row for site/%s" % BUNDLE)
    for rel, sha in rows.items():
        p = ROOT / rel
        if not p.is_file() or hashlib.sha256(p.read_bytes()).hexdigest() != sha:
            errors.append("site/SOURCES.md: %s does not match its SHA-256 (re-vendor it, or update the table)" % rel)
    try:
        doc = json.loads(files["firmware/versions.json"])
        boards = json.loads(BOARDS.read_text(encoding="utf-8"))["boards"]
    except (KeyError, ValueError) as e:
        return errors + ["installer: versions.json or boards.json does not read: %s" % e]
    vb = doc.get("boards") or {}
    if set(vb) != set(boards):
        errors.append("versions.json boards %s differ from boards.json %s" % (sorted(vb), sorted(boards)))
    for bid, b in vb.items():
        ref = boards.get(bid, {})
        for k in ("label", "chip", "flash_size", "photo", "photo_size"):
            if b.get(k) != ref.get(k):
                errors.append("versions.json %s: %s differs from boards.json" % (bid, k))
        photo = files.get(str(b.get("photo", "")).lstrip("/"))
        if photo is None:
            errors.append("versions.json %s: photo %s is not built" % (bid, b.get("photo")))
        else:
            try:
                size = list(jpeg_size(photo))
            except SystemExit:  # jpeg_size refuses a file that is not a JPEG
                size = None
            if size != list(b.get("photo_size") or ()):
                errors.append("versions.json %s: photo_size %s is not the photo's %s" % (bid, b.get("photo_size"), size))
        vers = b.get("versions")
        if not isinstance(vers, list) or not vers:
            errors.append("versions.json %s: no versions" % bid)
            continue
        for v in vers:
            if not isinstance(v.get("version"), str) or not isinstance(v.get("prerelease"), bool):
                errors.append("versions.json %s: a version without its name or pre-release flag" % bid)
            url, sha = v.get("url"), v.get("sha256")
            if url is None:
                if sha is not None or v.get("size") is not None:
                    errors.append("versions.json %s %s: a SHA-256 without a url" % (bid, v.get("version")))
                continue
            if not isinstance(sha, str) or not re.match(r"^[0-9a-f]{64}$", sha):
                errors.append("versions.json %s %s: no SHA-256" % (bid, v.get("version")))
            if not isinstance(v.get("size"), int) or v["size"] <= 0:
                errors.append("versions.json %s %s: no size" % (bid, v.get("version")))
            if url.startswith("/"):
                data = files.get(url.lstrip("/"))
                if data is None:
                    errors.append("versions.json %s %s: %s is not built" % (bid, v.get("version"), url))
                elif len(data) != v.get("size"):
                    errors.append("versions.json %s %s: %s is %d B, the list says %s" % (bid, v.get("version"), url, len(data), v.get("size")))
                elif hashlib.sha256(data).hexdigest() != sha:
                    errors.append("versions.json %s %s: %s does not match its SHA-256" % (bid, v.get("version"), url))
            elif not url.startswith("https://"):
                errors.append("versions.json %s %s: url %s is neither same-origin nor https" % (bid, v.get("version"), url))
    return errors


def page_images(files, page):
    """The worst-case image bytes one page loads: per <img>/<picture>, its largest candidate."""
    total = 0
    for block in re.findall(r"<picture>.*?</picture>|<img [^>]*>", page, re.S):
        cands = [u for u in urls(block) if u.startswith("/assets/")]
        total += max((len(files.get(u.lstrip("/"), b"")) for u in cands), default=0)
    return total


def budgets(files):
    """{name: (bytes, cap)} of the landing budget and each page's images."""
    pages = site_pages(files)
    land = files["index.html"]
    css = next(v for k, v in files.items() if re.match(r"assets/site-[0-9a-f]{8}\.css$", k))
    js = next(v for k, v in files.items() if re.match(r"assets/site-[0-9a-f]{8}\.js$", k))
    out = {"landing html+css+js gz": (len(g.gz(land)) + len(g.gz(css)) + len(g.gz(js)), LANDING_GZ_MAX)}
    inst = next((v for k, v in files.items() if re.match(r"assets/install-[0-9a-f]{8}\.js$", k)), b"")
    out["install html+css+js gz"] = (len(g.gz(files["install/index.html"])) + len(g.gz(css)) + len(g.gz(js)) + len(g.gz(inst)),
                                     INSTALL_GZ_MAX)
    for p, d in sorted(pages.items()):
        n = page_images(files, d)
        if n:
            out["images " + p] = (n, PAGE_IMAGES_MAX)
    return out


def served_diff(files, served):
    """The differences between a checked build and the folder that will be served (the site workflow's site/_build
    after the release zips joined it): every file the same bytes, none missing, none extra."""
    have = {f.relative_to(served).as_posix(): f for f in served.rglob("*") if f.is_file() and f.name != MARKER}
    errors = ["served: %s is missing" % p for p in sorted(set(files) - set(have))]
    errors += ["served: %s is not in the checked build" % p for p in sorted(set(have) - set(files))]
    errors += ["served: %s differs from the checked build" % p for p in sorted(set(files) & set(have))
               if have[p].read_bytes() != files[p]]
    return errors


def check(out, local=None, extra=None, served=None):
    """Every difference between a build in `out` and the firmware's pages, and every site finding; [] when clean.
    `served`: a built folder that must equal this checked build byte for byte (the deploy's own folder)."""
    files, table = emit(out, local, extra)
    errors = served_diff(files, served) if served is not None else []
    h_assets, h_shells = header()
    built = {path: gzb for _, gzb, path, _ in table}
    if set(built) != set(h_assets):
        errors.append("asset paths differ from web_assets.h (stale header? run gen_web_assets.py): %s" %
                      sorted(set(built) ^ set(h_assets)))
    emitted_assets = {"/" + p for p in files if p.startswith("a/")}
    if emitted_assets != set(h_assets):
        errors.append("emitted /a/ files differ from the header's asset list: %s" % sorted(emitted_assets ^ set(h_assets)))
    for path, gzb in h_assets.items():
        rel = path.lstrip("/")
        if hashlib.sha1(gzb).hexdigest()[:8] != path.split("/")[2]:
            errors.append("%s: the header's bytes do not hash to their path" % path)
        if rel in files and files[rel] != gzip.decompress(gzb):
            errors.append("%s: emitted bytes differ from the header's" % path)
    for page, dest in SHELLS.items():
        shell = files[dest]
        if shell.count(DEMO_TAG) != 1:
            errors.append("%s: the demo tag must appear exactly once" % dest)
        if shell.count(TRY_OG[dest]) != 1 or not shell.endswith(TRY_OG[dest] + shell[shell.find(b"</head>"):]):
            errors.append("%s: the share-card block must appear exactly once, right before </head>" % dest)
        b_ = bare(shell, dest)
        if b_ != g.page(g.WEB / page, table):
            errors.append("%s: differs from page() of web/%s beyond the two added blocks" % (dest, page))
        if b_ != gzip.decompress(h_shells[page]):
            errors.append("%s: differs from the header's %s beyond the two added blocks" % (dest, page))
        first = FIRST_BUNDLE.search(shell)
        if not first or shell.find(DEMO_TAG) + len(DEMO_TAG) != first.start():
            errors.append("%s: the demo tag is not right before the first bundle script" % dest)
    for dest, src in EXTRAS.items():
        if (out / dest).read_bytes() != src.read_bytes():
            errors.append("%s: differs from %s" % (dest, src.relative_to(ROOT)))
    for rel, src in fixture_files():
        if (out / "try" / "data" / rel).read_bytes() != src.read_bytes():
            errors.append("try/data/%s: differs from the fixture" % rel)
    # the site
    if not TOKENS.is_file() or TOKENS.read_text(encoding="utf-8").replace("\r\n", "\n") != tokens():
        errors.append("site/tokens.css differs from app.css's tokens (run python -I tools/build_site.py and commit it)")
    errors += link_check(files)[0]
    errors += page_checks(files)
    errors += seo_checks(files)
    errors += installer_checks(files)
    errors += security_txt_checks(files[".well-known/security.txt"].decode("utf-8"),
                                  datetime.datetime.now(datetime.timezone.utc))
    errors += robots_checks(files["robots.txt"].decode("utf-8"))
    for name, (n, cap) in budgets(files).items():
        if n > cap:
            errors.append("budget: %s is %d B, over its %d B cap (site/README.md)" % (name, n, cap))
    return errors


def sizes(out):
    """Byte totals of the build: /a/, /try/ (extras + shells), /try/data/ and the site's own files."""
    def total(p):
        return sum(f.stat().st_size for f in p.rglob("*") if f.is_file()) if p.exists() else 0
    data = total(out / "try" / "data")
    a, t = total(out / "a"), total(out / "try") - data
    return {"a": a, "try": t, "data": data, "site": total(out) - a - t - data}


def main(argv):
    ap = argparse.ArgumentParser(prog="build_site.py", description="Build the static site into site/_build/.")
    ap.add_argument("--check", action="store_true", help="build into a temporary folder, compare with the shipped pages, run the site checks")
    ap.add_argument("--out", metavar="DIR", help="build here: an absent or empty folder, or an earlier build")
    ap.add_argument("--links", action="store_true", help="print the external links of the build")
    ap.add_argument("--local-firmware", nargs="?", const=str(LOCAL_FW), metavar="DIR",
                    help="offer this computer's firmware build (default firmware/fauxcloud/build/default/) as "
                         "/firmware/local/ in the build: a preview for a rehearsal on a spare board, never deployed")
    ap.add_argument("--firmware-extra", metavar="DIR", help="add DIR's files to /firmware/ (the release zips of "
                    "tools/site_versions.py --mirror); with --check they are checked against versions.json")
    ap.add_argument("--served", metavar="DIR", help="with --check: DIR (the folder the deploy uploads) must equal the "
                    "checked build byte for byte")
    args = ap.parse_args(argv)
    out = pathlib.Path(args.out).resolve() if args.out else OUT
    local = pathlib.Path(args.local_firmware).resolve() if args.local_firmware else None
    extra = pathlib.Path(args.firmware_extra).resolve() if args.firmware_extra else None
    served = pathlib.Path(args.served).resolve() if args.served else None
    if served is not None and not args.check:
        raise SystemExit("build_site: --served goes with --check")
    if local is not None and not args.check and out.is_relative_to(ROOT.resolve()) and out != OUT.resolve():
        raise SystemExit("build_site: --local-firmware builds into site/_build/ or a folder outside the repository only")
    if args.check:
        with tempfile.TemporaryDirectory() as tmp:
            errors = check(pathlib.Path(tmp) / "site", local, extra, served)
        if errors:
            print("build_site: %d finding(s):\n  %s" % (len(errors), "\n  ".join(errors)))
            return 1
        print("build_site: /try/ shells and assets are the shipped bytes (two added blocks per shell); tokens in step "
              "with app.css; links, share cards, search tags, sitemap, words and budgets clean")
        return 0
    t = tokens()
    if not TOKENS.is_file() or TOKENS.read_text(encoding="utf-8").replace("\r\n", "\n") != t:
        TOKENS.write_bytes(t.encode("utf-8"))
        print("build_site: wrote site/tokens.css (commit it)")
    files, _ = emit(out, local, extra)
    s = sizes(out)
    print("build_site: wrote %d files to %s (/a/ %d B, /try/ %d B, /try/data/ %d B, site %d B)" % (
        len(files), out, s["a"], s["try"], s["data"], s["site"]))
    for name, (n, cap) in budgets(files).items():
        print("  %-34s %7d B of %d" % (name, n, cap))
    errs, external = link_check(files)
    found = errs + page_checks(files) + seo_checks(files) + installer_checks(files)
    # the site workflow builds before the release zips join (they pass the leak gate unpacked, in the release): one
    # note for them here; --check with --firmware-extra and --served checks them in full
    aside = [e for e in found if re.search(r"^versions\.json .*: /firmware/[^ ]+ is not built$", e)]
    for e in found:
        if e not in aside:
            print("  finding: " + e)
    if aside:
        print("  note: %d listed release zip(s) not in this build (they join it later: --firmware-extra)" % len(aside))
    if args.links:
        print("external links:\n  " + "\n  ".join(external))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
