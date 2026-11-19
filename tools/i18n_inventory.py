"""i18n_inventory.py -- read-only inventory of the user-visible strings of the web faces (firmware/fauxcloud/web).

What it does, per web file:
  1. lexes the JavaScript (comments, '…', "…", `…` with nested ${…}, regular expressions) and the HTML shells
     (text nodes and the user-facing attributes; <script>, <style> and the SVG drawing skipped); app.css `content:`;
  2. cuts each literal into messages: a literal with markup is parsed as an HTML fragment, inline tags (b, i, em,
     strong, a, code, br, sup, sub, abbr, kbd) stay inside the message, block tags end one; the values of aria-label,
     title, placeholder, alt, label and aria-description are messages of their own; ${…} becomes a slot "{}";
  3. classifies each message as text (a translator would see it) or code (keys, selectors, units, codes, SVG path
     data, CSS, URLs, MIME types, event and key names), with the heuristics in classify() below;
  4. says where each text lives: in the file's `W` table, in another keyed table (`const NAME = {`, upper-case name),
     or inline (a template, an attribute, an argument);
  5. measures the language packs: per face (dash / admin / setup, the files each page serves, from
     tools/gen_web_assets.py BUNDLES + PAGES and the setup page's includes), a JSON of key -> English text, compact,
     UTF-8, gzip -9 (stdlib; the image uses zopfli, about 5 % smaller), plus a pseudo-Polish pack built from it (see
     pseudo_pl()) and a x1.15-1.25 band for real Polish text.

The script never writes into the repository; --json writes where it is told.

Usage (stdlib only):
    python -I tools/i18n_inventory.py                 # the summary tables
    python -I tools/i18n_inventory.py --list FILE     # every message of one web file with its class and place
    python -I tools/i18n_inventory.py --sample 50     # a stratified sample for the hand calibration (--seed N)
    python -I tools/i18n_inventory.py --refactor      # the prerequisite refactor: texts by place and shape, bundle cost
    python -I tools/i18n_inventory.py --reuse         # duplicates, what a common table saves, the lint's first run
    python -I tools/i18n_inventory.py --json OUT      # every message as JSON (for a later coverage check)
"""
import gzip
import html.parser
import importlib.util
import json
import pathlib
import random
import re
import sys

sys.dont_write_bytecode = True  # read-only: loading gen_web_assets.py and the pins leaves no __pycache__ behind
ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB = ROOT / "firmware" / "fauxcloud" / "web"
SRC = ROOT / "firmware" / "fauxcloud" / "src"
TESTS = ROOT / "test" / "tools" / "test_ui_text.py"


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------- the faces, from the asset generator
def faces():
    """face -> ordered list of web file names it serves (scripts and the shell)."""
    g = load(ROOT / "tools" / "gen_web_assets.py", "gen_web_assets_ro")
    out = {}
    for page, face in (("index.html", "dash"), ("admin.html", "admin"), ("setup.html", "setup")):
        files = [page]
        for asset in g.PAGES[page]["initial"] + g.PAGES[page]["lazy"]:
            src = g.BUNDLES.get(asset)
            if isinstance(src, list):
                files += [s for s in src if s.endswith(".js")]
        files += re.findall(r"/\*@include ([A-Za-z0-9_.-]+)\*/", (WEB / page).read_text(encoding="utf-8"))
        seen = []
        for f in files:
            if f not in seen:
                seen.append(f)
        out[face] = seen
    return out


# ---------------------------------------------------------------- JavaScript lexer
KEYWORDS_BEFORE_REGEX = {"return", "typeof", "case", "do", "else", "in", "of", "new", "delete", "void", "throw",
                         "instanceof", "yield", "await"}


class Lit:
    __slots__ = ("file", "line", "pos", "end", "kind", "text", "slots", "before", "after", "table", "key")

    def __init__(self, file, src, pos, end, kind, text, slots):
        self.file, self.pos, self.end, self.kind, self.text, self.slots = file, pos, end, kind, text, slots
        self.line = src.count("\n", 0, pos) + 1
        self.before = src[max(0, pos - 80):pos]
        self.after = src[end:end + 120]
        self.table = None
        self.key = None


def unescape(s):
    def rep(m):
        e = m.group(1)
        if e[0] == "u":
            return chr(int(e[1:].strip("{}"), 16))
        if e[0] == "x":
            return chr(int(e[1:], 16))
        return {"n": "\n", "t": "\t", "r": "", "0": "\0"}.get(e, e)
    return re.sub(r"\\(u\{[0-9a-fA-F]+\}|u[0-9a-fA-F]{4}|x[0-9a-fA-F]{2}|.)", rep, s, flags=re.S)


class JsLexer:
    def __init__(self, file, src):
        self.file, self.src, self.n = file, src, len(src)
        self.lits = []
        self.tables = []  # (name, start, end) of `const NAME = {` object literals
        self.wns = {}  # start of a W table -> its namespace (the name FC.w registers it under)

    def run(self):
        self.code(0, False)
        self.lits.sort(key=lambda l: l.pos)
        return self

    def code(self, i, until_brace):
        s, n = self.src, self.n
        depth, prev = 0, ("start", "")
        opens = []  # stack of (brace index, table name or None)
        while i < n:
            c = s[i]
            if c in " \t\r\n":
                i += 1
                continue
            if s.startswith("//", i):
                j = s.find("\n", i)
                i = n if j < 0 else j
                continue
            if s.startswith("/*", i):
                j = s.find("*/", i + 2)
                i = n if j < 0 else j + 2
                continue
            if c in "'\"":
                j = i + 1
                while j < n and s[j] != c:
                    j += 2 if s[j] == "\\" else 1
                self.lits.append(Lit(self.file, s, i, j + 1, "q", unescape(s[i + 1:j]), 0))
                i, prev = j + 1, ("str", "")
                continue
            if c == "`":
                i, prev = self.template(i), ("str", "")
                continue
            if c == "/" and (prev[0] in ("start", "re") or (prev[0] == "p" and prev[1] not in ")]") or
                             (prev[0] == "id" and prev[1] in KEYWORDS_BEFORE_REGEX)):
                j, cls = i + 1, False
                while j < n and (cls or s[j] != "/") and s[j] != "\n":
                    if s[j] == "\\":
                        j += 1
                    elif s[j] == "[":
                        cls = True
                    elif s[j] == "]":
                        cls = False
                    j += 1
                j += 1
                while j < n and s[j].isalpha():
                    j += 1
                i, prev = j, ("re", "")
                continue
            if c == "{":
                m = re.search(r"(?:const|let|var)\s+([A-Z][A-Z0-9_]*)\s*=\s*$", s[max(0, i - 60):i])
                w = re.search(r"FC\.w\(\s*'([a-z0-9-]+)'\s*,\s*$", s[max(0, i - 60):i])  # const W = FC.w('file', {
                if w:
                    self.wns[i] = w.group(1)
                elif m and m.group(1) == "W":
                    self.wns[i] = self.file.rsplit(".", 1)[0]  # outlook.js: const W = { (lang.js registers it)
                opens.append((i, "W" if w else m.group(1) if m else None))
                depth += 1
            elif c == "}":
                if until_brace and depth == 0:
                    return i + 1
                depth -= 1
                if opens:
                    start, name = opens.pop()
                    if name:
                        self.tables.append((name, start, i))
            if c.isalpha() or c in "_$":
                m = re.match(r"[A-Za-z_$][\w$]*", s[i:])
                prev, i = ("id", m.group(0)), i + m.end()
                continue
            if c.isdigit():
                m = re.match(r"[0-9.eExXa-fA-F_n]+", s[i:])
                prev, i = ("num", ""), i + m.end()
                continue
            prev, i = ("p", c), i + 1
        return i

    def template(self, i):
        s, n = self.src, self.n
        start, j, parts, buf = i, i + 1, [], []
        while j < n:
            c = s[j]
            if c == "\\":
                buf.append(s[j:j + 2])
                j += 2
                continue
            if c == "`":
                break
            if s.startswith("${", j):
                parts.append(unescape("".join(buf)))
                buf = []
                j = self.code(j + 2, True)
                continue
            buf.append(c)
            j += 1
        parts.append(unescape("".join(buf)))
        self.lits.append(Lit(self.file, s, start, j + 1, "t", "{}".join(parts), len(parts) - 1))
        return j + 1


