#!/usr/bin/env python3
"""leakgate.py -- the publishing leak gate: nothing private leaves the repository. Stdlib only, Windows and Linux.

What it checks
  1. Every tracked file is classified by the path manifest (tools/publish-manifest.txt, format at its top):
     `ship` files are published and scanned, `review` files are scanned but refuse the export until someone decides,
     unclassified files refuse the export. The manifest is read in its published view (the dev-only block dropped),
     and a tracked file it classes `private` is a hit (`private-file`): a public tree holds no private file at all.
     Only the export's source tree runs with --source-is-private: there the dev-only lines apply and private files
     are skipped.
  2. Built-in public rules on every ship and review file (and on --tree folders, --bin strings, --messages):
       path        a path under or a link to the private notes, the local drop and capture folders, secrets files,
                   brief or research folders, or a private notes file by name
       dnum        a decision-number reference (capital D + one or two digits) in prose, a comment or a string
       tag         a block or review tag in prose or a comment (lower/upper b + digits, ER + digits, R + digit-digits);
                   reported, not fatal unless --strict-tags
       owner-note  "owner" and an ISO date on one comment, prose or string line
       polish      Polish diacritics outside the translation data folders
       lan         a private-range IPv4 address other than the documented 192.168.1.x, 192.168.77.x (and 192.0.2.x)
       mac         a MAC-like string whose first octet is not 02 (the documented example prefix)
       coords      a lat/lon value with two or more decimals, or a pair of coordinate-like numbers
       home-path   a user's home directory path (C:\\Users\\<name>, /home/<name>, /Users/<name>)
       binary      a binary file not covered by a `binary` manifest rule
       metadata    a PNG, JPEG, WebP (or a PNG inside an ICO) carrying EXIF, XMP, text chunks or comments
       private-file, unclassified, special-entry (a symlink or submodule): file-level refusals; a secrets file,
                   a `*.secret` file or a path under the root's drop, capture or notes folders is a private-file
                   hit in the published view whatever the manifest's order
  3. A private deny-list (--deny FILE, or the env var LEAKGATE_DENY holding the file base64-encoded), format below,
     plus the developer's secrets (--deny-secrets FILE, below).
  4. gitleaks, when it is on PATH (--no-gitleaks skips it).

Output is `file:line: class` per hit and a count table; the matched text is never printed. Exit 1 on any fatal hit,
any review or unclassified file, or a broken input; 0 otherwise.

Deny-list format: one pattern per line, blank lines and lines starting with `#` ignored.
    @category NAME     the class of the following patterns becomes `deny:NAME` (default `deny:other`)
    re:REGEX           a Python regular expression (case-sensitive unless it says (?i))
    w:TEXT             a whole-word literal, case-insensitive
    lit:TEXT           a literal substring, case-insensitive (use it for a literal that starts with `#`, `@` or a prefix)
    TEXT               the same as lit:TEXT
The deny-list is applied to the text of every scanned file and to every path; a path that matches is reported as
`<denied-name #n>`, never by name. Its hits cannot be allow-listed. In a binary, a pattern that matches 6 or more
characters is applied to every byte (no strings threshold); a shorter one is applied only to the binary's printable
strings, and in an image (.jpg, .jpeg, .png, .webp, .gif with a matching signature) to every byte but its pixel
data (JPEG scan data, PNG IDAT/fdAT, WebP VP8/VP8L/ALPH/ANMF payloads, GIF image data; PNG text and ICC profiles are
also read inflated; an image that does not parse is read in full, but a JPEG cut inside its scan data keeps that
tail as pixels), because a 3-letter case-insensitive literal matches random compressed bytes about once per 2 MB.
A leak drawn into an image's pixels cannot be found by bytes at all.

Secrets (--deny-secrets FILE, repeatable): the string values of a C header such as the firmware's secrets.h become
deny patterns of the class `deny:secrets:NAME`, NAME being the define's name; a value is never printed.
    read      `#define NAME "value"` lines, also `("value")` and a u8 / u / U / L prefix (escapes \\" \\\\ \\n),
              after the preprocessor's own steps: backslash-newline continuations are spliced and comments, before
              or after the literal, count as a space; numeric defines and lines other than #define are skipped; a
              #define whose value holds a quote in any other shape (F("..."), a raw string, two literals) is an
              error, so a #define value is never skipped silently
    skipped   a value equal to the same define's value in secrets.h.example next to the file (the placeholders; a
              built-in copy when that file is absent), `localhost`, values shorter than 8 characters, and the
              defines named by --deny-secrets-skip NAME[,NAME] (a value that is a public word, such as a name the
              code itself uses; names only: an argument that is not a define name is refused unechoed). Each
              skipped define is printed to stderr with its rule, never its value; the label counts the skips.
              The example is trusted as found: a real value pasted into the example is skipped too.
    patterns  the value (case-insensitive substring), its URL-encoded and form-encoded forms (a password inside a
              forwarding URL or a form body) and the base64 of the value's leading whole 3-byte groups,
              case-sensitive (it then also matches where the value starts a longer base64 text); always applied to
              every byte of a binary
Limits: it runs only where the file is (the pre-push hook, a local scan); CI has no secrets and keeps the deny-list
alone. It knows today's values: a rotated key is no longer checked, so a key that was ever committed goes into the
deny-list by hand after the rotation. A hash of a value, a value in an encoding other than those above, or a
value spread over lines is not found. A missing or unreadable file is an error (the hook then refuses the push).

Allow-list (tools/leakgate-allow.txt when present, or --allow FILE): false positives of the line rules only.
    CLASS REGEX @ PATH-GLOB         (the path glob may be left out for `tag` only)
A hit is dropped when REGEX matches a span of the same line that covers the hit and the file matches PATH-GLOB.

Git: --rev and --pre-push read raw blobs (git ls-tree + cat-file), never a checkout or an archive, so line-ending
conversion and export attributes cannot change what is scanned. --pre-push REMOTE (the hook) checks, per pushed ref,
the tip's whole tree, every blob that any pushed commit adds or changes, every pushed commit message and an annotated
tag's text.

Usage
  python -I tools/leakgate.py --manifest tools/publish-manifest.txt [--deny FILE] [--root DIR] [--json]
  python -I tools/leakgate.py --tree site/_build --bin build/fauxcloud.ino.bin [--deny FILE]
  python -I tools/leakgate.py --bin dist/fauxcloud-1.0.0.bin --bin-skip dnum,tag,polish   (a release image: BIN_SKIPPABLE)
  python -I tools/leakgate.py --manifest tools/publish-manifest.txt --rev HEAD --messages origin/main..HEAD
  python -I tools/leakgate.py --install-hook --deny FILE [--deny-secrets FILE] [--force]   (the hook runs --pre-push)
  python -I tools/leakgate.py --deny FILE --deny-secrets firmware/fauxcloud/secrets.h      (the default manifest)
  python -I tools/leakgate.py --manifest tools/publish-manifest.txt --source-is-private   (the export's source only)
"""
import argparse
import base64
import binascii
import json
import os
import posixpath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.parse
import zlib
from collections import Counter

try:
    import re._parser as re_parser
except ImportError:  # Python < 3.11
    import sre_parse as re_parser

MANIFEST_DEFAULT = "tools/publish-manifest.txt"
ALLOW_DEFAULT = "tools/leakgate-allow.txt"
DEV_ONLY_BEGIN = "# BEGIN dev-only"
DEV_ONLY_END = "# END dev-only"

# --- classes -----------------------------------------------------------------------------------------------------
NONFATAL = {"tag"}
ALLOWABLE = {"path", "dnum", "tag", "owner-note", "polish", "lan", "mac", "coords", "home-path"}

# --- file kinds ----------------------------------------------------------------------------------------------------
C_LIKE = {".c", ".h", ".cpp", ".hpp", ".cc", ".inc", ".ino", ".js", ".mjs", ".cjs", ".ts", ".java"}
CSS = {".css", ".scss"}
HASH = {".py", ".ps1", ".psm1", ".sh", ".bash", ".toml", ".cfg", ".csv", ".tab", ".gitignore",
        ".gitattributes", ".properties", ".conf"}
XML = {".svg", ".xml"}
PROSE = {".md", ".markdown", ".rst", ".txt", ".html", ".htm", ".yml", ".yaml", ""}  # YAML: step names are prose
DATA = {".json", ".pem", ".sha256", ".lock", ".der", ".crt"}
BINARY_EXT = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".ico", ".bin", ".elf", ".zip", ".gz", ".pdf", ".ttf",
              ".otf", ".woff", ".woff2", ".7z", ".tar", ".exe", ".dll", ".so", ".a", ".o", ".pyc", ".mp4", ".webm"}
TREE_ASSET_EXT = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".ico", ".woff", ".woff2", ".ttf", ".pdf", ".bin", ".gz"}
POLISH_EXEMPT = ["docs/i18n/**", "firmware/fauxcloud/web/data/**", "firmware/fauxcloud/web/lang/**"]


def kind_of(rel):
    base = rel.rsplit("/", 1)[-1]
    if base in ("Makefile", "Dockerfile", ".gitignore", ".gitattributes", ".editorconfig"):
        return "hash"
    ext = os.path.splitext(base)[1].lower()
    if ext in C_LIKE:
        return "c"
    if ext in CSS:
        return "css"
    if ext in HASH:
        return "hash"
    if ext in XML:
        return "xml"
    if ext in DATA:
        return "data"
    if ext in PROSE or "." not in base:
        return "prose"
    return "prose"


