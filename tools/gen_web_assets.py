"""gen_web_assets.py -- bake the pages and assets into firmware/fauxcloud/src/web_assets.h (gzipped shells + assets).

Assets, served at /a/<hash8>/<name> with an immutable year-long cache (hash8 = short SHA-1 of the gzip bytes) and
listed in web::ASSETS:
  - every firmware/fauxcloud/web/data/*.json (the data packs from tools/gen_tz_flags.py);
  - the bundles in BUNDLES: the stylesheets (see below) and the page scripts, each a concatenation of source files
    from web/. A bundle may name an earlier one (`/*@asset admin-station.js*/`): that is how a page script loads a
    lazy asset on first use (FC.lazy in core.js).

The stylesheet web/app.css is one file in sections, `/*@face all*/`, `/*@face dash*/`, `/*@face setup*/`,
`/*@face admin*/`. Assets: one app-<face>.css per served face = the `all` section + that face's section; no other
face's rules travel, so the sections need no scoping.

Pages: every web/*.html. `/*@include NAME*/` inlines a web/ file, `/*@css FACE*/` inlines the `all` + FACE sections
of the stylesheet (setup: one request in a captive mini-browser), `/*@asset NAME*/` becomes the asset's hashed
path. Every JavaScript bundle and the setup page's inline script go through terser 5.36.0, mangle only
(tools/package.json + lockfile, `npm ci --ignore-scripts` in tools/; node from FAUXCLOUD_NODE or PATH; no node =
a loud failure, never a silent fallback that would change every hash). HTML shells and CSS get the light minify
(indentation, blank lines, whole-line // and <!-- --> comments). zopfli (gzip format, mtime 0, byte-identical on every
machine), emitted as a C++ array, its length and the page version (the shell ETag): a short SHA-1 of the shell text with the
PAGE_MARK placeholder in it, written in the placeholder's place on the polled shells' <html data-page>.
THIRD_PARTY_LICENSES.md is served as the lazy asset licenses.txt (text/plain), linked once per face.

Usage (stdlib only, run isolated):
    python -I tools/gen_web_assets.py           # regenerate the header
    python -I tools/gen_web_assets.py --check   # CI: exit 1 if the committed header is stale or a page is over budget
    python -I tools/gen_web_assets.py --sizes   # page sizes against the budgets, every lazy file, the flash total
"""
import gzip
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB = ROOT / "firmware" / "fauxcloud" / "web"
OUT = ROOT / "firmware" / "fauxcloud" / "src" / "web_assets.h"
DATA = WEB / "data"
# the language packs' sources: web/lang/<lang>.json, {"_meta": {"name": …}, "<file>.<key>": {"en": …, "<lang>": …}}; format 2
# (tools/i18n_pack.py) adds the common table cmn.<id> and references to it, resolved here into each pack
LANG = WEB / "lang"
LANGS = re.compile(r"/\*@langs\*/\{\}")  # the marker and the empty object the source keeps
INCLUDE = re.compile(r"/\*@include ([A-Za-z0-9_.-]+)\*/")
ASSET = re.compile(r"/\*@asset ([A-Za-z0-9_.-]+)\*/")
CSS_INLINE = re.compile(r"/\*@css ([a-z]+)\*/")
TYPES = {".json": "application/json", ".js": "text/javascript", ".css": "text/css", ".svg": "image/svg+xml",
         ".txt": "text/plain; charset=utf-8", ".png": "image/png"}