# ---------------------------------------------------------------- messages inside a literal
INLINE_TAGS = {"b", "i", "em", "strong", "a", "code", "br", "sup", "sub", "abbr", "kbd"}
TEXT_ATTRS = {"aria-label", "title", "placeholder", "alt", "label", "aria-description", "aria-roledescription"}
SKIP_TAGS = {"script", "style", "svg"}
SVG_TEXT = {"text", "tspan", "title"}


class Frag(html.parser.HTMLParser):
    """An HTML fragment -> [(message, where)], where = "text" or the attribute name."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.out, self.buf, self.skip = [], [], 0
        self.svg_text = 0

    def flush(self):
        t = re.sub(r"\s+", " ", "".join(self.buf)).strip()
        self.buf = []
        if re.sub(r"</?\w+>", "", t).strip():
            self.out.append((t, "text"))

    def handle_starttag(self, tag, attrs):
        for k, v in attrs:
            if k in TEXT_ATTRS and v and v.strip():
                self.out.append((re.sub(r"\s+", " ", v).strip(), k))
        if tag in SKIP_TAGS:
            self.flush()
            self.skip += 1
            return
        if self.skip and tag in SVG_TEXT:
            self.svg_text += 1
            return
        if tag in INLINE_TAGS:
            self.buf.append("<%s>" % tag if tag != "br" else "<br>")
        else:
            self.flush()

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag in SKIP_TAGS:
            self.skip -= 1

    def handle_endtag(self, tag):
        if tag in SKIP_TAGS:
            self.skip = max(0, self.skip - 1)
            return
        if self.skip and tag in SVG_TEXT:
            self.flush()
            self.svg_text = max(0, self.svg_text - 1)
            return
        if tag in INLINE_TAGS:
            if tag != "br":
                self.buf.append("</%s>" % tag)
        else:
            self.flush()

    def handle_data(self, data):
        if not self.skip or self.svg_text:
            self.buf.append(data)

    def close(self):
        super().close()
        self.flush()


MARKUP = re.compile(r"<[a-zA-Z/!]")
ATTR_IN_TEXT = re.compile(r"\b(%s)=\"([^\"]*)\"" % "|".join(sorted(TEXT_ATTRS)))


def messages(text):
    """A literal -> [(message, where)]."""
    if MARKUP.search(text):
        p = Frag()
        try:
            p.feed(text)
            p.close()
        except Exception:
            return [(text, "raw")]
        return p.out
    attrs = ATTR_IN_TEXT.findall(text)
    if attrs:
        return [(v, k) for k, v in attrs if v.strip()]
    return [(text, "lit")]


# ---------------------------------------------------------------- text or code
UNITS = {"°C", "°F", "°", "km/h", "mph", "mm", "in", "hPa", "inHg", "%", "W/m²", "dBm", "m/s", "kt", "KB", "B", "ms",
         "s", "min", "h", "d", "lx", "UV", "mm/h", "in/h", "MB", "kB", "hPa/h", "µg/m³", "ppm", "m", "km", "ft", "mi"}
COMPASS = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"}
CODE_WORDS = set("""
a abbr b body br button canvas circle code details div em footer form g h1 h2 h3 h4 head header html i img input label
li line link main meta nav ol option p path polygon polyline rect script section select small span strong style sub
summary sup svg table tbody td text textarea th thead time tr tspan ul use
click change input keydown keyup pointerdown pointermove pointerup pointercancel scroll resize load error submit focus
blur visibilitychange hashchange popstate transitionend animationend toggle close open message storage pageshow
none auto block inline flex grid hidden visible absolute relative fixed static solid round butt ease linear smooth
transparent currentColor inherit initial unset normal bold italic nowrap wrap center left right top bottom middle start
end evenodd nonzero stroke fill opacity transform transition display width height color background
string number boolean object function undefined symbol bigint true false null
get post put delete patch head GET POST PUT DELETE PATCH HEAD json text blob default no-store same-origin
dark light day night imperial metric beforeend afterbegin afterend beforebegin
""".split())
CSS_VALUE = re.compile(r"^[\w.#%()\-, ]*(?:\d(?:px|em|rem|s|ms|%|deg|vh|vw)\b|rgba?\(|translate[XY]?\(|var\(--|cubic-bezier)")
SVG_PATH = re.compile(r"^[MmLlHhVvCcSsQqTtAaZz0-9 .,\-]+$")
IDENT = re.compile(r"^(?:[a-z][a-z0-9]*(?:[_\-.][a-z0-9]+)+|[a-z]+[A-Z]\w*|[A-Z][A-Z0-9_]+[_0-9][A-Z0-9_]*|[a-z]{1,3}\d*|"
                   r"--[\w-]+|[\w-]+\.(?:js|json|css|svg|png|txt|html|bin|csv)|\w+/[\w.+-]+)$")
URLISH = re.compile(r"(?:^[/#.\[:@]|://|^\w+=|^\?|^&|^data:|^mailto:|^[\w-]+:\s|^[\w.-]+@)")
CODEY = re.compile(r"=>|==|\|\||&&|\$\(|\bfunction\b|^\s*[\w-]+\s*:\s*[\w#-]+\s*;?$|^\W*$|^use strict$")
CODEY_WEAK = re.compile(r"[;{}]")  # code only when most of the words are not plain words (prose has semicolons)
WORD = re.compile(r"^[(“\"‘]?[A-Za-zÀ-ž’'][A-Za-zÀ-ž’'\-]*[.,:;!?…)”\"]*$")
NEUTRAL = re.compile(r"^(?:\{\w*\}|[·–—→←/+×=|•…]+)$")  # placeholders and lone separators: neither word nor code
HEADERS_KEYS = re.compile(r"^(?:[A-Z][a-z]+(?:-[A-Z][a-z]+)+|Arrow\w+|Escape|Enter|Tab|Home|End|Page\w+|Backspace|"
                          r"Content-\w+|X-[\w-]+|If-[\w-]+)$")


def comparison(before, after):
    b = before.rstrip()
    return bool(re.search(r"(===|!==|==|!=|\bcase|\bin)\s*$", b) or re.match(r"\s*(===|!==|==|!=|\s+in\b)", after))


def dom_arg(before):
    return bool(re.search(r"(?:\$|querySelector(?:All)?|getElementById|addEventListener|removeEventListener|createElement(?:NS)?|"
                          r"[gs]etAttribute|removeAttribute|hasAttribute|toggleAttribute|classList\.\w+|closest|matches|"
                          r"fetch|\.get|\.set|\.has|\.delete|store|includes|startsWith|endsWith|split|join|indexOf|lazy|"
                          r"poll|kick|enable|getItem|setItem|removeItem|dispatchEvent|Event|padStart|padEnd|replace|"
                          r"getPropertyValue|setProperty|new\s+Intl\.\w+|toLocale\w*String|FC\.\w+|matchMedia|own|"
                          r"svg|ic|icon|glyph|g)\(\s*(?:[^()]*,\s*)?$", before))


def obj_key(before, after):
    return bool(re.match(r"\s*:", after)) and bool(re.search(r"[{,]\s*$", before))


_VOCAB = None
_PINNED = None


def vocab():
    """Identifiers the code uses: object keys and property names in the scripts, classes and ids in app.css, JSON keys
    the firmware prints. A bare lower-case word in this set is taken as code."""
    global _VOCAB
    if _VOCAB is None:
        v = set()
        for p in WEB.glob("*.js"):
            s = p.read_text(encoding="utf-8")
            v.update(re.findall(r"[{,]\s*['\"]?([a-z]\w*)['\"]?\s*:", s))
            v.update(re.findall(r"\.([a-z]\w*)", s))
            v.update(re.findall(r"data-([a-z][\w-]*)", s))
            v.update(re.findall(r"(?m)^\s*['\"]?([a-z]\w*)['\"]?\s*:", s))
        v.update(re.findall(r"[.#]([a-z][\w-]*)", (WEB / "app.css").read_text(encoding="utf-8")))
        for p in list(SRC.glob("*.cpp")) + list(SRC.glob("*.h")):
            if p.name != "web_assets.h":
                v.update(re.findall(r'\\"([a-z]\w*)\\"', p.read_text(encoding="utf-8", errors="replace")))
        _VOCAB = v
    return _VOCAB


def pinned():
    """The bare words the pins quote ('now', 'from', ...): user-visible by definition."""
    global _PINNED
    if _PINNED is None:
        pins = load(TESTS, "test_ui_text_ro").NEW
        _PINNED = {p[1:-1] for v in pins.values() for p in v if len(p) > 2 and p[0] == p[-1] and p[0] in "'\""}
    return _PINNED


def classify(msg, lit=None):
    """'text' | 'code' | 'unit' (a unit, a compass point or a bare symbol: not translated as a sentence)."""
    t = re.sub(r"</?\w+>", " ", msg).replace("{}", " ").strip()
    if not re.search(r"[A-Za-zÀ-ž]", t):
        return "code"
    if msg in pinned():
        return "text"
    if t in UNITS or t.rstrip(".") in UNITS:
        return "unit"
    if t in COMPASS:
        return "unit"
    if lit is not None and lit.kind == "q" and msg == lit.text and (comparison(lit.before, lit.after) or
                                                                   obj_key(lit.before, lit.after) or dom_arg(lit.before)):
        return "code"
    if lit is not None and re.search(r"\[\s*$", lit.before) and re.match(r"\s*\]", lit.after):
        return "code"
    if lit is not None and re.match(r"(?:\s*,\s*'[^']*')*\s*\]\s*\.(?:includes|indexOf|forEach|map|some|every)", lit.after):
        return "code"  # ['sheet', 'scrim', 'toast'].includes(id)
    if URLISH.search(t) or CODEY.search(t) or HEADERS_KEYS.match(t) or SVG_PATH.match(t) or CSS_VALUE.match(t):
        return "code"
    words = [w for w in t.split() if not NEUTRAL.match(w)]
    if not words:
        return "code"
    if all(w in UNITS for w in words):
        return "unit"  # "in → mm", "°F → °C"
    plain = [w for w in words if WORD.match(w)]
    if lit is not None and lit.kind == "q" and len(words) == 1 and re.match(r"^[a-z][a-z0-9]*$", words[0]) and             re.search(r"\w\(\s*$", lit.before) and re.match(r"\s*,", lit.after):
        return "code"  # an id as the first argument: field('cdpw', 'Your admin password…')
    if len(words) == 1:
        w = words[0]
        if IDENT.match(w) or w in CODE_WORDS or w.lower() in CODE_WORDS and w[0].islower():
            return "code"
        if re.match(r"^[A-Z][a-z’']+[.,:!?…]*$", w) or re.match(r"^[a-z’']{2,}[.,:!?…]+$", w):
            return "text"  # "Wind", "Calm.", "now:"
        if re.match(r"^[a-z’']{2,}$", w):
            return "code" if w in vocab() else "text"  # a bare lower-case word in a value position: "now", "from" (the sample measures this)
        if re.match(r"^[A-Z][a-zA-Z’']*[a-z][a-zA-Z’']*$", w):
            return "text"  # "WiFi", "GitHub"-like: a name
        return "code"
    if CODEY_WEAK.search(t) and len(plain) / len(words) < .6:
        return "code"
    if len(plain) >= 2 and len(plain) / len(words) >= .5:
        if all(w.strip(".,:;").lower() in CODE_WORDS for w in plain):
            return "code"  # "transform .2s ease", "bold italic"
        return "text"
    return "code"


# ---------------------------------------------------------------- per file
def table_of(lit, tables):
    best = None
    for name, a, b in tables:
        if a < lit.pos < b and (best is None or a > best[1]):
            best = (name, a, b)
    return best[0] if best else None


def key_of(lit):
    m = re.search(r"(?:^|[{,\s])(['\"]?)([\w$-]+)\1\s*:\s*$", lit.before)
    return m.group(2) if m else None


def scan_js(name):
    src = (WEB / name).read_text(encoding="utf-8")
    lx = JsLexer(name, src).run()
    out = []
    for lit in lx.lits:
        lit.table = table_of(lit, lx.tables)
        lit.key = key_of(lit) if lit.table else None
        for msg, where in messages(lit.text):
            cls = classify(msg, lit if where in ("lit", "raw") else None)
            out.append({"file": name, "line": lit.line, "kind": lit.kind, "where": where, "msg": msg, "cls": cls,
                        "table": lit.table, "key": lit.key, "slots": msg.count("{}"),
                        "markup": bool(re.search(r"</?\w+>", msg)),
                        "concat": bool(re.match(r"\s*\+", lit.after) or re.search(r"\+\s*$", lit.before)),
                        "lit": lit.text if len(lit.text) < 400 else lit.text[:400] + "…"})
    return out


def scan_html(name):
    text = (WEB / name).read_text(encoding="utf-8")
    text = re.sub(r"/\*@\w+ [^*]*\*/", "", text)
    text = re.sub(r"<script>.*?</script>", "", text, flags=re.S)
    p = Frag()
    p.feed(text)
    p.close()
    out = []
    for msg, where in p.out:
        cls = classify(msg)
        line = 1 + text[:max(0, text.find(msg[:20]))].count("\n") if msg[:20] in text else 0
        out.append({"file": name, "line": line, "kind": "html", "where": where, "msg": msg, "cls": cls,
                    "table": None, "key": None, "slots": 0, "markup": bool(re.search(r"</?\w+>", msg)),
                    "concat": False, "lit": msg})
    return out


def scan_css():
    out = []
    for i, line in enumerate((WEB / "app.css").read_text(encoding="utf-8").splitlines(), 1):
        for m in re.finditer(r"content\s*:\s*\"([^\"]*)\"", line):
            if re.search(r"[A-Za-z]{2}", m.group(1)):
                out.append({"file": "app.css", "line": i, "kind": "css", "where": "content", "msg": m.group(1),
                            "cls": classify(m.group(1)), "table": None, "key": None, "slots": 0, "markup": False,
                            "concat": False, "lit": m.group(1)})
    return out


def inventory():
    rows = []
    for p in sorted(WEB.glob("*.js")):
        rows += scan_js(p.name)
    for p in sorted(WEB.glob("*.html")):
        rows += scan_html(p.name)
    rows += scan_css()
    return rows


# ---------------------------------------------------------------- packs
def gz(b):
    return len(gzip.compress(b, compresslevel=9, mtime=0))


def place(r):
    return "W" if r["table"] == "W" else "table" if r["table"] else "inline"


def pack_items(rows, files):
    """[(key, text)] for the distinct texts of these files: key = <file stem>.<table key or sN>."""
    items, seen = [], set()
    for f in files:
        n = 0
        for r in rows:
            if r["file"] != f or r["cls"] != "text":
                continue
            msg = r["msg"].replace("{}", "{%d}" % 0) if r["slots"] == 1 else _slots(r["msg"])
            if (f, msg) in seen:
                continue
            seen.add((f, msg))
            n += 1
            stem = f.rsplit(".", 1)[0]
            k = "%s.%s" % (stem, r["key"] if r["key"] and r["table"] else "s%d" % n)
            items.append((k, msg))
    return items


SETUP_ONLY = {"setup.html", "setup.js"}  # the setup face stays English: no pack, its texts still go into W


def translated_files(rows):
    """The files whose texts get packs: the dashboard and admin faces (the setup face stays English)."""
    fc = faces()
    out = []
    for face in ("dash", "admin"):
        out += [f for f in fc[face] if f not in out and f not in SETUP_ONLY and any(r["file"] == f for r in rows)]
    return out


def norm(msg):
    """Near-duplicate form: case folded, punctuation and inline tags dropped, spaces collapsed."""
    t = re.sub(r"</?\w+>", " ", msg).replace("{}", " ")
    return re.sub(r"\s+", " ", re.sub(r"[^\w\s{}]", " ", t.casefold())).strip()


def short_label(msg):
    """The dedup rule: a short label (<= 3 words), capitalised, no slot, no markup, no sentence end, no edge space."""
    return (msg == msg.strip() and len(msg.split()) <= 3 and re.match(r"^[A-Z]", msg) is not None
            and not re.search(r"[{}<>]", msg) and not re.search(r"[.?!:;]$", msg))


# a curated common table: labels with one role everywhere (actions, time words, password
# labels); weather nouns and state words stay per file (a chart legend, a value and a forecast phenomenon, or a
# gendered state word, translate differently)
CURATED_COMMON = {"Cancel", "Save", "Remove", "Add", "Edit", "Copy", "Close", "Restart", "Try again", "Sign out",
                  "Fetch now", "Fetching…", "Loading…", "Today", "Yesterday", "Tomorrow", "Morning", "Afternoon", "Now",
                  "Show table", "Switch theme", "Change password", "Admin password", "New password", "Password",
                  "Date range", "Add an airport"}


def reuse(rows):
    files = translated_files(rows)
    t = [r for r in rows if r["cls"] == "text" and r["file"] in files]
    by_text = {}
    for r in t:
        by_text.setdefault(r["msg"], []).append(r["file"])
    distinct_pairs = {(r["file"], r["msg"]) for r in t}
    multi = {m: sorted(set(fs)) for m, fs in by_text.items() if len(set(fs)) > 1}
    print("## Reuse in the translated faces (dash + admin; %d files)\n" % len(files))
    print("| measure | count |")
    print("|---|---:|")
    print("| text occurrences | %d |" % len(t))
    print("| distinct (file, text) = keys with one key per text per file | %d |" % len(distinct_pairs))
    print("| distinct texts over all files | %d |" % len(by_text))
    print("| repeats inside one file (occurrences beyond the first) | %d |" % (len(t) - len(distinct_pairs)))
    print("| texts found in 2+ files | %d (%d keys today) |" % (len(multi), sum(len(v) for v in multi.values())))
    short = {m: v for m, v in multi.items() if short_label(m)}
    print("| … of them short labels (the dedup rule) | %d (%d keys) |" % (len(short), sum(len(v) for v in short.values())))
    groups = {}
    for m in by_text:
        groups.setdefault(norm(m), set()).add(m)
    near = {k: v for k, v in groups.items() if len(v) > 1 and k}
    print("| near-duplicate groups (same words, other case or punctuation) | %d (%d texts) |" % (
        len(near), sum(len(v) for v in near.values())))

    g = load(ROOT / "tools" / "gen_web_assets.py", "gen_web_assets_ro4")
    per_asset = {n: [f for f in s if f in files] for n, s in g.BUNDLES.items() if isinstance(s, list) and n.endswith(".js")}
    per_asset["status.js"] += ["index.html"]
    per_asset["admin.js"] += ["admin.html"]

    def measure(shared):
        """(keys, EN gz, pPL gz) of the per-asset packs with the texts in `shared` moved to one common pack."""
        keys = e = p = 0
        for name, fl in per_asset.items():
            items = [(k, v) for k, v in pack_items(rows, fl) if v not in shared]
            if items:
                keys += len(items)
                e += pack_bytes(items)[1]
                p += pack_bytes(items, pseudo_pl)[1]
        if shared:
            items = [("common.c%d" % i, v) for i, v in enumerate(sorted(shared))]
            keys += len(items)
            e += pack_bytes(items)[1]
            p += pack_bytes(items, pseudo_pl)[1]
        return keys, e, p
    print("\n| dedup rule | keys to translate | EN gz (per-asset packs + common) | pseudo-PL gz | saving pPL |")
    print("|---|---:|---:|---:|---:|")
    base = measure(set())
    curated = set(short) & CURATED_COMMON
    for label, shared in (("none (one key per text per file)", set()),
                          ("curated: actions, time words, password labels (%d)" % len(curated), curated),
                          ("short labels with the same text in 2+ files (rule)", set(short)),
                          ("every text found in 2+ files (full)", set(multi))):
        k, e, p = measure(shared)
        print("| %s | %d | %d | %d | %+d |" % (label, k, e, p, p - base[2]))
    print("\nshort labels shared by 2+ files (text: files):")
    for m in sorted(short, key=lambda m: (-len(short[m]), m)):
        print("  %-28s %s" % (m, ", ".join(short[m])))
    print("\nlonger texts shared by 2+ files:")
    for m in sorted(set(multi) - set(short), key=lambda m: (-len(multi[m]), m)):
        print("  %-60s %s" % (m[:60], ", ".join(multi[m])))
    print("\nnear-duplicate groups:")
    for k in sorted(near, key=lambda k: -len(near[k])):
        print("  " + " | ".join(sorted(near[k])))


def lint_tiers(rows):
    """What a --check would report today: every text outside W, split into the strict tier (capitalised, several
    words, or sentence punctuation) and the soft tier (bare lower-case words: where the false positives sit)."""
    t = [r for r in rows if r["cls"] == "text" and r["table"] != "W"]
    soft = [r for r in t if re.match(r"^[a-z’'][a-z’' ]*$", r["msg"]) and len(r["msg"].split()) <= 2]
    print("lint first run: %d texts outside W in %d files; strict tier %d, soft tier (bare lower-case 1-2 words) %d"
          % (len(t), len({r["file"] for r in t}), len(t) - len(soft), len(soft)))
    for f in sorted({r["file"] for r in t}):
        a = sum(r["file"] == f for r in t)
        b = sum(r["file"] == f for r in soft)
        print("  %-18s %4d  (soft %d)" % (f, a, b))


def _slots(msg):
    i = [0]

    def rep(_):
        i[0] += 1
        return "{%d}" % (i[0] - 1)
    return re.sub(r"\{\}", rep, msg)


# a deterministic pseudo-Polish: a letter substitution (no substring shared with the English, so gzip cannot reuse the
# keys' English), ~9 % of the letters as two-byte Polish letters, and one extra letter after every fifth (+20 %
# length). Placeholders, digits, punctuation and inline tags stay.
# (code points, not letters: the leak gate keeps Polish letters out of the source tree)
_PL = "ozwy%se%sc%smnipkstrsu%sa%sd%sgr" % tuple(chr(c) for c in (0x105, 0x119, 0x142, 0xF3, 0x15B, 0x17C))
_SUB = dict(zip("abcdefghijklmnopqrstuvwxyz", _PL))
_SUB.update({c.upper(): v.upper() for c, v in _SUB.items()})


def pseudo_pl(text):
    out, letters = [], 0
    for part in re.split(r"(\{\w*\}|</?\w+>)", text):
        if re.fullmatch(r"\{\w*\}|</?\w+>", part or "x") and part:
            out.append(part)
            continue
        for c in part:
            if c in _SUB:
                out.append(_SUB[c])
                letters += 1
                if letters % 5 == 0:
                    out.append("y")
            else:
                out.append(c)
    return "".join(out)


def pack_bytes(items, transform=None):
    d = {k: (transform(v) if transform else v) for k, v in items}
    raw = json.dumps(d, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return len(raw), gz(raw)


# ---------------------------------------------------------------- reports
def summary(rows):
    fc = faces()
    files = sorted({r["file"] for r in rows}, key=lambda f: (f.endswith(".html"), f))
    print("## Per file (messages; text = what a translator sees)\n")
    print("| file | text msgs | distinct | in W | other table | inline | slots>0 | markup | concat | unit/compass | text bytes |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    tot = [0] * 10
    for f in files:
        t = [r for r in rows if r["file"] == f and r["cls"] == "text"]
        u = [r for r in rows if r["file"] == f and r["cls"] == "unit"]
        d = {r["msg"] for r in t}
        c = [len(t), len(d), sum(place(r) == "W" for r in t), sum(place(r) == "table" for r in t),
             sum(place(r) == "inline" for r in t), sum(r["slots"] > 0 for r in t), sum(r["markup"] for r in t),
             sum(r["concat"] for r in t), len(u), sum(len(m.encode()) for m in d)]
        tot = [a + b for a, b in zip(tot, c)]
        print("| %s | %s |" % (f, " | ".join(str(x) for x in c)))
    print("| **all** | %s |" % " | ".join("**%d**" % x for x in tot))
    allcode = [r for r in rows if r["cls"] == "code"]
    print("\ncode messages (not text): %d; with letters: %d" % (
        len(allcode), sum(bool(re.search("[A-Za-z]", r["msg"])) for r in allcode)))

    print("\n## Packs per face (key -> English text, compact JSON, gzip -9)\n")
    print("| face | files | keys | EN raw | EN gz | pseudo-PL raw | pseudo-PL gz | x1.15 | x1.25 | texts-only gz |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    allfiles = []
    for face, fl in fc.items():
        fl = [f for f in fl if any(r["file"] == f for r in rows)]
        allfiles += [f for f in fl if f not in allfiles]
        items = pack_items(rows, fl)
        er, eg = pack_bytes(items)
        pr, pg = pack_bytes(items, pseudo_pl)
        tg = gz(json.dumps([v for _, v in items], ensure_ascii=False, separators=(",", ":")).encode())
        print("| %s | %d | %d | %d | %d | %d | %d | %d | %d | %d |" % (face, len(fl), len(items), er, eg, pr, pg,
                                                                    round(eg * 1.15), round(eg * 1.25), tg))
    shared = [f for f in allfiles if sum(f in fl for fl in fc.values()) > 1]
    items = pack_items(rows, allfiles)
    er, eg = pack_bytes(items)
    pr, pg = pack_bytes(items, pseudo_pl)
    print("| one pack per language (all files once) | %d | %d | %d | %d | %d | %d | %d | %d | |" % (
        len(allfiles), len(items), er, eg, pr, pg, round(eg * 1.15), round(eg * 1.25)))
    print("\nshared by two or more faces: %s" % ", ".join(shared))
    for face, fl in fc.items():
        own = [f for f in fl if f not in shared and any(r["file"] == f for r in rows)]
        items = pack_items(rows, own)
        print("face-only %-5s: %3d keys, EN gz %5d, pseudo-PL gz %5d" % (face, len(items), pack_bytes(items)[1],
                                                                     pack_bytes(items, pseudo_pl)[1]))
    items = pack_items(rows, [f for f in shared if any(r["file"] == f for r in rows)])
    print("common       : %3d keys, EN gz %5d, pseudo-PL gz %5d" % (len(items), pack_bytes(items)[1],
                                                                 pack_bytes(items, pseudo_pl)[1]))
    print("\nper file (EN gz / pseudo-PL gz of that file's keys alone):")
    for f in files:
        items = pack_items(rows, [f])
        if items:
            print("  %-18s %4d keys  EN %5d  pPL %5d" % (f, len(items), pack_bytes(items)[1], pack_bytes(items, pseudo_pl)[1]))
    # one pack per script asset (the pack follows its script: loaded with it, cached with it); the shell's texts go with
    # the page's main script, the setup page's with its inline script
    g = load(ROOT / "tools" / "gen_web_assets.py", "gen_web_assets_ro3")
    per_asset = {n: list(s) for n, s in g.BUNDLES.items() if isinstance(s, list) and n.endswith(".js")}
    per_asset["status.js"] = per_asset["status.js"] + ["index.html"]
    per_asset["admin.js"] = per_asset["admin.js"] + ["admin.html"]
    per_asset["setup.html"] = fc["setup"]
    print("\nper script asset (EN gz / pseudo-PL gz; the page's shell texts with its main script):")
    tot_e = tot_p = 0
    for name, fl in per_asset.items():
        items = pack_items(rows, fl)
        if items:
            e, p = pack_bytes(items)[1], pack_bytes(items, pseudo_pl)[1]
            tot_e, tot_p = tot_e + e, tot_p + p
            print("  %-18s %4d keys  EN %5d  pPL %5d" % (name, len(items), e, p))
    print("  %-18s            EN %5d  pPL %5d  (the flash of one language as per-asset packs)" % ("sum", tot_e, tot_p))
    # English as the key (gettext style): the Polish pack carries both
    items = pack_items(rows, allfiles)
    d = {v: pseudo_pl(v) for _, v in items}
    raw = json.dumps(d, ensure_ascii=False, separators=(",", ":")).encode()
    print("\ngettext style (English text as the key), one pack, pseudo-PL: raw %d, gz %d" % (len(raw), gz(raw)))

    pins = load(TESTS, "test_ui_text_ro").NEW
    allp = [p for v in pins.values() for p in v]
    syn = [p for p in allp if "${" in p or p[:1] in "'`\"" or p.startswith("<")]
    print("\npins in test_ui_text.py NEW: %d strings in %d lists; spelled with JS/HTML syntax ('…', `…`, ${…}, tags): %d" % (
        len(allp), len(pins), len(syn)))


def listing(rows, f):
    for r in rows:
        if r["file"] == f:
            print("%5d %-4s %-6s %-7s %-10s %s" % (r["line"], r["cls"], place(r), r["where"][:7], (r["key"] or "")[:10],
                                                  r["msg"][:150]))


def sample(rows, n, seed):
    rnd = random.Random(seed)
    text = [r for r in rows if r["cls"] == "text"]
    code = [r for r in rows if r["cls"] != "text" and re.search(r"[A-Za-z]{2}", r["msg"])]
    pick = rnd.sample(text, n // 2) + rnd.sample(code, n - n // 2)
    for i, r in enumerate(pick, 1):
        print("%2d | %s:%d | %s | %s | %s" % (i, r["file"], r["line"], r["cls"], place(r), r["msg"][:90].replace("|", "¦")))


def _min(text):
    # the generator's light minify (whole-line comments, indentation); terser is not run here
    return "\n".join(l.strip() for l in text.splitlines() if l.strip() and not l.strip().startswith("//")) + "\n"


def refactor_cost(key_len=7):
    """Bytes a move of the inline plain-string texts costs in the bundles, measured on the '…'/"…" literals outside any
    table (templates are left alone): variant W = a `const W = {key:'text',…}` table + `W.key` at the use; variant tr =
    `tr('text')` at the use (English as the key). gzip -9 of the lightly minified source, before and after."""
    g = load(ROOT / "tools" / "gen_web_assets.py", "gen_web_assets_ro2")
    bundles = {n: s for n, s in g.BUNDLES.items() if isinstance(s, list) and n.endswith(".js")}
    bundles["setup (inline)"] = re.findall(r"/\*@include ([A-Za-z0-9_.-]+)\*/", (WEB / "setup.html").read_text(encoding="utf-8"))
    print("| bundle | plain-string texts moved | gz before | gz W variant | delta | per string | gz tr() variant | delta | per string |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    tot = [0, 0, 0, 0]
    for name, files in bundles.items():
        orig, wv, tv, moved = [], [], [], 0
        for f in files:
            src = (WEB / f).read_text(encoding="utf-8")
            lx = JsLexer(f, src).run()
            edits = []
            for lit in lx.lits:
                if lit.kind != "q" or table_of(lit, lx.tables):
                    continue
                m = messages(lit.text)
                if len(m) == 1 and m[0][1] == "lit" and classify(m[0][0], lit) == "text":
                    edits.append(lit)
            w_src, t_src, table = src, src, []
            for i, lit in enumerate(sorted(edits, key=lambda l: -l.pos)):
                key = ("k%x" % i).ljust(key_len, "_")
                table.append("%s:%s" % (key, src[lit.pos:lit.end]))
                w_src = w_src[:lit.pos] + "W." + key + w_src[lit.end:]
                t_src = t_src[:lit.pos] + "tr(" + src[lit.pos:lit.end] + ")" + t_src[lit.end:]
            moved += len(edits)
            orig.append(_min(src))
            wv.append(_min("const W={%s};\n" % ",".join(reversed(table)) + w_src) if table else _min(src))
            tv.append(_min(t_src))
        a, b, c = (gz("\n".join(x).encode("utf-8")) for x in (orig, wv, tv))
        tot = [tot[0] + moved, tot[1] + a, tot[2] + b, tot[3] + c]
        print("| %s | %d | %d | %d | %+d | %s | %d | %+d | %s |" % (name, moved, a, b, b - a,
              "%.1f" % ((b - a) / moved) if moved else "–", c, c - a, "%.1f" % ((c - a) / moved) if moved else "–"))
    print("| **all** | %d | %d | %d | %+d | %.1f | %d | %+d | %.1f |" % (tot[0], tot[1], tot[2], tot[2] - tot[1],
          (tot[2] - tot[1]) / tot[0], tot[3], tot[3] - tot[1], (tot[3] - tot[1]) / tot[0]))


def refactor_size(rows):
    """What the prerequisite refactor touches: distinct texts by place, and the inline ones by shape."""
    t = [r for r in rows if r["cls"] == "text"]
    print("| place | occurrences | distinct (file, text) |")
    print("|---|---:|---:|")
    for pl in ("W", "table", "inline"):
        x = [r for r in t if place(r) == pl]
        print("| %s | %d | %d |" % (pl, len(x), len({(r["file"], r["msg"]) for r in x})))
    inl = [r for r in t if place(r) != "W"]
    shapes = {
        "plain string literal, whole message": lambda r: r["kind"] == "q" and r["where"] == "lit",
        "template text with ${…} slots": lambda r: r["slots"] > 0,
        "template/markup text, no slot": lambda r: r["kind"] in ("t", "q") and r["where"] == "text" and not r["slots"],
        "attribute (aria-label, title, placeholder…)": lambda r: r["where"] in TEXT_ATTRS,
        "HTML shell or CSS": lambda r: r["kind"] in ("html", "css"),
        "  of them carry inline markup (<b>, <a>…)": lambda r: r["markup"],
        "  of them are joined with + (rewrite as one message)": lambda r: r["concat"],
        "  of them start or end with a space (a fragment)": lambda r: r["msg"] != r["msg"].strip(),
    }
    print("\n| not yet in a W table, by shape | occurrences | distinct |")
    print("|---|---:|---:|")
    for k, f in shapes.items():
        x = [r for r in inl if f(r)]
        print("| %s | %d | %d |" % (k, len(x), len({(r["file"], r["msg"]) for r in x})))
    files = sorted({r["file"] for r in inl})
    print("\nfiles with texts outside W: %d (%s)" % (len(files), ", ".join(files)))


# ---------------------------------------------------------------- keys, the pack source, the lint
LANG_DIR = WEB / "lang"
ALLOW = ROOT / "tools" / "i18n-allow.txt"
SHELL_ATTR = {"t": None, "tl": "aria-label", "tt": "title"}
# namespaces whose English lives in the pack source only: the code registers an empty table that a pack fills. The
# firmware's notices (dash.js NT): English shows the words the firmware sends, so English carries no copy of them
# (test_notices_sync.py keeps the pack's "en" fields equal to src/notices.cpp)
PACK_ONLY = {"notice": "dash.js"}


def lang_files():
    """The translated faces' files (the dashboard and the admin: shells and scripts; setup stays English): the files with
    packs and the lint's scope."""
    fc, out = faces(), []
    for f in fc["dash"] + fc["admin"]:
        if f not in out and f not in SETUP_ONLY:
            out.append(f)
    return out