# --- globs ---------------------------------------------------------------------------------------------------------
def glob_to_regex(glob):
    """Gitignore-style glob, anchored at the root: `**` any number of path segments, `*` and `?` inside one segment,
    a trailing `/` matches the folder and everything under it."""
    g = glob.strip().lstrip("/")
    if g.endswith("/"):
        g += "**"
    out, i = [], 0
    while i < len(g):
        c = g[i]
        if g.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif g.startswith("**", i):
            out.append(".*")
            i += 2
        elif c == "*":
            out.append("[^/]*")
            i += 1
        elif c == "?":
            out.append("[^/]")
            i += 1
        elif c == "[":
            j = g.find("]", i)
            if j < 0:
                out.append(re.escape(c))
                i += 1
            else:
                cls = g[i:j + 1]
                if cls.startswith("[!"):
                    cls = "[^" + cls[2:]
                out.append(cls)
                i = j + 1
        else:
            out.append(re.escape(c))
            i += 1
    return re.compile("^" + "".join(out) + "$")


class Manifest:
    def __init__(self):
        self.rules = []      # (cls, glob, regex, source, lineno)
        self.binary = []     # (glob, regex)

    @staticmethod
    def parse(text, source="<manifest>"):
        m = Manifest()
        m.add(text, source)
        return m

    def add(self, text, source):
        for n, raw in enumerate(text.splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            if len(parts) != 2 or parts[0] not in ("ship", "private", "review", "binary"):
                raise ValueError("%s:%d: expected `ship|private|review|binary <glob>`" % (source, n))
            cls, glob = parts[0], parts[1].strip()
            rx = glob_to_regex(glob)
            if cls == "binary":
                self.binary.append((glob, rx))
            else:
                self.rules.append((cls, glob, rx, source, n))

    def classify(self, rel):
        for cls, _glob, rx, _src, _n in self.rules:
            if rx.match(rel):
                return cls
        return "unclassified"

    def binary_ok(self, rel):
        return any(rx.match(rel) for _g, rx in self.binary)


def strip_dev_only(text):
    """The published view of a manifest: the lines between the dev-only markers (inclusive) are dropped."""
    out, skip = [], False
    for line in text.splitlines(keepends=True):
        s = line.strip()
        if s == DEV_ONLY_BEGIN:
            skip = True
            continue
        if s == DEV_ONLY_END:
            skip = False
            continue
        if not skip:
            out.append(line)
    return "".join(out)


# --- comment and prose scope -----------------------------------------------------------------------------------------
def scope_mask(kind, text, strings=False):
    """Returns the text with everything that is not prose or a comment blanked (newlines kept, columns kept);
    strings=True keeps the contents of string literals too."""
    if kind == "prose":
        return text
    if kind == "data":
        return re.sub(r"[^\n]", " ", text)
    out = []
    n = len(text)
    i = 0
    if kind in ("c", "css"):
        state = None  # None, "line", "block", or a quote char
        while i < n:
            c = text[i]
            if c == "\n":
                out.append(c)
                if state == "line" or state in ("'", '"'):
                    state = None
                i += 1
                continue
            if state is None:
                if kind == "c" and text.startswith("//", i):
                    state = "line"
                    out.append("  ")
                    i += 2
                    continue
                if text.startswith("/*", i):
                    state = "block"
                    out.append("  ")
                    i += 2
                    continue
                if c in ("'", '"') or (kind == "c" and c == "`"):
                    state = c
                out.append(" ")
                i += 1
            elif state in ("line", "block"):
                if state == "block" and text.startswith("*/", i):
                    state = None
                    out.append("  ")
                    i += 2
                    continue
                out.append(c)
                i += 1
            else:  # in a string
                if c == "\\" and i + 1 < n and text[i + 1] != "\n":
                    out.append("  ")
                    i += 2
                    continue
                if c == state:
                    state = None
                    out.append(" ")
                else:
                    out.append(c if strings else " ")
                i += 1
        return "".join(out)
    if kind == "xml":
        state = None
        while i < n:
            c = text[i]
            if state is None and text.startswith("<!--", i):
                state = "c"
                out.append("    ")
                i += 4
                continue
            if state == "c" and text.startswith("-->", i):
                state = None
                out.append("   ")
                i += 3
                continue
            out.append(c if (state == "c" or c == "\n") else " ")
            i += 1
        return "".join(out)
    # hash languages: `#` at a line start or after whitespace; Python triple-quoted strings and PowerShell <# #> count
    # as comments; single-line strings are skipped
    state = None  # None, "line", "ps", '"""', "'''", or a quote char
    while i < n:
        c = text[i]
        if state in ('"""', "'''"):
            if text.startswith(state, i):
                state = None
                out.append("   ")
                i += 3
                continue
            out.append(c)
            i += 1
            continue
        if state == "ps":
            if text.startswith("#>", i):
                state = None
                out.append("  ")
                i += 2
                continue
            out.append(c)
            i += 1
            continue
        if c == "\n":
            out.append(c)
            if state in ("line", "'", '"'):
                state = None
            i += 1
            continue
        if state == "line":
            out.append(c)
            i += 1
            continue
        if state in ("'", '"'):
            if c == "\\" and i + 1 < n and text[i + 1] != "\n":
                out.append("  ")
                i += 2
                continue
            if c == state:
                state = None
                out.append(" ")
            else:
                out.append(c if strings else " ")
            i += 1
            continue
        # state None
        if text.startswith('"""', i) or text.startswith("'''", i):
            state = text[i:i + 3]
            out.append("   ")
            i += 3
            continue
        if text.startswith("<#", i):
            state = "ps"
            out.append("  ")
            i += 2
            continue
        if c == "#" and (i == 0 or text[i - 1] in " \t\n"):
            state = "line"
            out.append(" ")
            i += 1
            continue
        if c in ("'", '"'):
            state = c
        out.append(" ")
        i += 1
    return "".join(out)


# --- built-in rules --------------------------------------------------------------------------------------------------
# The patterns are written so that this file does not match them itself (character classes split the words).
PATH_RX = [
    re.compile(r"docs[/\\]interna[l]\b"),
    re.compile(r"(?<![\w.-])_in[b]ox\b"),
    re.compile(r"(?:^|(?<=[\s\"'`(\[=,:<>|]))(?:\.{1,2}[/\\])?captur[e]s[/\\]"),
    re.compile(r"(?<![\w/.-])(?:brief[s]|researc[h])[/\\][\w.-]"),
    re.compile(r"(?<![\w/.-])(?:architectur[e]|backlo[g]|concep[t]|qa-lo[g]|engineering-rule[s]|positionin[g]"
               r"|publishing-pane[l]|contribution-mode[l])\.md\b"),
    re.compile(r"(?<![\w.-])secret[s][/\\]"),
]
SECRETS_FILE_RX = re.compile(r"(?<![\w.-])secret[s]\.[\w.-]+")
SECRETS_PUBLIC = {"secrets.h", "secrets.h.example"}
# folders that are private wherever a manifest would put them (the words are split so this file passes its own rules)
PRIVATE_DIRS = ("_in" + "box/", "captur" + "es/", "docs/" + "intern" + "al/")


def private_path(rel):
    """True for a path that is never published, whatever the manifest says: a secrets file (the documented example
    excepted), a `*.secret` file, or anything under the local drop, capture or notes folders at the root."""
    base = rel.rsplit("/", 1)[-1]
    if base.startswith("secrets.") and base != "secrets.h.example":
        return True
    return base.endswith(".secret") or rel.startswith(PRIVATE_DIRS)
DNUM_RX = re.compile(r"\bD[0-9]{1,2}\b")
TAG_RX = re.compile(r"\b(?:[bB][0-9]{1,2}[a-z]?(?:\.[0-9]+)*|E[R][0-9]{1,2}|R[0-9]{1,2}-[0-9]{1,2})\b")
OWNER_RX = re.compile(r"\bowne[r](?:'s)?\b", re.I)
DATE_RX = re.compile(r"\b20[0-9]{2}-[01][0-9]-[0-3][0-9]\b")
POLISH_RX = re.compile("[\u0105\u0107\u0119\u0142\u0144\u00f3\u015b\u017a\u017c"
                       "\u0104\u0106\u0118\u0141\u0143\u00d3\u015a\u0179\u017b]")
LAN_RX = re.compile(
    r"(?<![\d.])(?:"
    r"192\.168\.(?P<c>[0-9]{1,3})(?:\.(?:[0-9]{1,3}|[xX*]))?"
    r"|10\.[0-9]{1,3}\.[0-9]{1,3}\.(?:[0-9]{1,3}|[xX*])"
    r"|172\.(?:1[6-9]|2[0-9]|3[01])\.[0-9]{1,3}\.(?:[0-9]{1,3}|[xX*])"
    r")(?![\d]|\.[0-9])")
LAN_DOCUMENTED = {"1", "77"}
MAC_RX = re.compile(r"(?<![0-9A-Fa-f:-])([0-9A-Fa-f]{2})([:-])[0-9A-Fa-f]{2}(?:\2[0-9A-Fa-f]{2}){4}(?![0-9A-Fa-f]|[:-][0-9A-Fa-f])")
COORD_KEY_RX = re.compile(r"\b(?:lat|latitude|lon|lng|longitude)\b[\"']?\s*[:=]\s*[\"']?(-?[0-9]{1,3}\.[0-9]{2,})(?![0-9])",
                          re.I)
COORD_PAIR_RX = re.compile(r"(?<![\w.])(-?[0-9]{1,2}\.[0-9]{4,})\s*,\s*(-?[0-9]{1,3}\.[0-9]{4,})(?![\w.])")
HOME_RX = re.compile(r"(?:\b[A-Za-z]:[\\/]Users[\\/](?!Public\b|Default\b|All Users\b)[A-Za-z0-9._-]+"
                     r"|(?<![\w.-])/home/(?!runner\b|user\b|ubuntu\b)[a-z_][a-z0-9_-]*/"
                     r"|(?<![\w.-])/Users/(?!Shared\b)[A-Za-z][A-Za-z0-9._-]*/)")


class Hit:
    __slots__ = ("file", "line", "cls", "span")

    def __init__(self, file, line, cls, span=None):
        self.file, self.line, self.cls, self.span = file, line, cls, span

    def key(self):
        return (self.file, self.line, self.cls)

    def as_dict(self):
        return {"file": self.file, "line": self.line, "class": self.cls}


def _lines(text):
    return text.split("\n")


def line_rules(rel, text, kind, polish_exempt=False):
    """The built-in text rules. Returns [(lineno, cls, (start, end), line_text)]."""
    res = []
    lines = _lines(text)
    scoped = _lines(scope_mask(kind, text))
    texty = _lines(scope_mask(kind, text, strings=True))
    for no, line in enumerate(lines, 1):
        if not line:
            continue
        for rx in PATH_RX:
            for m in rx.finditer(line):
                res.append((no, "path", m.span(), line))
        for m in SECRETS_FILE_RX.finditer(line):
            name = m.group(0).rstrip(".")
            if name not in SECRETS_PUBLIC:
                res.append((no, "path", m.span(), line))
        if not polish_exempt:
            for m in POLISH_RX.finditer(line):
                res.append((no, "polish", m.span(), line))
        for m in LAN_RX.finditer(line):
            if m.group("c") is not None and m.group("c") in LAN_DOCUMENTED:
                continue
            res.append((no, "lan", m.span(), line))
        for m in MAC_RX.finditer(line):
            v = m.group(0).lower()
            hexd = re.sub(r"[:-]", "", v)
            if m.group(1) == "02" or set(hexd) in ({"0"}, {"f"}) or not re.search(r"[0-9]", hexd):
                continue
            if m.group(2) == "-" and not re.search(r"[a-f]", hexd):  # digit runs such as SVG path data
                continue
            res.append((no, "mac", m.span(), line))
        for m in COORD_KEY_RX.finditer(line):
            res.append((no, "coords", m.span(), line))
        for m in COORD_PAIR_RX.finditer(line):
            try:
                a, b = float(m.group(1)), float(m.group(2))
            except ValueError:
                continue
            if 0.5 <= abs(a) <= 90 and 0.5 <= abs(b) <= 180:
                res.append((no, "coords", m.span(), line))
        for m in HOME_RX.finditer(line):
            res.append((no, "home-path", m.span(), line))
        sc = scoped[no - 1] if no - 1 < len(scoped) else ""
        st = texty[no - 1] if no - 1 < len(texty) else ""
        # decision numbers and owner notes: comments, prose and string literals (assert messages, test names);
        # block tags: comments and prose only (version strings in literals would drown them)
        if st.strip():
            for m in DNUM_RX.finditer(st):
                res.append((no, "dnum", m.span(), line))
            m = OWNER_RX.search(st)
            if m and DATE_RX.search(st):
                res.append((no, "owner-note", m.span(), line))
        if sc.strip():
            for m in TAG_RX.finditer(sc):
                res.append((no, "tag", m.span(), line))
    return res


# --- deny-list -------------------------------------------------------------------------------------------------------
# a pattern that can match fewer characters reads random compressed bytes as a hit, so it skips raw binary bytes
SHORT_PATTERN = 6


def min_width(rx):
    """The fewest characters the compiled pattern can match (lookarounds and anchors count zero)."""
    try:
        return re_parser.parse(rx.pattern, rx.flags).getwidth()[0]
    except Exception:  # an internal parser change must not make a pattern skip binaries
        return SHORT_PATTERN


class DenyList:
    def __init__(self):
        self.patterns = []  # (category, compiled, text_only) from the deny-list file
        self.secrets = []   # (define name, compiled) from --deny-secrets
        self.secret_files = 0
        self.secret_values = 0
        self.secret_skipped = 0
        self._seen = set()
        self.source = "absent"

    def text_only(self):
        return sum(1 for _c, _rx, t in self.patterns if t)

    def add_secrets(self, pairs):
        """pairs: (define name, value) already filtered; each value adds itself and its encoded twins."""
        for name, value in pairs:
            if value in self._seen:
                continue
            self._seen.add(value)
            forms = [re.compile(re.escape(value), re.I)]
            seen = {value}
            for enc in (urllib.parse.quote(value, safe=""), urllib.parse.quote_plus(value, safe="")):
                if enc not in seen:
                    seen.add(enc)
                    forms.append(re.compile(re.escape(enc), re.I))
            raw = value.encode("utf-8")
            b64 = base64.b64encode(raw[:len(raw) - len(raw) % 3]).decode("ascii")
            if len(b64) >= SECRETS_FLOOR:
                forms.append(re.compile(re.escape(b64)))
            self.secrets += [(name, rx) for rx in forms]
            self.secret_values += 1
        self.secret_files += 1

    @staticmethod
    def parse(text, source="file"):
        d = DenyList()
        d.source = source
        cat = "other"
        for n, raw in enumerate(text.splitlines(), 1):
            line = raw.rstrip("\r\n").strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("@category"):
                cat = line[len("@category"):].strip() or "other"
                if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", cat):
                    raise ValueError("deny-list line %d: a category is lower-case letters, digits and dashes" % n)
                continue
            try:
                if line.startswith("re:"):
                    rx = re.compile(line[3:])
                elif line.startswith("w:"):
                    lit = line[2:]
                    rx = re.compile(r"(?<!\w)" + re.escape(lit) + r"(?!\w)", re.I)
                elif line.startswith("lit:"):
                    rx = re.compile(re.escape(line[4:]), re.I)
                else:
                    rx = re.compile(re.escape(line), re.I)
            except re.error:
                # never echo the pattern: it is private
                raise ValueError("deny-list line %d: the regular expression does not compile" % n)
            if rx.pattern in ("", r"(?<!\w)(?!\w)"):
                raise ValueError("deny-list line %d: empty pattern" % n)
            d.patterns.append((cat, rx, min_width(rx) < SHORT_PATTERN))
        return d

    def classed(self):
        """Every pattern as (class, compiled, text_only); a secret is never text-only."""
        return [("deny:" + c, rx, t) for c, rx, t in self.patterns] + \
            [("deny:secrets:" + n, rx, False) for n, rx in self.secrets]

    def scan_blob(self, data):
        """The patterns that are not text-only over a whole binary, without a strings threshold: the bytes read as
        latin-1 and as UTF-8. Returns the classes that matched (one hit per class)."""
        return self.scan_views((data.decode("latin-1"), data.decode("utf-8", "replace")), text_only=False)

    def scan_views(self, views, text_only):
        """The classes whose patterns (text-only ones, or the others) match any of the given texts."""
        cats = []
        for cls, rx, t in self.classed():
            if t == text_only and cls not in cats and any(rx.search(v) for v in views):
                cats.append(cls)
        return cats

    def scan(self, text):
        res = []
        pats = self.classed()
        if not pats:
            return res
        for no, line in enumerate(_lines(text), 1):
            if not line:
                continue
            seen = set()
            for cls, rx, _t in pats:
                if cls not in seen and rx.search(line):
                    seen.add(cls)
                    res.append((no, cls))
        return res


# --- secrets ---------------------------------------------------------------------------------------------------------
SECRETS_FLOOR = 8
SECRETS_EXAMPLE = "secrets.h.example"
# the example's placeholders, used when the example file is not next to the secrets file
SECRETS_PLACEHOLDERS = {"STA_SSID": "your-home-wifi", "STA_PASS": "your-wifi-password", "AP_PASS": "change-me-8+chars",
                        "ADMIN_PASS": "change-me-8+chars", "MQTT_HOST": "192.168.1.10"}
SECRETS_SKIP = ("localhost",)
SKIP_KEY_RX = re.compile(r"[A-Za-z_]\w*")
DEFINE_RX = re.compile(r'\s*#\s*define\s+([A-Za-z_]\w*)(.*)')
# a plain literal, optionally in parentheses or with an encoding prefix (comments are already blanked)
DEFINE_VALUE_RX = re.compile(r'\s+(\()?\s*(?:u8|u|U|L)?"((?:[^"\\\n]|\\.)*)"\s*(?(1)\))\s*')


def c_unescape(s):
    return re.sub(r"\\(.)", lambda m: {"n": "\n", '"': '"', "\\": "\\"}.get(m.group(1), m.group(0)), s)


def c_logical_lines(text):
    """(first line number, text) of each line after the preprocessor's backslash-newline splice."""
    buf, start = [], None
    for n, line in enumerate(text.splitlines(), 1):
        start = start or n
        if line.rstrip().endswith("\\"):
            buf.append(line.rstrip()[:-1])
            continue
        yield start, "".join(buf) + line
        buf, start = [], None
    if buf:
        yield start, "".join(buf)


def c_code(s):
    """One line with each comment replaced by a space (an unclosed block comment ends the line); string and
    character literals kept as written."""
    out, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if c in "\"'":
            j = i + 1
            while j < n and s[j] != c:
                j += 2 if s[j] == "\\" else 1
            out.append(s[i:j + 1])
            i = j + 1
        elif s.startswith("//", i):
            break
        elif s.startswith("/*", i):
            out.append(" ")
            j = s.find("*/", i + 2)
            if j < 0:
                break
            i = j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def parse_defines(text, strict=True):
    """[(name, value)] of the `#define NAME "value"` lines (continuations spliced, comments blanked). strict: a
    #define that holds a quote in any other shape raises (its line number only, never its text)."""
    out = []
    for n, line in c_logical_lines(text):
        d = DEFINE_RX.fullmatch(c_code(line))
        if not d:
            continue
        m = DEFINE_VALUE_RX.fullmatch(d.group(2))
        if m:
            out.append((d.group(1), c_unescape(m.group(2))))
        elif strict and '"' in d.group(2):
            raise ValueError("secrets file line %d: a string #define this tool cannot read "
                             "(one plain literal)" % n)
    return out


def skip_keys(args):
    """The define names of --deny-secrets-skip (repeatable, comma-separated). An argument that is not a name is
    refused without echoing it: a value typed there by mistake must not reach the output."""
    keys = []
    for a in args:
        for k in a.split(","):
            k = k.strip()
            if not k:
                continue
            if not SKIP_KEY_RX.fullmatch(k):
                raise ValueError("--deny-secrets-skip takes define names (letters, digits, underscore), "
                                 "comma-separated")
            keys.append(k)
    return keys


def read_secrets(path, skip=()):
    """The values of a secrets header that become deny patterns: (define name, value), floor and skip list applied."""
    return read_secrets_report(path, skip)[0]


def read_secrets_report(path, skip=()):
    """(kept [(name, value)], skipped [(name, reason)], how many names of `skip` the file does not define). A
    reason names the rule, never the value."""
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError:
        raise ValueError("secrets file not found or unreadable: %s" % path) from None
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        raise ValueError("secrets file is not UTF-8: %s" % path) from None
    example_path = os.path.join(os.path.dirname(os.path.abspath(path)), SECRETS_EXAMPLE)
    try:
        with open(example_path, "rb") as f:
            example = dict(parse_defines(f.read().decode("utf-8-sig", "replace"), strict=False))
    except OSError:
        example = dict(SECRETS_PLACEHOLDERS)
    defines = parse_defines(text)
    named = set(skip)
    kept, skipped = [], []
    for n, v in defines:
        if not v:
            continue
        if n in named:
            skipped.append((n, "named by --deny-secrets-skip"))
        elif example.get(n) == v:
            skipped.append((n, "equals the example's value, not a secret"))
        elif v in SECRETS_SKIP:
            skipped.append((n, "a generic host name, not a secret"))
        elif len(v) < SECRETS_FLOOR:
            skipped.append((n, "shorter than %d characters" % SECRETS_FLOOR))
        else:
            kept.append((n, v))
    return kept, skipped, len(named - {n for n, _v in defines})


def add_secrets_file(deny, path, skip=(), log=None):
    """Loads the values and prints one line per skipped define (its name and the rule), so a skip is never silent."""
    kept, skipped, unknown = read_secrets_report(path, skip)
    log = log or sys.stderr
    for n, why in skipped:
        print("leakgate: secrets: %s: %s, skipped" % (n, why), file=log)
    if unknown:
        print("leakgate: secrets: %d --deny-secrets-skip name(s) not defined in the secrets file" % unknown, file=log)
    deny.add_secrets(kept)
    deny.secret_skipped += len(skipped)


def load_deny(path, env):
    if path:
        with open(path, "rb") as f:
            return DenyList.parse(f.read().decode("utf-8-sig"), "file")
    raw = env.get("LEAKGATE_DENY", "").strip()
    if raw:
        try:
            data = base64.b64decode(raw, validate=False)
        except (binascii.Error, ValueError):
            raise ValueError("LEAKGATE_DENY is not base64")
        return DenyList.parse(data.decode("utf-8-sig"), "env")
    return DenyList()


# --- allow-list ------------------------------------------------------------------------------------------------------
class AllowList:
    def __init__(self):
        self.rules = []  # (cls, regex, pathrx or None)

    @staticmethod
    def parse(text, source="<allow>"):
        a = AllowList()
        for n, raw in enumerate(text.splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            path = None
            if " @ " in line:
                line, path = line.rsplit(" @ ", 1)
            parts = line.split(None, 1)
            if len(parts) != 2:
                raise ValueError("%s:%d: expected `CLASS REGEX [@ PATH-GLOB]`" % (source, n))
            cls, rx = parts
            if cls not in ALLOWABLE:
                raise ValueError("%s:%d: class %s cannot be allow-listed" % (source, n, cls))
            if path is None and cls != "tag":
                raise ValueError("%s:%d: an allow line for %s needs `@ PATH-GLOB`" % (source, n, cls))
            a.rules.append((cls, re.compile(rx), glob_to_regex(path.strip()) if path else None))
        return a

    def allows(self, rel, cls, span, line):
        for c, rx, prx in self.rules:
            if c != cls or (prx is not None and not prx.match(rel)):
                continue
            for m in rx.finditer(line):
                if m.start() <= span[0] and m.end() >= span[1]:
                    return True
        return False


# --- images ----------------------------------------------------------------------------------------------------------
IMAGE_EXT = {".jpg", ".jpeg", ".png", ".webp", ".gif"}
INFLATE_MAX = 1 << 20


def inflate(data):
    """A zlib stream, capped so a crafted chunk cannot exhaust memory; the raw bytes when it does not inflate."""
    try:
        return zlib.decompressobj().decompress(data, INFLATE_MAX)
    except zlib.error:
        return data


def png_text(typ, body):
    """A PNG text chunk's keyword and text, inflated when compressed."""
    key, _, rest = body.partition(b"\x00")
    if typ == b"zTXt" and rest:
        return key + b"\n" + inflate(rest[1:])
    if typ == b"iTXt" and len(rest) >= 2:
        flag, tail = rest[0], rest[2:]
        _lang, _, tail = tail.partition(b"\x00")
        tkey, _, text = tail.partition(b"\x00")
        return key + b"\n" + tkey + b"\n" + (inflate(text) if flag else text)
    return body


def gif_blocks(data):
    """A GIF's comment and XMP application extensions; image data is skipped."""
    out = []
    n = len(data)
    if n < 13:
        return out
    i = 13
    if data[10] & 0x80:
        i += 3 << ((data[10] & 7) + 1)

    def subblocks(j):
        parts = []
        while j < n and data[j]:
            parts.append(data[j + 1:j + 1 + data[j]])
            j += 1 + data[j]
        return b"".join(parts), j + 1

    while i < n:
        b = data[i]
        if b == 0x21 and i + 1 < n:
            label = data[i + 1]
            body, nxt = subblocks(i + 2)
            if label == 0xFE or (label == 0xFF and body.startswith(b"XMP DataXMP")):
                out.append(body)
            i = nxt
        elif b == 0x2C and i + 10 <= n:
            j = i + 10
            if data[i + 9] & 0x80:
                j += 3 << ((data[i + 9] & 7) + 1)
            _pixels, i = subblocks(j + 1)  # the LZW code size byte, then the image data
        else:
            break
    return out


def image_meta_blocks(data):
    """The metadata blocks of a PNG, JPEG, WebP, GIF (or of the PNGs inside an ICO): EXIF, XMP, IPTC, text chunks,
    comments. An empty list means no metadata; the pixel data is never returned."""
    out = []
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        i = 8
        while i + 8 <= len(data):
            ln = int.from_bytes(data[i:i + 4], "big")
            typ = data[i + 4:i + 8]
            if typ in (b"eXIf", b"tEXt", b"zTXt", b"iTXt"):
                out.append(png_text(typ, data[i + 8:i + 8 + ln]))
            if typ == b"IEND":
                break
            i += 12 + ln
        return out
    if data.startswith(b"\xff\xd8"):
        i = 2
        while i + 4 <= len(data):
            if data[i] != 0xFF:
                break
            marker = data[i + 1]
            if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
                i += 2
                continue
            if marker == 0xDA:  # start of scan: no more metadata segments to read
                break
            ln = int.from_bytes(data[i + 2:i + 4], "big")
            body = data[i + 4:i + 2 + ln]
            if marker == 0xE1 and (body.startswith(b"Exif\x00") or body.startswith(b"http://ns.adobe.com/xap/")):
                out.append(body)
            if marker in (0xED, 0xFE):  # Photoshop IRB (IPTC), comment
                out.append(body)
            i += 2 + ln
        return out
    if data[:4] == b"\x00\x00\x01\x00" and len(data) >= 6:  # ICO: PNG entries are parsed as PNGs
        count = int.from_bytes(data[4:6], "little")
        for k in range(count):
            e = 6 + 16 * k
            if e + 16 > len(data):
                break
            size = int.from_bytes(data[e + 8:e + 12], "little")
            off = int.from_bytes(data[e + 12:e + 16], "little")
            if data[off:off + 8] == b"\x89PNG\r\n\x1a\n":
                out += image_meta_blocks(data[off:off + size])
        return out
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        i = 12
        while i + 8 <= len(data):
            fourcc = data[i:i + 4]
            ln = int.from_bytes(data[i + 4:i + 8], "little")
            if fourcc in (b"EXIF", b"XMP "):
                out.append(data[i + 8:i + 8 + ln])
            i += 8 + ln + (ln & 1)
        return out
    if data[:6] in (b"GIF87a", b"GIF89a"):
        return gif_blocks(data)
    return out


def image_metadata(data):
    """True when a PNG, JPEG, WebP, GIF (or a PNG inside an ICO) carries EXIF, XMP, text chunks or comments."""
    return bool(image_meta_blocks(data))


def is_image(rel, data):
    """An image file by extension and by signature: its pixel data is not read by text-only patterns."""
    if os.path.splitext(rel)[1].lower() not in IMAGE_EXT:
        return False
    return data.startswith((b"\x89PNG\r\n\x1a\n", b"\xff\xd8", b"GIF87a", b"GIF89a")) or \
        (data[:4] == b"RIFF" and data[8:12] == b"WEBP")


class Malformed(Exception):
    pass


# the next JPEG marker inside entropy-coded data: 0xFF not followed by a stuffed zero or a restart marker
JPEG_NEXT_MARKER_RX = re.compile(b"\\xff(?![\\x00\\xd0-\\xd7])")


def _jpeg_parts(data, out, depth=0):
    n = len(data)
    if not data.startswith(b"\xff\xd8"):
        raise Malformed
    out.append(data[:2])
    i = 2
    while i < n:
        if data[i] != 0xFF:
            raise Malformed
        while i + 1 < n and data[i + 1] == 0xFF:  # fill bytes before a marker
            i += 1
        if i + 1 >= n:
            out.append(data[i:])
            return
        marker = data[i + 1]
        if marker == 0xD9:  # end of image: what follows is a trailer, read in full (another JPEG the same way)
            out.append(data[i:i + 2])
            rest = data[i + 2:]
            if rest.startswith(b"\xff\xd8") and depth < 8:
                _jpeg_parts(rest, out, depth + 1)
            elif rest:
                out.append(rest)
            return
        if marker in (0x01, 0xD8) or 0xD0 <= marker <= 0xD7:
            out.append(data[i:i + 2])
            i += 2
            continue
        if i + 4 > n:
            raise Malformed
        ln = int.from_bytes(data[i + 2:i + 4], "big")
        if ln < 2 or i + 2 + ln > n:
            raise Malformed
        out.append(data[i:i + 2 + ln])
        i += 2 + ln
        if marker == 0xDA:  # start of scan: skip the entropy-coded data
            m = JPEG_NEXT_MARKER_RX.search(data, i)
            if not m:
                return
            i = m.start()


def _png_parts(data, out):
    n = len(data)
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise Malformed
    out.append(data[:8])
    i = 8
    while i < n:
        if i + 12 > n:
            raise Malformed
        ln = int.from_bytes(data[i:i + 4], "big")
        typ = data[i + 4:i + 8]
        end = i + 12 + ln
        if end > n:
            raise Malformed
        if typ in (b"IDAT", b"fdAT"):
            out.append(data[i:i + 8])
        else:
            body = data[i + 8:i + 8 + ln]
            out.append(data[i:end])
            if typ in (b"zTXt", b"iTXt"):
                out.append(png_text(typ, body))
            elif typ == b"iCCP":  # profile name, then a compressed profile whose description is text
                out.append(inflate(body.partition(b"\x00")[2][1:]))
        i = end
        if typ == b"IEND":
            if i < n:
                out.append(data[i:])
            return


def _webp_parts(data, out):
    n = len(data)
    if not (data[:4] == b"RIFF" and data[8:12] == b"WEBP"):
        raise Malformed
    riff_end = 8 + int.from_bytes(data[4:8], "little")
    if riff_end > n:
        raise Malformed
    out.append(data[:12])
    i = 12
    while i + 8 <= riff_end:
        fourcc = data[i:i + 4]
        ln = int.from_bytes(data[i + 4:i + 8], "little")
        body_end = i + 8 + ln
        if body_end > riff_end:
            raise Malformed
        out.append(data[i:i + 8] if fourcc in (b"VP8 ", b"VP8L", b"ALPH", b"ANMF") else data[i:body_end])
        i = body_end + (ln & 1)
    if i < riff_end:
        raise Malformed
    if riff_end < n:
        out.append(data[riff_end:])


def _gif_parts(data, out):
    n = len(data)
    if n < 13 or data[:6] not in (b"GIF87a", b"GIF89a"):
        raise Malformed

    def after_subblocks(j):
        while True:
            if j >= n:
                raise Malformed
            if data[j] == 0:
                return j + 1
            j += 1 + data[j]

    i = 13
    if data[10] & 0x80:
        i += 3 << ((data[10] & 7) + 1)
    if i > n:
        raise Malformed
    out.append(data[:i])
    while i < n:
        b = data[i]
        if b == 0x21 and i + 2 <= n:  # an extension (comment, plain text, application, control), read in full
            end = after_subblocks(i + 2)
            out.append(data[i:end])
            i = end
        elif b == 0x2C and i + 10 <= n:  # image descriptor, local colour table, code size; the LZW data skipped
            j = i + 10
            if data[i + 9] & 0x80:
                j += 3 << ((data[i + 9] & 7) + 1)
            if j >= n:
                raise Malformed
            out.append(data[i:j + 1])
            i = after_subblocks(j + 1)
        elif b == 0x3B:  # trailer and anything after it
            out.append(data[i:])
            return
        else:
            raise Malformed


def image_scan_bytes(data):
    """Every byte of an image except its pixel data (JPEG entropy-coded scans, PNG IDAT / fdAT, WebP VP8 / VP8L /
    ALPH / ANMF payloads, GIF LZW data), plus inflated PNG text and ICC profiles; the pieces are joined by newlines so
    a match never spans a skipped gap. A file that does not parse is returned whole."""
    out = []
    try:
        if data.startswith(b"\x89PNG\r\n\x1a\n"):
            _png_parts(data, out)
        elif data.startswith(b"\xff\xd8"):
            _jpeg_parts(data, out)
        elif data[:4] == b"RIFF":
            _webp_parts(data, out)
        else:
            _gif_parts(data, out)
    except Malformed:
        return data
    return b"\n".join(out)


def is_binary(rel, data):
    ext = os.path.splitext(rel)[1].lower()
    return ext in BINARY_EXT or b"\x00" in data[:8192]


# the line-rule classes --bin-skip may leave out of a binary's strings scan: a firmware image carries compressed web assets
# whose bytes read as random strings (a capital D and a digit, a b-digit pair, a two-byte sequence that decodes to a
# Polish letter) and SDK names such as a chip package code; the source tree scan covers our own string literals for
# these three classes. An ELF's debug info also carries the build folders of the prebuilt SDK archives (other
# developers' home paths), so an ELF may skip home-path too; an image built in CI holds only the runner's own paths.
# Every other class (paths, LAN addresses, MACs, coordinates, owner notes) and the deny-list stay.
BIN_SKIPPABLE = {"dnum", "tag", "polish", "home-path"}

BIN_STRINGS_RX = re.compile(rb"(?:[\x20-\x7e\t]|[\xc2-\xdf][\x80-\xbf]){4,}")


def bin_strings(data):
    return "\n".join(m.group(0).decode("utf-8", "replace") for m in BIN_STRINGS_RX.finditer(data))


# --- gitleaks --------------------------------------------------------------------------------------------------------
def parse_gitleaks_report(report, base, keep=None):
    """gitleaks JSON report -> [(rel, line, cls)]; keep = the set of scanned paths (others dropped)."""
    out = []
    for f in report or []:
        p = str(f.get("File", "")).replace("\\", "/")
        b = base.replace("\\", "/").rstrip("/") + "/"
        if p.startswith(b):
            p = p[len(b):]
        while p.startswith("./"):
            p = p[2:]
        if keep is not None and p not in keep:
            continue
        rule = re.sub(r"[^A-Za-z0-9_.-]", "_", str(f.get("RuleID", "secret")))
        out.append((p, int(f.get("StartLine", 0) or 0), "gitleaks:" + rule))
    return out


def run_gitleaks(src, keep=None):
    exe = shutil.which("gitleaks")
    if not exe:
        return None, "not found"
    fd, rep = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    try:
        cmd = [exe, "detect", "--no-git", "--source", src, "--report-format", "json", "--report-path", rep,
               "--redact", "--no-banner", "--exit-code", "0", "--log-level", "error"]
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if r.returncode != 0:
            return None, "failed (exit %d)" % r.returncode
        with open(rep, "rb") as f:
            raw = f.read().decode("utf-8") or "[]"
        return parse_gitleaks_report(json.loads(raw), src, keep), "ran"
    finally:
        os.unlink(rep)


# --- the scan --------------------------------------------------------------------------------------------------------
NUL = chr(0)


class Result:
    def __init__(self):
        self.hits = []
        self.files = Counter()          # ship / private / review / unclassified / tree / bin / message / history
        self.review = []
        self.unclassified = []
        self.notes = []
        self.deny_source = "absent"
        self.gitleaks = "skipped"
        self.redacted = {}              # a label whose path matched the deny-list -> its placeholder

    def fatal(self, strict_tags=False):
        return [h for h in self.hits if strict_tags or h.cls not in NONFATAL]

    def ok(self, strict_tags=False):
        return not self.fatal(strict_tags) and not self.review and not self.unclassified

    def counts(self):
        return Counter(h.cls for h in self.hits)


def rmtree(path):
    """shutil.rmtree that also removes read-only files (git's objects on Windows)."""
    def clear(func, p, _exc):
        try:
            os.chmod(p, stat.S_IWRITE)
            func(p)
        except OSError:
            pass
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=clear)
    else:
        shutil.rmtree(path, onerror=clear)


def git_out(root, args, data=None):
    r = subprocess.run(["git", "-C", root] + list(args), input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise ValueError("git %s failed" % args[0])
    return r.stdout


def git_ls_files(root):
    r = subprocess.run(["git", "-C", root, "ls-files", "-z"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        return None
    return [p for p in r.stdout.decode("utf-8").split(NUL) if p]


def walk_files(root):
    out = []
    for d, dirs, files in os.walk(root):
        dirs[:] = sorted(x for x in dirs if x != ".git")
        for f in sorted(files):
            out.append(os.path.relpath(os.path.join(d, f), root).replace("\\", "/"))
    return out


def cat_blobs(root, shas):
    """Raw blob bytes by object id (git cat-file --batch: no line-ending conversion, no attributes)."""
    shas = list(shas)
    if not shas:
        return {}
    out = git_out(root, ["cat-file", "--batch"], ("\n".join(shas) + "\n").encode("ascii"))
    res, i = {}, 0
    while i < len(out):
        j = out.index(b"\n", i)
        head = out[i:j].decode("ascii", "replace").split()
        if len(head) < 3 or head[1] == "missing":
            raise ValueError("git cat-file: an object is missing")
        size = int(head[2])
        res[head[0]] = out[j + 1:j + 1 + size]
        i = j + 1 + size + 1
    return res


def read_tree(root, rev):
    """The committed bytes of every file of REV, read as raw blobs (never a checkout or an archive, so neither
    core.autocrlf nor export-ignore can change what is scanned or exported).
    Returns ({path: bytes}, [paths that are not plain files: symlinks, submodules], {path: blob id})."""
    out = git_out(root, ["ls-tree", "-r", "-z", "--full-tree", rev])
    entries, special = [], []
    for rec in out.decode("utf-8").split(NUL):
        if not rec:
            continue
        meta, path = rec.split("\t", 1)
        mode, typ, sha = meta.split()
        if typ != "blob" or mode == "120000":
            special.append(path)
            continue
        entries.append((path, sha))
    blobs = cat_blobs(root, sorted({s for _p, s in entries}))
    return {p: blobs[s] for p, s in entries}, special, dict(entries)


class WorkTree:
    """The tracked files of a work tree (git ls-files), or every file of a plain folder."""

    def __init__(self, root, files=None):
        self.root = root
        if files is None:
            files = git_ls_files(root)
            if files is None:
                files = walk_files(root)
        self.files = files
        self.special = []

    def read(self, rel):
        full = os.path.join(self.root, *rel.split("/"))
        if not os.path.isfile(full):
            return None
        with open(full, "rb") as f:
            return f.read()


class Blobs:
    """Files held in memory (a commit's tree)."""

    def __init__(self, blobs, special=()):
        self.blobs = blobs
        self.files = sorted(blobs)
        self.special = list(special)

    def read(self, rel):
        return self.blobs.get(rel)


class Scanner:
    def __init__(self, deny=None, allow=None, polish_exempt=None):
        self.deny = deny or DenyList()
        self.allow = allow or AllowList()
        self.polish_exempt = [glob_to_regex(g) for g in (polish_exempt if polish_exempt is not None else POLISH_EXEMPT)]

    def scan_text(self, label, rel, text, kind, result):
        exempt = any(rx.match(rel) for rx in self.polish_exempt)
        seen = set()
        for no, cls, span, line in line_rules(rel, text, kind, exempt):
            if cls in ALLOWABLE and self.allow.allows(rel, cls, span, line):
                continue
            k = (no, cls)
            if k in seen:
                continue
            seen.add(k)
            result.hits.append(Hit(label, no, cls, span))
        for no, cls in self.deny.scan(text):
            k = (no, cls)
            if k in seen:
                continue
            seen.add(k)
            result.hits.append(Hit(label, no, cls))

    def name_label(self, rel, result, label=None):
        """The label a file is reported under: its path, or `<denied-name #n>` when the path itself matched the
        deny-list (then the path is the secret and is never printed; the deny hit is recorded once, on the placeholder)."""
        label = label or rel
        if label in result.redacted:
            return result.redacted[label]
        cats = []
        for _no, cls in self.deny.scan(rel):
            if cls not in cats:
                cats.append(cls)
        if not cats:
            return label
        ph = "<denied-name #%d>" % (len(result.redacted) + 1)
        result.redacted[label] = ph
        for cls in cats:
            result.hits.append(Hit(ph, 0, cls))
        return ph

    def scan_content(self, label, rel, data, result, binary_ok):
        if is_binary(rel, data):
            if not binary_ok:
                result.hits.append(Hit(label, 0, "binary"))
            meta = image_meta_blocks(data)
            if meta:
                result.hits.append(Hit(label, 0, "metadata"))
            short = ()
            if self.deny.text_only():
                if is_image(rel, data):
                    blob = image_scan_bytes(data)
                    short = (blob.decode("latin-1"), blob.decode("utf-8", "replace"))
                else:
                    short = (bin_strings(data),)
            for cls in self.deny.scan_blob(data) + self.deny.scan_views(short, text_only=True):
                result.hits.append(Hit(label, 0, cls))
            return
        self.scan_text(label, rel, data.decode("utf-8", "replace"), kind_of(rel), result)

    def scan_file(self, label, rel, data, result, binary_ok):
        self.scan_content(self.name_label(rel, result, label), rel, data, result, binary_ok)


def load_manifests(paths, read, source_private=False):
    """read(rel) -> bytes or None for a path inside the scanned tree; an absolute path is read from disk. The
    published view (dev-only block dropped) unless source_private."""
    m = Manifest()
    rels = set()
    for p in paths:
        if os.path.isabs(p):
            with open(p, "rb") as f:
                data = f.read()
        else:
            rel = posixpath.normpath(p.replace("\\", "/"))
            data = read(rel)
            if data is None:
                raise ValueError("manifest not found in the scanned tree: %s" % p)
            rels.add(rel)
        text = data.decode("utf-8-sig")
        m.add(text if source_private else strip_dev_only(text), p)
    return m, rels


def load_allow(path, read):
    if path:
        with open(path, "rb") as f:
            return AllowList.parse(f.read().decode("utf-8-sig"), path)
    data = read(ALLOW_DEFAULT)
    return AllowList.parse(data.decode("utf-8-sig"), ALLOW_DEFAULT) if data is not None else AllowList()


def gitleaks_files(files, result, labels):
    """gitleaks over exactly these files (written to a temporary folder), so it never reads anything else."""
    if not shutil.which("gitleaks"):
        result.gitleaks = "not found"
        return
    tmp = tempfile.mkdtemp(prefix="leakgate-gl-")
    try:
        for rel, data in files.items():
            full = os.path.join(tmp, *rel.split("/"))
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "wb") as f:
                f.write(data)
        found, state = run_gitleaks(tmp, set(files))
        result.gitleaks = state
        for p, ln, cls in found or []:
            result.hits.append(Hit(labels.get(p, p), ln, cls))
    finally:
        rmtree(tmp)


def scan_root(source, scanner, result, manifest=None, manifest_rels=(), source_private=False, gitleaks=True,
              classify_only=False):
    """Classify and scan one tree. In the published view (default) a file the manifest classes `private` is a
    `private-file` hit: it must not be in a public tree at all. source_private=True is for the export's source (the dev
    repository), where private files are expected and skipped."""
    keep, labels = {}, {}
    want_gl = gitleaks and not classify_only and shutil.which("gitleaks")
    for path in source.special:
        result.hits.append(Hit(scanner.name_label(path, result), 0, "special-entry"))
    for rel in source.files:
        cls = manifest.classify(rel) if manifest is not None else "ship"
        result.files[cls] += 1
        if cls == "private" and source_private:
            continue
        label = scanner.name_label(rel, result)
        if not source_private and private_path(rel):  # refused whatever the manifest's order says
            result.hits.append(Hit(label, 0, "private-file"))
            continue
        if cls == "private":
            result.hits.append(Hit(label, 0, "private-file"))
            continue
        if cls == "review":
            result.review.append(label)
        elif cls == "unclassified":
            result.unclassified.append(label)
            continue
        if classify_only:
            continue
        data = source.read(rel)
        if data is None:
            result.notes.append("missing on disk: %s" % label)
            continue
        if rel in manifest_rels:
            data = strip_dev_only(data.decode("utf-8-sig")).encode("utf-8")
        binary_ok = manifest.binary_ok(rel) if manifest is not None else \
            os.path.splitext(rel)[1].lower() in TREE_ASSET_EXT
        scanner.scan_content(label, rel, data, result, binary_ok)
        if want_gl:
            keep[rel], labels[rel] = data, label
    if gitleaks and not classify_only:
        gitleaks_files(keep, result, labels)


def scan_tree(path, scanner, result, gitleaks=True):
    """A built tree (the site output): every file is scanned as published; binaries by asset extension."""
    if not os.path.isdir(path):
        raise ValueError("--tree needs a folder: %s" % path)
    base = path.replace("\\", "/").rstrip("/")
    files = walk_files(path)
    keep, labels = {}, {}
    for rel in files:
        with open(os.path.join(path, rel), "rb") as f:
            data = f.read()
        label = scanner.name_label(rel, result, base + "/" + rel)
        scanner.scan_content(label, rel, data, result, os.path.splitext(rel)[1].lower() in TREE_ASSET_EXT)
        keep[rel], labels[rel] = data, label
    result.files["tree"] += len(files)
    if gitleaks:
        gitleaks_files(keep, result, labels)


def scan_bin(path, scanner, result, skip=()):
    """A firmware image: the public rules and the deny-list on its strings (4 bytes and longer), the deny-list's
    patterns that are not text-only on every byte.
    skip: public line-rule classes left out of the strings scan (only BIN_SKIPPABLE; the deny-list is never skipped)."""
    with open(path, "rb") as f:
        data = f.read()
    base = os.path.basename(path)
    label = scanner.name_label(base, result, path.replace("\\", "/"))
    before = {h.cls for h in result.hits if h.file == label}
    first = len(result.hits)
    scanner.scan_text(label, base + ".strings", bin_strings(data), "prose", result)
    if skip:
        result.hits[first:] = [h for h in result.hits[first:] if h.cls not in skip]
    found = {h.cls for h in result.hits if h.file == label} - before
    for cls in scanner.deny.scan_blob(data):
        if cls not in found:
            result.hits.append(Hit(label, 0, cls))
    result.files["bin"] += 1


def scan_messages(root, rng, scanner, result):
    args = ["log", "--format=%H%x00%B%x01"] + (rng.split() if isinstance(rng, str) else list(rng))
    out = git_out(root, args)
    for chunk in out.decode("utf-8", "replace").split(chr(1)):
        chunk = chunk.lstrip("\n")
        if NUL not in chunk:
            continue
        sha, body = chunk.split(NUL, 1)
        scanner.scan_text("commit:" + sha[:12], "COMMIT_MSG", body, "prose", result)
        result.files["message"] += 1


def introduced(root, commit):
    """(path, mode, blob id) of every file a commit adds or changes against each parent (a root commit: every file)."""
    out = git_out(root, ["diff-tree", "-r", "-z", "--root", "-m", "--no-renames", "--no-commit-id",
                         "--diff-filter=d", commit])
    parts = out.decode("utf-8").split(NUL)
    res, i = [], 0
    while i < len(parts):
        meta = parts[i]
        if not meta.startswith(":") or i + 1 >= len(parts):
            i += 1
            continue
        f = meta[1:].split()
        res.append((parts[i + 1], f[1], f[3]))
        i += 2
    return res


def object_exists(root, sha):
    return subprocess.run(["git", "-C", root, "cat-file", "-e", sha], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def pre_push(root, remote, lines, deny, manifest_paths, allow_path=None, source_private=False, gitleaks=True):
    """The pre-push check (git's stdin lines: <local ref> <local sha> <remote ref> <remote sha>). For every pushed
    ref: the tip's tree in full (classification and every file), then EVERY pushed commit's added or changed blobs
    (a word added and removed again inside the push is still caught), every pushed commit message and an annotated
    tag's own text. The manifest and the allow-list are the tip's."""
    result = Result()

    def zero(s):
        return not s.strip("0")

    for raw in lines:
        f = raw.split()
        if len(f) != 4 or zero(f[1]):
            continue  # a malformed line or a deleted ref
        ref, obj, remote_sha = f[0], f[1], f[3]
        # a branch or tag name that matches the deny-list is printed as a placeholder, like a file name
        ref = Scanner(deny).name_label(ref, result)
        typ = git_out(root, ["cat-file", "-t", obj]).decode().strip()
        r = subprocess.run(["git", "-C", root, "rev-parse", "--verify", "-q", obj + "^{commit}"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if r.returncode != 0:
            result.hits.append(Hit(ref, 0, "special-entry"))  # a tag of a tree or a blob: refused
            continue
        commit = r.stdout.decode().strip()
        blobs, special, ids = read_tree(root, commit)
        source = Blobs(blobs, special)
        scanner = Scanner(deny, load_allow(allow_path, source.read))
        if typ == "tag":  # the tag object: tagger line and message
            scanner.scan_text("tag:" + ref, "TAG_MSG", git_out(root, ["cat-file", "tag", obj]).decode("utf-8", "replace"),
                              "prose", result)
        manifest, mrels = load_manifests(manifest_paths or [MANIFEST_DEFAULT], source.read, source_private)
        scan_root(source, scanner, result, manifest, mrels, source_private, gitleaks)
        if not zero(remote_sha) and object_exists(root, remote_sha):
            rng = ["%s..%s" % (remote_sha, commit)]
        else:
            rng = [commit, "--not", "--remotes=" + remote]
        commits = git_out(root, ["rev-list"] + rng).decode().split()
        seen = set(ids.values())
        todo = []
        for c in commits:
            for path, mode, sha in introduced(root, c):
                label = "%s@%s" % (path, c[:12])
                if mode in ("120000", "160000"):
                    result.hits.append(Hit(scanner.name_label(path, result, label), 0, "special-entry"))
                    continue
                if sha not in seen:
                    seen.add(sha)
                    todo.append((path, sha, label))
        data = cat_blobs(root, sorted({s for _p, s, _l in todo}))
        keep, labels = {}, {}
        for path, sha, label in todo:
            cls = manifest.classify(path)
            if cls == "private" and source_private:
                continue
            label = scanner.name_label(path, result, label)
            result.files["history"] += 1
            if not source_private and private_path(path):
                cls = "private"
            if cls in ("private", "unclassified"):
                result.hits.append(Hit(label, 0, "private-file" if cls == "private" else "unclassified"))
                continue
            scanner.scan_content(label, path, data[sha], result, manifest.binary_ok(path))
            hist = path + "@" + sha[:12]
            keep[hist], labels[hist] = data[sha], label
        if gitleaks and keep:
            gitleaks_files(keep, result, labels)
        scan_messages(root, rng, scanner, result)
    return result


# --- output ----------------------------------------------------------------------------------------------------------
def summary_lines(result, strict_tags=False):
    out = []
    c = result.counts()
    f = result.files
    out.append("files: ship %d, private %d, review %d, unclassified %d%s" % (
        f["ship"], f["private"], f["review"], f["unclassified"],
        "".join(", %s %d" % (k, f[k]) for k in ("tree", "bin", "history", "message") if f[k])))
    out.append("deny-list: %s; gitleaks: %s" % (result.deny_source, result.gitleaks))
    if c:
        out.append("hits by class:")
        for cls, n in sorted(c.items(), key=lambda kv: (-kv[1], kv[0])):
            nf = len({h.file for h in result.hits if h.cls == cls})
            out.append("  %-22s %6d in %d files%s" % (cls, n, nf,
                                                      "  (report only)" if cls in NONFATAL and not strict_tags else ""))
    else:
        out.append("hits by class: none")
    out.append("result: %s" % ("PASS" if result.ok(strict_tags) else "FAIL"))
    return out


def print_text(result, strict_tags=False, summary_only=False, stream=None):
    stream = stream or sys.stdout
    if not summary_only:
        for rel in result.unclassified:
            print("%s: unclassified" % rel, file=stream)
        for rel in result.review:
            print("%s: review" % rel, file=stream)
        for h in sorted(result.hits, key=lambda h: (h.file, h.line, h.cls)):
            print("%s:%d: %s" % (h.file, h.line, h.cls), file=stream)
        for n in result.notes:
            print("note: " + n, file=stream)
    for line in summary_lines(result, strict_tags):
        print(line, file=stream)


def as_json(result, strict_tags=False):
    return {
        "ok": result.ok(strict_tags),
        "files": dict(result.files),
        "review": result.review,
        "unclassified": result.unclassified,
        "deny": result.deny_source,
        "gitleaks": result.gitleaks,
        "counts": dict(result.counts()),
        "hits": [h.as_dict() for h in sorted(result.hits, key=lambda h: (h.file, h.line, h.cls))],
        "notes": result.notes,
    }


def github_report(result, strict_tags=False, limit=50, env=None):
    shown = 0
    for h in sorted(result.fatal(strict_tags), key=lambda h: (h.file, h.line)):
        if shown >= limit:
            break
        print("::error file=%s,line=%d::leak gate: %s" % (h.file, max(h.line, 1), h.cls))
        shown += 1
    path = (os.environ if env is None else env).get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    c = result.counts()
    per_file = Counter(h.file for h in result.hits)
    lines = ["## Leak gate: %s" % ("PASS" if result.ok(strict_tags) else "FAIL"), "",
             "Files: ship %d, private %d, review %d, unclassified %d. Deny-list: %s." % (
                 result.files["ship"], result.files["private"], result.files["review"],
                 result.files["unclassified"], result.deny_source), "",
             "| class | hits |", "|---|---|"]
    lines += ["| %s | %d |" % (k, v) for k, v in sorted(c.items(), key=lambda kv: -kv[1])]
    lines += ["", "| top files | hits |", "|---|---|"]
    lines += ["| `%s` | %d |" % (k, v) for k, v in per_file.most_common(15)]
    with open(path, "a", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


# --- the hook --------------------------------------------------------------------------------------------------------
HOOK = """#!/bin/sh
# pre-push: the leak gate on every pushed commit (written by tools/leakgate.py --install-hook)
deny=%(deny)s
manifest=%(manifest)s
%(secrets_var)s[ -f "$deny" ] || { echo "leak gate: the deny-list $deny is missing; push refused" >&2; exit 1; }
%(secrets_check)spy=
for c in python3 python; do "$c" -c "" >/dev/null 2>&1 && { py=$c; break; }; done
[ -n "$py" ] || { echo "leak gate: no python; push refused" >&2; exit 1; }
exec "$py" -I tools/leakgate.py --pre-push "$1" --manifest "$manifest" --deny "$deny"%(secrets_arg)s --strict-tags
"""
HOOK_SECRETS = {
    "secrets_var": "secrets=%s\n",
    "secrets_check": '[ -f "$secrets" ] || { echo "leak gate: the secrets file $secrets is missing; push refused" >&2;'
                     ' exit 1; }\n',
    "secrets_arg": ' --deny-secrets "$secrets"',
}


def sh_quote(s):
    return "'" + s.replace("'", "'\"'\"'") + "'"


def install_hook(root, deny, manifest, force=False, secrets=None, skip=()):
    r = subprocess.run(["git", "-C", root, "rev-parse", "--git-path", "hooks"], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise ValueError("not a git repository: %s" % root)
    hooks = r.stdout.decode().strip()
    if not os.path.isabs(hooks):
        hooks = os.path.join(root, hooks)
    os.makedirs(hooks, exist_ok=True)
    path = os.path.join(hooks, "pre-push")
    if os.path.exists(path) and not force:
        raise ValueError("a pre-push hook exists; --force replaces it")
    deny = os.path.abspath(deny)  # the hook may run from anywhere in the work tree's history
    fill = {"deny": sh_quote(deny.replace("\\", "/")), "manifest": sh_quote(manifest.replace("\\", "/")),
            "secrets_var": "", "secrets_check": "", "secrets_arg": ""}
    if skip and not secrets:
        raise ValueError("--deny-secrets-skip needs --deny-secrets")
    if secrets:
        add_secrets_file(DenyList(), secrets, skip)  # fails now, not at the first push; prints what it skips
        fill.update(HOOK_SECRETS)
        fill["secrets_var"] %= sh_quote(os.path.abspath(secrets).replace("\\", "/"))
        if skip:
            fill["secrets_arg"] += " --deny-secrets-skip " + sh_quote(",".join(skip))
    body = HOOK % fill
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(body)
    os.chmod(path, 0o755)
    return path


# --- main ------------------------------------------------------------------------------------------------------------
def repo_root():
    r = subprocess.run(["git", "rev-parse", "--show-toplevel"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode == 0:
        return r.stdout.decode().strip()
    return os.getcwd()


def deny_label(deny):
    """e.g. `file (28 patterns, 3 text-only) + secrets (4 values)`; counts only, never a pattern or a value. The
    environment's list (CI, whose log is public) does not say how many of its patterns are short."""
    out = deny.source
    if deny.source == "env":
        out = "env (%d patterns; short patterns: text and strings only)" % len(deny.patterns)
    elif deny.source != "absent":
        k = deny.text_only()
        out = "%s (%d patterns%s)" % (deny.source, len(deny.patterns), ", %d text-only" % k if k else "")
    if deny.secret_files:
        out += " + secrets (%d values%s)" % (deny.secret_values,
                                             ", %d skipped" % deny.secret_skipped if deny.secret_skipped else "")
    return out


def emit(result, a, env=None):
    if a.json:
        print(json.dumps(as_json(result, a.strict_tags), indent=1))
    else:
        print_text(result, a.strict_tags, a.summary_only)
    if a.github:
        github_report(result, a.strict_tags, env=env)
    return 0 if result.ok(a.strict_tags) else 1


def main(argv=None, env=None, stdin=None):
    env = os.environ if env is None else env
    ap = argparse.ArgumentParser(description="The publishing leak gate (see the module docstring).")
    ap.add_argument("--root", help="the repository or folder to classify and scan (default: the git top level of the cwd)")
    ap.add_argument("--manifest", action="append", default=[],
                    help="path manifest, relative to the scanned tree; repeatable (first match wins in the given order)")
    ap.add_argument("--source-is-private", action="store_true",
                    help="the export's source tree: private files are expected and skipped, and the manifest's dev-only "
                         "lines apply; never in CI or the hook (there a private file is a hit)")
    ap.add_argument("--no-manifest", action="store_true", help="scan --root as a published tree (every file ship)")
    ap.add_argument("--allow", help="allow-list file (default: %s in the scanned tree when present)" % ALLOW_DEFAULT)
    ap.add_argument("--deny", help="private deny-list file (else the env var LEAKGATE_DENY, base64)")
    ap.add_argument("--deny-secrets", action="append", default=[], metavar="FILE",
                    help="a C header whose `#define NAME \"value\"` values are deny patterns (the firmware's "
                         "secrets.h); repeatable; never printed; a missing file is an error")
    ap.add_argument("--deny-secrets-skip", action="append", default=[], metavar="NAME[,NAME]",
                    help="define names whose values are public words, not secrets (names only, never values); "
                         "each skipped name is printed")
    ap.add_argument("--tree", action="append", default=[], help="also scan a built folder, every file as published")
    ap.add_argument("--bin", action="append", default=[],
                    help="also scan a binary (its strings; deny patterns of 6+ characters on every byte)")
    ap.add_argument("--bin-skip", default="",
                    help="comma-separated line-rule classes left out of the --bin strings scan (only %s)"
                    % ", ".join(sorted(BIN_SKIPPABLE)))
    ap.add_argument("--rev", help="scan the tree of this commit (raw blobs) instead of the working tree")
    ap.add_argument("--messages", help="also scan the commit messages of this git log range")
    ap.add_argument("--pre-push", metavar="REMOTE", help="the pre-push check: reads git's ref lines from stdin")
    ap.add_argument("--no-gitleaks", action="store_true")
    ap.add_argument("--strict-tags", action="store_true", help="tag hits fail too")
    ap.add_argument("--classify-only", action="store_true", help="the manifest check only")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--github", action="store_true", help="annotations and the step summary in GitHub Actions")
    ap.add_argument("--summary-only", action="store_true")
    ap.add_argument("--install-hook", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(errors="replace")
        except (ValueError, OSError):
            pass
    root = os.path.abspath(a.root or repo_root())

    if a.install_hook:
        deny = a.deny or env.get("LEAKGATE_DENY_FILE", "")
        if not deny:
            print("leakgate: --install-hook needs --deny FILE (the private deny-list)", file=sys.stderr)
            return 2
        if len(a.deny_secrets) > 1:
            print("leakgate: --install-hook takes one --deny-secrets FILE", file=sys.stderr)
            return 2
        try:
            p = install_hook(root, deny, (a.manifest or [MANIFEST_DEFAULT])[0], a.force,
                             a.deny_secrets[0] if a.deny_secrets else None, skip_keys(a.deny_secrets_skip))
        except ValueError as e:
            print("leakgate: %s" % e, file=sys.stderr)
            return 2
        print("pre-push hook written: %s" % p)
        return 0

    bin_skip = {c.strip() for c in a.bin_skip.split(",") if c.strip()}
    if bin_skip - BIN_SKIPPABLE:
        print("leakgate: --bin-skip takes only %s" % ", ".join(sorted(BIN_SKIPPABLE)), file=sys.stderr)
        return 2

    try:
        deny = load_deny(a.deny, env)
    except (OSError, ValueError, UnicodeDecodeError) as e:
        print("leakgate: deny-list: %s" % e, file=sys.stderr)
        return 2
    try:
        skip = skip_keys(a.deny_secrets_skip)
        if skip and not a.deny_secrets:
            raise ValueError("--deny-secrets-skip needs --deny-secrets")
        for p in a.deny_secrets:
            add_secrets_file(deny, p, skip)
    except ValueError as e:
        print("leakgate: %s" % e, file=sys.stderr)
        return 2

    try:
        if a.pre_push:
            lines = (stdin if stdin is not None else sys.stdin).read().splitlines()
            result = pre_push(root, a.pre_push, lines, deny, a.manifest, a.allow, a.source_is_private,
                              not a.no_gitleaks)
            result.deny_source = deny_label(deny)
            if a.no_gitleaks:
                result.gitleaks = "skipped"
            return emit(result, a, env)

        result = Result()
        result.deny_source = deny_label(deny)
        if a.rev:
            blobs, special, _ids = read_tree(root, a.rev)
            source = Blobs(blobs, special)
        else:
            source = WorkTree(root)
        scanner = Scanner(deny, load_allow(a.allow, source.read))
        # the root is scanned unless only --tree / --bin / --messages were asked for
        scan_the_root = a.no_manifest or a.manifest or a.rev or not (a.tree or a.bin or a.messages)
        if scan_the_root:
            manifest, mrels = (None, set()) if a.no_manifest else \
                load_manifests(a.manifest or [MANIFEST_DEFAULT], source.read, a.source_is_private)
            scan_root(source, scanner, result, manifest, mrels, a.source_is_private, not a.no_gitleaks,
                      a.classify_only)
        for t in a.tree:
            scan_tree(t, scanner, result, gitleaks=not a.no_gitleaks)
        for b in a.bin:
            scan_bin(b, scanner, result, bin_skip)
        if a.messages:
            scan_messages(root, a.messages, scanner, result)
        if a.no_gitleaks:
            result.gitleaks = "skipped"
    except (OSError, ValueError, UnicodeDecodeError) as e:
        print("leakgate: %s" % e, file=sys.stderr)
        return 2
    return emit(result, a, env)


if __name__ == "__main__":
    sys.exit(main())