LICENSES = ROOT / "THIRD_PARTY_LICENSES.md"  # served as licenses.txt: one source, static
FACES = ["dash", "setup", "admin"]
# asset name -> source files concatenated in order, or ("css", section) for a stylesheet. Lazy assets come before
# the bundles that name them (a bundle links only what is built already).
BUNDLES = {
    "favicon.svg": ["favicon.svg"],  # every page's <link rel="icon"> (not part of the page budgets: the tab loads it)
    # the home-screen icon (<link rel="apple-touch-icon"> on every face; loaded by "Add to Home Screen", not by the page),
    # a committed binary rendered by tools/gen_touch_icon.py; copied as it is (a PNG does not gzip further)
    "touch-icon.png": ("bin", WEB / "touch-icon.png"),
    # sky effects: the night's moon, the star tile (stars at night, flakes in snow) and the rain tile, named from sky.css
    # (before the stylesheets that link them); fetched when the sky shows them, cached a year
    "moon.svg": ["moon.svg"], "stars.svg": ["stars.svg"], "rain.svg": ["rain.svg"],
    "sky.css": ["sky.css"],  # their rules, linked by core.js setSky() the first time the sky is night or rain
    "sky.js": ["sky.js"],  # the tile cut-outs (no effect under a glass tile), loaded by setSky() after sky.css
    "licenses.txt": ("copy", LICENSES),  # the "Open source · Licenses" link of every face (lazy: on click)
    "app-dash.css": ("css", "dash"),
    "app-admin.css": ("css", "admin"),
    "admin-station.js": ["admin-station.js"],
    "dash-lg.js": ["dash-lg.js"],
    "dash-sheet.js": ["dash-sheet.js"],
    "dash-airport.js": ["dash-airport.js"],  # the dashboard's airport view (lazy, on the plane tab)
    "dash-wufc.js": ["dash-wufc.js"],  # page 2 "Next" of the outlook card (lazy: the WU line or the airport module)
    "outlook-why.js": ["outlook-why.js"],  # the strip's ⓘ + the admin "Weather description" sheet (lazy, both faces)
    "admin-learn.js": ["admin-learn.js"],  # the admin "Learning" sheet (lazy)
    "admin-fwd.js": ["admin-fwd.js"],  # the admin "Online services" sheet (lazy)
    "admin-metar.js": ["admin-metar.js"],  # the admin "Airport weather" sheet (lazy; FWD_TLS builds use it)
    "admin-sheets.js": ["admin-sheets.js"],  # MQTT, Name, Password and devices, Maintenance (lazy)
    "admin-capture.js": ["admin-capture.js"],  # the admin "Listen to a new console" sheet (lazy, from Publishing)
    # the strip's words and glyphs as their own asset, so the admin's Weather description draws the same blocks
    # (loaded before outlook-why.js there); the dashboard loads it at once, before status.js (defer keeps the order)
    "outlook.js": ["outlook.js"],
    "status.js": ["core.js", "lang.js", "health.js", "tendency.js", "dash.js"],
    "admin.js": ["core.js", "lang.js", "health.js", "crypto.js", "picker.js", "adminface.js"],
}
# page -> the assets it loads at once, and the ones it loads on first use
PAGES = {
    "index.html": {"initial": ["app-dash.css", "outlook.js", "status.js"], "lazy": ["dash-lg.js", "dash-sheet.js", "outlook-why.js", "dash-airport.js", "dash-wufc.js", "sky.css", "sky.js"]},
    "admin.html": {"initial": ["app-admin.css", "admin.js"], "lazy": ["admin-station.js", "outlook.js", "outlook-why.js", "admin-learn.js", "admin-fwd.js", "admin-metar.js", "admin-sheets.js", "admin-capture.js", "sky.css", "sky.js"]},
    "setup.html": {"initial": [], "lazy": []},  # its inline script names sky.css (counted in the flash total, not here)
}
# gzip bytes. Initial = what a page needs before it works (shell + initial assets): the measured 30 KB envelope that
# keeps the console's upload under 200 ms during a two-device cold load (tools/coldload.py). Every other asset is
# fetched on its own, on first use: each one holds the single httpd worker about 4.5 ms/KB, so each is capped, and all
# of it together is capped by the flash it takes in the image.
# The dashboard's initial is above 30 KB by its measured cost: the reload after an OTA (core.js, the shell's version on
# <html>, 31 B) and its own words with lang.js (the W tables in status.js, +3 108 B: 33 850 measured), plus the eleven
# lower-case measure names of the tiles' "Open {m} history" (a W text is never lower-cased in code: +82 B, 33 941
# measured). The admin stays inside its 30 KB (30 060 measured).
# Hash noise: a language pack edit changes the pack's hash in its face's manifest, and that moves the main bundle's gzip by
# up to about 41 B measured. So the budgets a pack edit can move carry 64 B of noise margin over their measured figure
# (noise margin, not budget): the dashboard's initial (its packs' hashes live in status.js), each first visit in a
# language, the code cap and WEB_LANG_MAX. The admin's initial keeps its 30 KB, far above the noise.
HASH_NOISE = 64
BUDGET_INITIAL = {"index.html": 33941 + HASH_NOISE, "admin.html": 30720, "setup.html": 20480}
# A first visit in a language: the initial page plus the packs of its initial scripts, fetched before the first poll (the
# same cold-load rule; tools/coldload.py --lang measures it). Each language names its own figure, at the measured value;
# a language without one fails the budget check.
# the admin's: + "Listen to a new console"'s home line, its button on the fauxcloud Wi-Fi sheet and their admin.pl.json keys,
# +437 B (34 026 -> 34 463 measured on 1.1.0-dev.2)
BUDGET_FIRST = {("index.html", "pl"): 38492 + HASH_NOISE, ("admin.html", "pl"): 34187 + 437 + HASH_NOISE}
# 13 KB: about 4.5 ms of worker time per KB -> ~60 ms per file, far under the 200 ms upload rule; sized by tz.json
# (12 968 B, the zone picker's pack), the largest lazy file
LAZY_FILE_MAX = 13312
# The flash the web files take in the image (web_assets.h), derived from the image headroom, not from a page rule, and
# split in two so a raise always names which it is: code = the scripts, the stylesheets and the page shells; assets =
# data packs, images and texts (WEB_ASSET_EXT: the flag packs, tz.json, the icons, licenses.txt). The guarded budgets
# are the initial pages and the lazy file sizes; the image gate (tools/build.ps1 + CI: fail above the app0 slot,
# warning at 95 %) stays the hard limit (the app image is at about 90 %).
# A feature raises the code cap by its measured cost (the commit that raises it names the measurement). The two faces'
# words in their W tables, lang.js on both and the packs' manifests: +10 354 B (148 594 measured); the tiles' lower-case
# measure names and the forecast card's day words (no W text lower-cased in code): +72 B (148 666 measured); the history
# chart's lowest / highest labels, the sky mask drawn on the page (sky.js / sky.css) and the record CSV's per-kind columns with
# the compass: +584 B over 148 659 (149 243 measured); "Listen to a new console" (admin-capture.js, its button on the
# fauxcloud Wi-Fi sheet, the home line, the update sheet's refusal): +4 921 B (149 091 -> 154 012 measured on 1.1.0-dev.2)
WEB_CODE_MAX = 149243 + 4921 + HASH_NOISE
WEB_ASSET_MAX = 52000
WEB_ASSET_EXT = (".json", ".png", ".svg", ".txt")
# the language packs (web/lang -> <asset>.<lang>.json), a line of their own so code and assets stay what they were
# measured (pl): the dashboard's six packs and the admin's six (29 365); + admin-capture's pack and the four keys of
# "Listen to a new console" elsewhere: 30 568 measured on 1.1.0-dev.2 (+1 196 over 29 372); the cap is the measurement + the
# 64-B hash noise (the old base sat 7 B under its measurement, which left 52 B, under the noise margin)
WEB_LANG_MAX = 30568 + HASH_NOISE
WEB_FLASH_MAX = WEB_CODE_MAX + WEB_ASSET_MAX + WEB_LANG_MAX
# The faces with packs: the dashboard and the admin; one pack per script asset, its namespaces = the asset's source files'
# stems, a page shell's words (data-t) travel with its main script. The setup face stays English.
LANG_PAGES = ["index.html", "admin.html"]
LANG_SHELL = {"status.js": "index", "admin.js": "admin"}
# namespaces with no English in the code, carried by a script's pack: the firmware's notices (dash.js registers an empty
# table; English shows the firmware's own words, so the English bundle carries none of them)
LANG_EXTRA = {"status.js": ["notice"]}
# the setup page carries its script inline (one request in a captive mini-browser): terser runs on it too
INLINE_SCRIPT_PAGES = {"setup.html"}
# The polled shells carry their own version on <html> (data-page) and /api/live repeats it, so a page an OTA left
# behind (a home-screen app resumes it without asking the server) sees the difference and reloads once (core.js).
# The source holds this placeholder; the version is hashed over the shell with it, then written in its place.
PAGE_MARK = 'data-page="00000000"'
POLLED_PAGES = {"index.html", "admin.html"}