def w_literals(name):
    """[(namespace, object literal source)] of one script's W tables."""
    src = (WEB / name).read_text(encoding="utf-8")
    lx = JsLexer(name, src).run()
    return [(lx.wns[a], src[a:b + 1]) for n, a, b in sorted(lx.tables, key=lambda t: t[1]) if a in lx.wns]


def node_exe():
    import os
    import shutil
    exe = os.environ.get("FAUXCLOUD_NODE") or shutil.which("node")
    if not exe:
        raise SystemExit("i18n_inventory: node not found (set FAUXCLOUD_NODE)")
    return exe


def shell_keys(name):
    """{key: English} of a page shell's data-t / data-tl / data-tt words."""
    out = {}
    for m in re.finditer(r"<(\w+)([^>]*)>([^<]*)", (WEB / name).read_text(encoding="utf-8")):
        attrs = dict(re.findall(r'([\w-]+)="([^"]*)"', m.group(2)))
        for a, at in SHELL_ATTR.items():
            k = attrs.get("data-" + a)
            if k:
                out[k] = html.unescape(attrs.get(at, "") if at else m.group(3)).strip()
    return out


def keys(files):
    """{"<namespace>.<key>": English text or count entry} of the files' W tables (node evaluates the literals: they
    are plain data) and of the shells' data-t words, in file order."""
    import subprocess
    lits = [[ns, lit] for f in files if f.endswith(".js") for ns, lit in w_literals(f)]
    # a key twice in one namespace (two tables of one file) would take one Polish text for both: a failure
    js = ("let d='';process.stdin.on('data',c=>d+=c).on('end',()=>{const o={};for(const[n,l]of JSON.parse(d))"
          "{const t=new Function('return ('+l+')')();for(const k in t){if(n+'.'+k in o)throw new Error('duplicate key '+n+'.'+k);"
          "o[n+'.'+k]=t[k];}}process.stdout.write(JSON.stringify(o));});")
    r = subprocess.run([node_exe(), "-e", js], input=json.dumps(lits).encode("utf-8"), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode:
        raise SystemExit("i18n_inventory: a W table is not plain data: " + r.stderr.decode("utf-8", "replace")[-400:])
    out = json.loads(r.stdout.decode("utf-8"))
    for f in files:
        if f.endswith(".html"):
            out.update(shell_keys(f))
    out.update(pack_only_keys(files))
    return out


def pack_only_keys(files):
    """{"<namespace>.<key>": English} of the PACK_ONLY namespaces whose file is among these, from the packs' "en" fields
    (every pack must say the same English)."""
    out = {}
    for path in sorted(LANG_DIR.glob("*.json")):
        d = json.loads(path.read_text(encoding="utf-8"))
        for k, e in d.items():
            ns = k.split(".", 1)[0]
            if ns in PACK_ONLY and PACK_ONLY[ns] in files:
                if k in out and out[k] != e.get("en"):
                    raise SystemExit("i18n_inventory: %s: the packs disagree on its English" % k)
                out[k] = e.get("en")
    return out


def pack_path(lang):
    return LANG_DIR / ("%s.json" % lang)


def pack_format():
    """tools/i18n_pack.py: the source format 2 (the common table, references, their resolution)."""
    return load(ROOT / "tools" / "i18n_pack.py", "i18n_pack_ro")


def write_pack(path, meta, entries):
    lines = ["  %s: %s" % (json.dumps(k, ensure_ascii=False), json.dumps(v, ensure_ascii=False))
             for k, v in [("_meta", meta)] + list(entries.items())]
    path.write_text("{\n" + ",\n".join(lines) + "\n}\n", encoding="utf-8", newline="\n")


def sync(lang):
    """Brings web/lang/<lang>.json in line with the English: new keys added with an empty translation, a changed English
    copied in and the entry marked stale, keys the code dropped removed. Prints what changed."""
    path = pack_path(lang)
    old = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    meta = old.pop("_meta", {"name": lang})
    en = keys(lang_files())
    new = {k: e for k, e in old.items() if k.startswith("cmn.")}  # the common table belongs to no file: kept as it is
    for k, v in en.items():
        e = dict(old.get(k) or {"en": v, lang: ""})
        if e.get("en") != v:
            e["en"], e["stale"] = v, True
            print("stale  %s" % k)
        if k not in old:
            print("new    %s" % k)
        new[k] = e
    for k in old:
        if k not in en and not k.startswith("cmn."):
            print("gone   %s" % k)
    write_pack(path, meta, new)
    print("%s: %d keys, %d without a translation" % (path.name, len(new), sum(not e.get(lang) for e in new.values())))
    bad = pack_format().resolve(new, lang)[1]
    for k, why in bad:
        print("broken %s: %s" % (k, why))


TAGS = re.compile(r"</?\w+[^>]*>")
SLOTS = re.compile(r"\{(\w+)\}")
CATEGORIES = {"pl": {"one", "few", "many", "other"}}


def coverage(lang):
    """Problems of a pack against the English: [(severity, key, what)]; severity 'fail' breaks a page, 'warn' does not,
    'same' lists English that several keys share without referencing the common table (a consistency hint)."""
    path = pack_path(lang)
    if not path.is_file():
        return [("fail", lang, "no pack file")]
    d = json.loads(path.read_text(encoding="utf-8"))
    pk = pack_format()
    res, broken = pk.resolve(d, lang)  # references resolved; a dangling or mismatched one is a failure
    en = keys(lang_files())
    out = [("fail", k, why) for k, why in broken]
    skip = {k for k, _ in broken}
    for k, v in en.items():
        e = d.get(k)
        t = res.get(k)
        if k in skip:
            continue
        if not t:
            out.append(("warn", k, "no translation (English shows)"))
            continue
        if e.get("en") != v or e.get("stale"):
            # the strings are frozen: an English that moved under its translation is a failure, never a hint
            out.append(("fail", k, "stale: the English changed"))
        if isinstance(v, dict) != isinstance(t, dict):
            out.append(("fail", k, "a count entry must stay a count entry (and a text a text)"))
            continue
        # a form may drop a slot only where an English form does (English "1 row" has no {n}): the slots every English form
        # has are required, and none beyond what some English form has
        en_forms = list(v.values()) if isinstance(v, dict) else [v]
        need = set.intersection(*(set(SLOTS.findall(x)) for x in en_forms))
        may = set.union(*(set(SLOTS.findall(x)) for x in en_forms))
        if isinstance(v, dict):
            need.discard("n")  # a count form may say its count in words ("ostatni odczyt")
        ref = v["other"] if isinstance(v, dict) else v
        for form in (t.values() if isinstance(t, dict) else [t]):
            s = set(SLOTS.findall(form))
            if not need <= s <= may:
                out.append(("fail", k, "slots differ: %s" % form))
            # whole tags, attributes included: a link or a class the translation changed is a broken page too
            if sorted(TAGS.findall(form)) != sorted(TAGS.findall(ref)):
                out.append(("fail", k, "tags differ: %s" % form))
        if isinstance(t, dict) and lang in CATEGORIES and set(t) != CATEGORIES[lang]:
            out.append(("fail", k, "count forms %s, %s needs %s" % (sorted(t), lang, sorted(CATEGORIES[lang]))))
    for k in d:
        if k not in en and k != "_meta" and not k.startswith("cmn."):
            out.append(("fail", k, "no such English key"))
    for cid, users in pk.refs(d, lang).items():
        if not users:
            out.append(("warn", "cmn." + cid, "a common entry no key references"))
    same = {}
    for k, v in en.items():
        if isinstance(v, str):
            same.setdefault(v, []).append(k)
    for v, ks in same.items():
        if len(ks) > 1 and not all(pk.is_ref((d.get(k) or {}).get(lang)) for k in ks):
            out.append(("same", ", ".join(ks), v))
    return out


def allow_list():
    """[(file or '*', exact text or compiled regex, line no)] from tools/i18n-allow.txt (each entry with its reason)."""
    out = []
    if not ALLOW.is_file():
        return out
    for i, line in enumerate(ALLOW.read_text(encoding="utf-8").splitlines(), 1):
        s = line.split("  #", 1)[0].rstrip()
        if not s or s.lstrip().startswith("#"):
            continue
        f, _, t = s.partition(": ")
        out.append((f, re.compile(t[1:-1]) if t.startswith("/") and t.endswith("/") and len(t) > 1 else t, i))
    return out


def strict(r):
    return not (re.match(r"^[a-z’'][a-z’' ]*$", r["msg"]) and len(r["msg"].split()) <= 2)


def lint(rows, files):
    """(hits, soft, unused allow entries): texts outside W in these files that no allow entry covers."""
    allow, used = allow_list(), set()
    covered = {f: set(shell_keys(f).values()) for f in files if f.endswith(".html")}
    hits, soft = [], []
    for r in rows:
        if r["file"] not in files or r["cls"] != "text" or r["table"] == "W":
            continue
        if TAGS.sub("", r["msg"]) in covered.get(r["file"], ()):
            continue
        a = next((a for a in allow if a[0] in (r["file"], "*") and (a[1].search(r["msg"]) if hasattr(a[1], "search")
                                                                    else a[1] == r["msg"])), None)
        if a:
            used.add(a[2])
            continue
        (hits if strict(r) else soft).append(r)
    return hits, soft, [a for a in allow if a[2] not in used]


def check(rows, lang="pl"):
    files = lang_files()
    hits, soft, unused = lint(rows, files)
    for r in hits:
        print("text outside W  %s:%d  %s" % (r["file"], r["line"], r["msg"]))
    for r in soft:
        print("soft (check)    %s:%d  %s" % (r["file"], r["line"], r["msg"]))
    for a in unused:
        print("allow-list entry %d matches nothing: %s: %s" % (a[2], a[0], getattr(a[1], "pattern", a[1])))
    cov = coverage(lang)
    for sev, k, what in cov:
        if sev == "fail" or "--verbose" in sys.argv:
            print("pack %s  %s  %s: %s" % (lang, sev, k, what))
        elif sev == "same":
            print("same English, not via cmn (check)  %s: %s" % (k, what))
    warn = sum(s == "warn" for s, _, _ in cov)
    # a translation still marked proposed has not been reviewed yet: a release carries none
    path = pack_path(lang)
    proposed = sorted(k for k, e in json.loads(path.read_text(encoding="utf-8")).items()
                      if isinstance(e, dict) and e.get("proposed")) if path.is_file() else []
    for k in proposed:
        print("pack %s  proposed (not reviewed)  %s" % (lang, k))
    # a soft hit (a short lower-case word outside W) fails as well: a real exception goes to the allow-list with its reason
    fails = len(hits) + len(soft) + len(unused) + len(proposed) + sum(s == "fail" for s, _, _ in cov)
    print("i18n check: %d texts outside W (strict), %d soft, %d stale allow entries; pack %s: %d problems, %d warnings, "
          "%d proposed, %d shared English not via cmn" % (len(hits), len(soft), len(unused), lang,
                                                          fails - len(hits) - len(soft) - len(unused) - len(proposed), warn,
                                                          len(proposed), sum(s == "same" for s, _, _ in cov)))
    return 1 if fails else 0


def main(argv):
    global WEB, LANG_DIR
    sys.stdout.reconfigure(encoding="utf-8")
    if "--web" in argv:  # a fixture tree (test_i18n_check.py)
        WEB = pathlib.Path(argv[argv.index("--web") + 1])
        LANG_DIR = WEB / "lang"
    if "--keys" in argv:
        print(json.dumps(keys(lang_files()), ensure_ascii=False, indent=1))
        return 0
    if "--sync" in argv:
        sync(argv[argv.index("--sync") + 1])
        return 0
    rows = inventory()
    if "--check" in argv:
        return check(rows)
    if "--list" in argv:
        listing(rows, argv[argv.index("--list") + 1])
    elif "--sample" in argv:
        seed = int(argv[argv.index("--seed") + 1]) if "--seed" in argv else 17
        sample(rows, int(argv[argv.index("--sample") + 1]), seed)
    elif "--reuse" in argv:
        reuse(rows)
        print()
        lint_tiers(rows)
    elif "--refactor" in argv:
        refactor_size(rows)
        print()
        refactor_cost()
    elif "--json" in argv:
        pathlib.Path(argv[argv.index("--json") + 1]).write_text(json.dumps(rows, ensure_ascii=False, indent=1),
                                                                encoding="utf-8")
    else:
        summary(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