def read(path):
    # read_text: a CRLF checkout (core.autocrlf) must give the same bytes, hash and path as CI
    return path.read_text(encoding="utf-8")


def inline(text):
    def repl(m):
        path = WEB / m.group(1)
        if not path.is_file():
            raise SystemExit("include not found: %s" % path)
        return read(path)
    return INCLUDE.sub(repl, text)


# ---------------------------------------------------------------- stylesheet
def css_sections():
    text = read(WEB / "app.css")
    parts = re.split(r"/\*@face ([a-z]+)\*/", text)
    sections = {}
    for i in range(1, len(parts), 2):
        sections[parts[i]] = sections.get(parts[i], "") + parts[i + 1]
    for face in sections:
        if face != "all" and face not in FACES:
            raise SystemExit("unknown face section in app.css: %s" % face)
    return sections


def text_no_comments(t):
    return re.sub(r"/\*.*?\*/", "", t, flags=re.S)


STRING = re.compile(r"""("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')""")


def minify_css(text):
    # whitespace is squeezed outside quoted strings only (content: "a, b" must stay as written)
    parts = STRING.split(text_no_comments(text))
    for i in range(0, len(parts), 2):
        t = re.sub(r"\s+", " ", parts[i])
        parts[i] = re.sub(r"\s*([{};,>])\s*", r"\1", t).replace(";}", "}")
    return "".join(parts).strip() + "\n"


def stylesheet(faces):
    s = css_sections()
    parts = [text_no_comments(s.get("all", ""))]
    parts += [text_no_comments(s.get(f, "")) for f in faces]
    return minify_css("".join(parts))


# ---------------------------------------------------------------- assets
def minify(text):
    out = []
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("//") or (s.startswith("<!--") and s.endswith("-->")):
            continue
        out.append(s)
    return "\n".join(out) + "\n"


_TERSER_CACHE = {}


def node():
    """The node binary: FAUXCLOUD_NODE, else node on PATH. None found: a loud failure (a fallback would change hashes)."""
    exe = os.environ.get("FAUXCLOUD_NODE") or shutil.which("node")
    if not exe or (not pathlib.Path(exe).is_file() and not shutil.which(exe)):
        raise SystemExit("gen_web_assets: node not found. Install node 20+ (or set FAUXCLOUD_NODE) and run "
                         "`npm ci --ignore-scripts` in tools/ (see firmware/README.md)")
    if not (ROOT / "tools" / "node_modules" / "terser").is_dir():
        raise SystemExit("gen_web_assets: terser not installed: run `npm ci --ignore-scripts` in tools/")
    return exe


def terser(js):
    """JavaScript -> terser 5.36.0, mangle only (tools/terser_run.js). Cached per text within one run."""
    if js in _TERSER_CACHE:
        return _TERSER_CACHE[js]
    r = subprocess.run([node(), str(ROOT / "tools" / "terser_run.js")], input=js.encode("utf-8"),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=str(ROOT / "tools"))
    if r.returncode:
        raise SystemExit("terser failed: " + r.stderr.decode("utf-8", "replace").strip())
    out = r.stdout.decode("utf-8") + "\n"
    _TERSER_CACHE[js] = out
    return out


# zopfli -- gzip-compatible output, ~5 % smaller, deterministic (no mtime). The
# module is pinned with hashes in tools/requirements.txt and installed beside this script into tools/.pylib (gitignored;
# `python -I` sees no user site-packages): python -m pip install --require-hashes --target tools/.pylib -r
# tools/requirements.txt (CI may install it into the interpreter). A missing module is a hard error, never a silent gzip
# fallback: the bytes (and the hashed asset paths) must be the same on every machine.
ZOPFLI_ITERATIONS = 15
_pylib = ROOT / "tools" / ".pylib"
if _pylib.is_dir() and str(_pylib) not in sys.path:
    sys.path.append(str(_pylib))  # after the stdlib
try:
    import zopfli.gzip as _zopfli_gzip
except ImportError:
    _zopfli_gzip = None


def gz(data):
    if _zopfli_gzip is None:
        sys.exit("gen_web_assets: zopfli not installed: run `python -m pip install --require-hashes --only-binary :all: "
                 "--target tools/.pylib -r tools/requirements.txt` (see firmware/README.md)")
    return _zopfli_gzip.compress(data, numiterations=ZOPFLI_ITERATIONS)


def i18n_pack():
    import importlib.util
    spec = importlib.util.spec_from_file_location("i18n_pack", ROOT / "tools" / "i18n_pack.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def lang_sources():
    """lang -> (its own name, {"<file>.<key>": translated text or count entry}) from web/lang/*.json, references to the
    common table resolved."""
    out, pk = {}, i18n_pack()
    for path in sorted(LANG.glob("*.json")):
        d = json.loads(read(path))
        tr, bad = pk.resolve(d, path.stem)
        if bad:
            raise SystemExit("%s: %s: %s" % (path.name, bad[0][0], bad[0][1]))
        out[path.stem] = (d.get("_meta", {}).get("name", path.stem), tr)
    return out


def lang_assets():
    """script asset -> its pack namespaces (the dashboard's scripts, in the page's order)."""
    out = {}
    for page in LANG_PAGES:
        for a in PAGES[page]["initial"] + PAGES[page]["lazy"]:
            src = BUNDLES.get(a)
            if a.endswith(".js") and isinstance(src, list) and a not in out:
                out[a] = [x.rsplit(".", 1)[0] for x in src] + ([LANG_SHELL[a]] if a in LANG_SHELL else []) + LANG_EXTRA.get(a, [])
    return out


def lang_packs():
    """[(pack name, raw bytes, lang, asset)]: one compact JSON {namespace: {key: text}} per script asset and language."""
    out = []
    for lang, (_, tr) in lang_sources().items():
        for a, spaces in lang_assets().items():
            d = {}
            for k, v in tr.items():
                ns, key = k.split(".", 1)
                if ns in spaces:
                    d.setdefault(ns, {})[key] = v
            if d:
                raw = json.dumps(d, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
                out.append(("%s.%s.json" % (a.rsplit(".", 1)[0], lang), raw, lang, a))
    return out


def assets():
    """[(name, gzip bytes, hashed path, content type)]: data packs, language packs, then bundles."""
    items, packs = [], []
    for path in sorted(DATA.glob("*.json")):
        raw = read(path).encode("utf-8")
        items.append((path.name, raw))
        g = gz(raw)
        packs.append((path.name, g, "/a/%s/%s" % (hashlib.sha1(g).hexdigest()[:8], path.name), "application/json"))
    langs, names = {}, {k: v[0] for k, v in lang_sources().items()}
    for name, raw, lang, a in lang_packs():
        g = gz(raw)
        path = "/a/%s/%s" % (hashlib.sha1(g).hexdigest()[:8], name)
        packs.append((name, g, path, "application/json"))
        langs.setdefault(lang, {"n": names[lang]})[a] = path
    # the manifest lang.js reads: language -> its own name and the pack of each script asset this page loads (its own
    # page's only: the other face's pack URLs would be dead bytes on every first visit); _i = the page's initial scripts
    # (their packs go first, the last one is the page's main)
    def manifest(bundle):
        pages = [p for p in LANG_PAGES if bundle in PAGES[p]["initial"]]
        init = [a for p in pages for a in PAGES[p]["initial"] if a.endswith(".js")]
        if not init:
            raise SystemExit("bundle %s carries lang.js but no translated page loads it at once: its packs would never load" % bundle)
        mine = {a for p in pages for a in PAGES[p]["initial"] + PAGES[p]["lazy"]}
        own = {lang: {k: v for k, v in d.items() if k == "n" or k in mine} for lang, d in langs.items()}
        return json.dumps(dict(own, _i=init), ensure_ascii=False, separators=(",", ":"))
    out = list(packs)
    for name, sources in BUNDLES.items():
        if isinstance(sources, tuple) and sources[0] == "bin":
            data = sources[1].read_bytes()
            g = gz(data)
            out.append((name, g, "/a/%s/%s" % (hashlib.sha1(g).hexdigest()[:8], name), TYPES[pathlib.Path(name).suffix]))
            continue
        if isinstance(sources, tuple) and sources[0] == "copy":
            raw = read(sources[1])
        elif isinstance(sources, tuple):
            raw = stylesheet([sources[1]])
        else:
            missing = [s for s in sources if not (WEB / s).is_file()]
            if missing:
                raise SystemExit("bundle %s: missing %s" % (name, ", ".join(missing)))
            # a bundle may name data packs (/*@asset tz.json*/) and earlier bundles: those are hashed already
            text = LANGS.sub(lambda m: manifest(name), link("\n".join(read(WEB / s) for s in sources), out))
            # a stylesheet of its own (sky.css) gets the stylesheet's minify (its /* */ comments never ship)
            raw = terser(text) if name.endswith(".js") else minify_css(text) if name.endswith(".css") else minify(text)
        g = gz(raw.encode("utf-8"))
        out.append((name, g, "/a/%s/%s" % (hashlib.sha1(g).hexdigest()[:8], name), TYPES[pathlib.Path(name).suffix]))
    return out


def link(text, table):
    paths = {name: path for name, _, path, _ in table}

    def repl(m):
        if m.group(1) not in paths:
            raise SystemExit("asset not found: %s" % m.group(1))
        return paths[m.group(1)]
    return ASSET.sub(repl, text)


SCRIPT = re.compile(r"(<script>)(.*?)(</script>)", re.S)


def page(path, table):
    return page_version(path, table)[0]


def page_version(path, table):
    """(shell bytes, version): the version is a short SHA-1 of the built shell with PAGE_MARK still in it."""
    text = inline(read(path))
    text = CSS_INLINE.sub(lambda m: stylesheet([m.group(1)]), text)
    text = link(text, table)
    if path.name in INLINE_SCRIPT_PAGES:
        text = SCRIPT.sub(lambda m: m.group(1) + terser(m.group(2)).rstrip("\n") + m.group(3), text)
    raw = minify(text).encode("utf-8")
    if raw.count(PAGE_MARK.encode()) != (path.name in POLLED_PAGES):
        raise SystemExit("%s: %s must appear %s" % (path.name, PAGE_MARK,
                                                    "once, on <html>" if path.name in POLLED_PAGES else "nowhere"))
    v = hashlib.sha1(raw).hexdigest()[:8]
    return raw.replace(PAGE_MARK.encode(), b'data-page="%s"' % v.encode()), v


def c_name(path):
    return re.sub(r"[^A-Za-z0-9]", "_", path.name).upper()


def array(lines, name, data):
    lines.append("const uint8_t %s[] = {" % name)
    for i in range(0, len(data), 20):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 20]) + ",")
    lines.append("};")


def render():
    pages = sorted(WEB.glob("*.html"))
    if not pages:
        raise SystemExit("no pages in %s" % WEB)
    lines = [
        "// web_assets.h -- GENERATED by tools/gen_web_assets.py from firmware/fauxcloud/web/. Do not edit.",
        "// Regenerate: python -I tools/gen_web_assets.py   (CI runs it with --check)",
        "// On the ESP32 const data stays in flash (PROGMEM is a no-op there), served with Content-Encoding: gzip.",
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "namespace fc {",
        "namespace web {",
        "",
    ]
    table = assets()
    for name, g, path, ctype in table:
        lines.append("// %s -> %s: %d B gzip" % (name, path, len(g)))
        array(lines, c_name(pathlib.Path(name)) + "_GZ", g)
        lines.append("")
    lines += ["struct Asset {", "  const char* path;  // /a/<hash8>/<name>", "  const char* type;",
              "  const uint8_t* gz;", "  size_t len;", "};", "const Asset ASSETS[] = {"]
    for name, g, path, ctype in table:
        lines.append('    {"%s", "%s", %s_GZ, %d},' % (path, ctype, c_name(pathlib.Path(name)), len(g)))
    lines += ["};", "const size_t ASSET_COUNT = %d;" % len(table), ""]
    for p in pages:
        raw, version = page_version(p, table)
        g = gz(raw)
        name = c_name(p)
        lines.append("// %s: %d B source -> %d B gzip" % (p.name, len(raw), len(g)))
        array(lines, name + "_GZ", g)
        lines.append("const size_t %s_GZ_LEN = %d;" % (name, len(g)))
        lines.append('const char %s_VERSION[] = "%s";' % (name, version))
        lines.append("")
    lines += ["}  // namespace web", "}  // namespace fc", ""]
    return "\n".join(lines)


def page_sizes(built):
    """page -> (shell, initial, total) gzip bytes; initial = shell + initial assets, total = + its lazy scripts."""
    table = {name: len(g) for name, g, _, _ in built}
    out = {}
    for p, deps in PAGES.items():
        if not (WEB / p).is_file():
            continue
        for d in deps["initial"] + deps["lazy"]:
            if d not in table:
                raise SystemExit("page %s: unknown asset %s" % (p, d))
        shell = len(gz(page(WEB / p, built)))
        initial = shell + sum(table[d] for d in deps["initial"])
        out[p] = (shell, initial, initial + sum(table[d] for d in deps["lazy"]))
    return out


def lazy_assets(built):
    """Every asset no page loads at once (lazy scripts, data packs, licenses.txt; the favicon too)."""
    initial = {d for deps in PAGES.values() for d in deps["initial"]}
    return [(name, len(g)) for name, g, _, _ in built if name not in initial]


def flash_total(built):
    """gzip bytes of every asset and every page shell baked into the image."""
    return sum(len(g) for _, g, _, _ in built) + sum(len(gz(page(p, built))) for p in sorted(WEB.glob("*.html")))


def flash_split(built):
    """(code, assets, packs) -- the flash total in its three budgets (WEB_CODE_MAX, WEB_ASSET_MAX, WEB_LANG_MAX)."""
    packs = {name for name, _, _, _ in lang_packs()}
    p = sum(len(g) for name, g, _, _ in built if name in packs)
    a = sum(len(g) for name, g, _, _ in built if name.endswith(WEB_ASSET_EXT) and name not in packs)
    return flash_total(built) - a - p, a, p


def cold_load(built):
    """(page, lang) -> a translated page's initial bytes for a first visit in that language (initial + the initial
    scripts' packs)."""
    sizes, pages = {name: len(g) for name, g, _, _ in built}, page_sizes(built)
    return {(page, lang): pages[page][1] + sum(sizes.get("%s.%s.json" % (a.rsplit(".", 1)[0], lang), 0)
                                              for a in PAGES[page]["initial"] if a.endswith(".js"))
            for page in LANG_PAGES for lang in lang_sources()}


def over_budget(built):
    flash = flash_total(built)
    return ["%s: initial %d B > %d" % (p, i, BUDGET_INITIAL[p]) for p, (_, i, _) in page_sizes(built).items()
            if i > BUDGET_INITIAL[p]] + \
           ["%s: first visit in %s %d B > %d" % (p, lang, n, BUDGET_FIRST.get((p, lang), 0))
            for (p, lang), n in cold_load(built).items() if n > BUDGET_FIRST.get((p, lang), 0)] + \
           ["%s: lazy file %d B > %d" % (n, b, LAZY_FILE_MAX) for n, b in lazy_assets(built) if b > LAZY_FILE_MAX] + \
           (["flash: %d B > %d" % (flash, WEB_FLASH_MAX)] if flash > WEB_FLASH_MAX else []) + \
           ["flash %s: %d B > %d" % (k, v, m) for k, v, m in zip(("code", "assets", "packs"), flash_split(built),
                                                                 (WEB_CODE_MAX, WEB_ASSET_MAX, WEB_LANG_MAX)) if v > m]


def sizes():
    built = assets()
    lazy = dict(lazy_assets(built))
    for name, g, _, _ in built:
        print("asset %-17s %6d B gz%s" % (name, len(g), "  lazy (max %d)%s" % (
            LAZY_FILE_MAX, "  OVER" if len(g) > LAZY_FILE_MAX else "") if name in lazy else "  initial"))
    for p, (shell, initial, total) in page_sizes(built).items():
        print("page  %-12s shell %6d  initial %6d (budget %d)  with its lazy scripts %6d%s" % (
            p, shell, initial, BUDGET_INITIAL[p], total, "" if initial <= BUDGET_INITIAL[p] else "  OVER"))
    f = flash_total(built)
    print("flash all assets + pages %d B gz (max %d, %d B free; the image gate is the hard limit)%s" %
          (f, WEB_FLASH_MAX, WEB_FLASH_MAX - f, "" if f <= WEB_FLASH_MAX else "  OVER"))
    c, a, p = flash_split(built)
    print("flash code %d B (max %d, %d B free)%s, assets %d B (max %d, %d B free)%s, packs %d B (max %d, %d B free)%s" %
          (c, WEB_CODE_MAX, WEB_CODE_MAX - c, "  OVER" if c > WEB_CODE_MAX else "",
           a, WEB_ASSET_MAX, WEB_ASSET_MAX - a, "  OVER" if a > WEB_ASSET_MAX else "",
           p, WEB_LANG_MAX, WEB_LANG_MAX - p, "  OVER" if p > WEB_LANG_MAX else ""))
    for (page, lang), n in cold_load(built).items():
        b = BUDGET_FIRST.get((page, lang), 0)
        print("page  %-12s first visit in %s: initial + its packs %d (budget %d)%s" % (page, lang, n, b, "  OVER" if n > b else ""))


def main():
    if "--sizes" in sys.argv[1:]:
        sizes()
        return 0
    text = render()
    if "--check" in sys.argv[1:]:
        over = over_budget(assets())
        if over:
            print("over budget: " + "; ".join(over))
            return 1
        current = OUT.read_text(encoding="utf-8") if OUT.is_file() else ""
        if current != text:
            print("web_assets.h is stale: run python -I tools/gen_web_assets.py and commit the result")
            return 1
        print("web_assets.h is up to date")
        return 0
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
