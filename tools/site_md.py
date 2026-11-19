"""site_md.py -- the docs' Markdown to HTML, stdlib only (tools/build_site.py renders /docs/ with it).

A deliberately small subset, the one the public docs are written in (site/README.md, "Writing a docs page"), so every
page also reads well on GitHub:
  front matter (--- title / order / lede ---), ##, ### and #### headings (ids from the text; h2s feed the "All pages"
  index, h2s and h3s the outline),
  paragraphs, **bold**, *italic*, `code`, [links](target), <https://autolinks> and bare https:// URLs,
  a button: a paragraph that is only [**Label**](target),
  a labelled entry: a paragraph that starts **Cause.** / **Fix.** / **fauxcloud shows this as** (a label alone takes the
  list after it and the plain paragraphs after that list, up to the next label, heading or other block),
  - bullets and 1. numbered lists (one nesting level; an item may hold more paragraphs, indented),
  steps: a numbered list whose items carry a paragraph starting "**You should see**",
  a photo: a paragraph that is only ![alt](path of a variant under site/assets/ "optional caption") (the build writes
  the <figure>),
  callouts: > [!NOTE] / > [!TIP] (a note: quiet, an info icon) and > [!WARNING] (amber); the first line in bold is the
  title, else the kind names it; plain > quotes,
  fenced code with a language and an optional title="...", GFM pipe tables, --- rules.
Raw HTML is escaped, never passed through; anything else is plain text. Unknown block syntax that would silently
render wrong (an image inside a line, HTML blocks, setext headings, deeper nesting) is refused with the file and line.
"""
import html
import re

FENCE = re.compile(r"^(```+)\s*([A-Za-z0-9_+-]*)\s*(?:title=\"([^\"]*)\")?\s*$")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
BULLET = re.compile(r"^( {0,3})[-*]\s+(.*)$")
NUMBER = re.compile(r"^( {0,3})(\d{1,3})[.)]\s+(.*)$")
TABLE_SEP = re.compile(r"^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)*\|?\s*$")
ALERT = re.compile(r"^\[!(NOTE|WARNING|TIP|IMPORTANT|CAUTION)\]\s*$")
SEE = "**You should see**"
# a paragraph that is one link with a bold label is a call to action: the site's button (on GitHub, a bold link)
BUTTON = re.compile(r"^\[\*\*([^*\]]+)\*\*\]\(([^)\s]+)\)$")
# the labels of a troubleshooting entry: a <dl> with the label in its own column (on GitHub, a bold run)
LABEL = re.compile(r"^\*\*(Cause\.|Fix\.|fauxcloud shows this as)\*\*(?:\s+(.*))?$")
# a paragraph that is one image: a photo of the site (the build's figure hook maps the path to its variants); a title in
# quotes becomes its caption (on GitHub, the image's tooltip)
IMAGE = re.compile(r"^!\[([^\]]+)\]\(([^)\s]+)(?:\s+\"([^\"]+)\")?\)$")


class MarkdownError(SystemExit):
    pass


def front_matter(text):
    """(meta dict, body, body's first line number) -- the --- block at the very top, simple `key: value` lines."""
    lines = text.split("\n")
    if lines and lines[0].strip() == "---":
        for i in range(1, len(lines)):
            if lines[i].strip() == "---":
                meta = {}
                for ln in lines[1:i]:
                    if ln.strip():
                        k, _, v = ln.partition(":")
                        meta[k.strip()] = v.strip()
                return meta, "\n".join(lines[i + 1:]), i + 2
    return {}, text, 1


def slugify(text):
    s = re.sub(r"<[^>]+>", "", text)
    s = html.unescape(s).lower()
    s = re.sub(r"[^a-z0-9]+", "-", s).strip("-")
    return s or "section"


# ---------------------------------------------------------------- inline
CODE_SPAN = re.compile(r"(`+)(.+?)\1")
LINK = re.compile(r"\[([^\]]+)\]\(([^)\s]+)\)")
AUTOLINK = re.compile(r"&lt;(https?://[^\s&\"'<>]+)&gt;")
BARE = re.compile(r"(?<![\"'=/\w])(https://[A-Za-z0-9./_~%#?&;=+-]*[A-Za-z0-9/_~#=-])")
BOLD = re.compile(r"\*\*(.+?)\*\*")
ITALIC = re.compile(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])")


class Inline:
    """Inline rendering with a link hook: link(target) -> href (the build maps .md files to site URLs)."""

    def __init__(self, link):
        self.link = link

    def __call__(self, text):
        slots = []

        def keep(s):
            slots.append(s)
            return "\x00%d\x00" % (len(slots) - 1)

        text = CODE_SPAN.sub(lambda m: keep("<code>%s</code>" % html.escape(m.group(2).strip(), quote=False)), text)
        if re.search(r"!\[[^\]]*\]\(", text):
            raise MarkdownError("site_md: an image inside a line (a photo is a paragraph of its own): %s" % text[:80])
        text = html.escape(text, quote=False)

        def a(m):
            # the whole anchor, label included, goes into one slot, so the URL rules below never see a label
            label, target = m.group(1), html.unescape(m.group(2))
            label = ITALIC.sub(r"<em>\1</em>", BOLD.sub(r"<b>\1</b>", label))
            return keep('<a href="%s">%s</a>' % (html.escape(self.link(target), quote=True), label))

        def url(m):
            u = m.group(1)  # already HTML-escaped text: unescape once, then escape for an attribute
            return keep('<a href="%s">%s</a>' % (html.escape(html.unescape(u), quote=True), u))

        text = LINK.sub(a, text)
        text = AUTOLINK.sub(url, text)
        text = BARE.sub(url, text)
        text = BOLD.sub(r"<b>\1</b>", text)
        text = ITALIC.sub(r"<em>\1</em>", text)
        while "\x00" in text:
            text = re.sub(r"\x00(\d+)\x00", lambda m: slots[int(m.group(1))], text)
        return text


# ---------------------------------------------------------------- blocks
class Doc:
    """One rendered page: html, h2 list [(id, text)], every id."""

    def __init__(self, source, link, first_line=1, figure=None):
        self.source = source
        self.inline = Inline(link)
        self.figure = figure
        self.first = first_line
        self.toc = []
        self.outline = []
        self.ids = set()
        self.first_h1 = None

    def fail(self, n, why):
        raise MarkdownError("site_md: %s:%d: %s" % (self.source, self.first + n, why))

    def uid(self, text):
        base = slugify(text)
        s, k = base, 2
        while s in self.ids:
            s, k = "%s-%d" % (base, k), k + 1
        self.ids.add(s)
        return s

    def render(self, text):
        return self.blocks(text.split("\n"), 0, top=True)

    def blocks(self, lines, offset, top=False):
        out, i, n = [], 0, len(lines)
        while i < n:
            line = lines[i]
            s = line.strip()
            if not s:
                i += 1
                continue
            m = FENCE.match(s)
            if m:
                j = i + 1
                while j < n and lines[j].strip() != m.group(1):
                    j += 1
                if j >= n:
                    self.fail(offset + i, "code fence never closed")
                indent = len(line) - len(line.lstrip())
                code = "\n".join(ln[indent:] if ln[:indent].strip() == "" else ln for ln in lines[i + 1:j])
                label = m.group(3) or m.group(2) or "Code"
                out.append('<div class="code"><div class="bar2"><span>%s</span><button type="button">Copy</button></div>'
                           "<pre><code>%s</code></pre></div>" % (html.escape(label), html.escape(code, quote=False)))
                i = j + 1
                continue
            m = HEADING.match(s)
            if m:
                level, title = len(m.group(1)), m.group(2)
                if level == 1:
                    if top and self.first_h1 is None and not out:
                        self.first_h1 = title  # the template prints the title; a leading H1 is dropped
                        i += 1
                        continue
                    self.fail(offset + i, "a second level-1 heading (the page title comes from the front matter)")
                if level > 4:
                    self.fail(offset + i, "headings deeper than #### are not used on the site")
                inner = self.inline(title)
                hid = self.uid(inner)
                if level == 2 and top:
                    self.toc.append((hid, re.sub(r"<[^>]+>", "", inner)))
                if level <= 3 and top:
                    self.outline.append((level, hid, re.sub(r"<[^>]+>", "", inner)))
                out.append('<h%d id="%s">%s</h%d>' % (level, hid, inner, level))
                i += 1
                continue
            if s in ("---", "***", "___"):
                out.append("<hr>")
                i += 1
                continue
            if s.startswith(">"):
                j = i
                while j < n and lines[j].strip().startswith(">"):
                    j += 1
                body = [re.sub(r"^\s*>\s?", "", ln) for ln in lines[i:j]]
                out.append(self.quote(body, offset + i))
                i = j
                continue
            if s.startswith("|") and i + 1 < n and TABLE_SEP.match(lines[i + 1]):
                j = i + 2
                while j < n and lines[j].strip().startswith("|"):
                    j += 1
                out.append(self.table(lines[i], lines[i + 2:j], offset + i))
                i = j
                continue
            if BULLET.match(line) or NUMBER.match(line):
                j, html_ = self.listing(lines, i, offset)
                out.append(html_)
                i = j
                continue
            im = IMAGE.match(s)
            if im and not top:
                self.fail(offset + i, "a photo only at the page's top level, not inside a list item or a callout")
            if im and self.figure and (i + 1 >= n or not lines[i + 1].strip()):
                out.append(self.figure(html.unescape(im.group(1)), im.group(2), self.inline(im.group(3)) if im.group(3) else ""))
                i += 1
                continue
            if s.startswith("![") or re.match(r"^<[A-Za-z!/]", s):
                self.fail(offset + i, "an image only as a paragraph of its own; raw HTML blocks are not supported")
            if i + 1 < n and re.match(r"^\s*(=+|-+)\s*$", lines[i + 1]) and lines[i + 1].strip():
                self.fail(offset + i, "setext headings are not supported (use ## )")
            j = i
            para = []
            while j < n and lines[j].strip() and not self.starts_block(lines, j):
                para.append(lines[j].strip())
                j += 1
            text = " ".join(para)
            b, lab = BUTTON.match(text), LABEL.match(text)
            if b:
                out.append('<p class="ctas"><a class="btn primary" href="%s">%s</a></p>' % (
                    html.escape(self.inline.link(html.unescape(b.group(2))), quote=True), html.escape(b.group(1))))
            elif lab:
                rest = lab.group(2) or ""
                value = "<p>%s</p>" % self.inline(rest) if rest else ""
                k = j
                while not rest and k < n and not lines[k].strip():
                    k += 1
                if not rest and k < n and (BULLET.match(lines[k]) or NUMBER.match(lines[k])):
                    j, value = self.listing(lines, k, offset)
                    j, more = self.follow(lines, j)
                    value += more
                elif not rest:
                    self.fail(offset + i, "a label alone must be followed by a list")
                out.append('<dl class="lab"><dt>%s</dt><dd>%s</dd></dl>' % (html.escape(lab.group(1).rstrip(".")), value))
            else:
                out.append("<p>%s</p>" % self.inline(text))
            i = j
        return "\n".join(out)

    def follow(self, lines, j):
        """(next line, html): the plain paragraphs from line j on, up to a label, a button, a heading or any other block."""
        out, n = [], len(lines)
        while True:
            k = j
            while k < n and not lines[k].strip():
                k += 1
            s = lines[k].strip() if k < n else ""
            if not s or self.starts_block(lines, k) or s in ("---", "***", "___") or s.startswith(("![", "<")):
                return j, "".join(out)
            e, para = k, []
            while e < n and lines[e].strip() and not self.starts_block(lines, e):
                para.append(lines[e].strip())
                e += 1
            text = " ".join(para)
            if LABEL.match(text) or BUTTON.match(text):
                return j, "".join(out)
            out.append("\n<p>%s</p>" % self.inline(text))
            j = e

    def starts_block(self, lines, j):
        s = lines[j].strip()
        return bool(FENCE.match(s) or HEADING.match(s) or s.startswith(">") or BULLET.match(lines[j]) or NUMBER.match(lines[j])
                    or (s.startswith("|") and j + 1 < len(lines) and TABLE_SEP.match(lines[j + 1])))

    def listing(self, lines, i, offset):
        ordered = bool(NUMBER.match(lines[i]))
        marker = NUMBER if ordered else BULLET
        items, n, j = [], len(lines), i
        while j < n:
            m = marker.match(lines[j])
            if not m:
                break
            base = len(m.group(1))
            first = m.group(3) if ordered else m.group(2)
            content_indent = len(lines[j]) - len(first)
            body = [first]
            j += 1
            while j < n:
                ln = lines[j]
                if not ln.strip():
                    # a blank line: the item goes on if the next text line is indented under it
                    k = j
                    while k < n and not lines[k].strip():
                        k += 1
                    if k < n and len(lines[k]) - len(lines[k].lstrip()) > base and not marker.match(lines[k]):
                        body += [""] * (k - j)
                        j = k
                        continue
                    break
                ind = len(ln) - len(ln.lstrip())
                if ind <= base and (marker.match(ln) or BULLET.match(ln) or NUMBER.match(ln)):
                    break
                if ind <= base and self.starts_block(lines, j):
                    break
                body.append(ln[min(ind, content_indent):] if ind >= 2 else ln.strip())
                j += 1
            items.append(body)
            # a blank line, then the next marker of the same list: keep going
            k = j
            while k < n and not lines[k].strip():
                k += 1
            if k < n and marker.match(lines[k]) and len(marker.match(lines[k]).group(1)) == base:
                j = k
                continue
            break
        steps = ordered and any(any(ln.strip().startswith(SEE) for ln in b) for b in items)
        parts = []
        for b in items:
            nested = any(BULLET.match(ln) or NUMBER.match(ln) for ln in b[1:])
            loose = steps or "" in b
            if steps:
                parts.append("<li>%s</li>" % self.step(b, offset + i))
            elif loose or nested:
                inner = self.blocks(b, offset + i)
                if not loose and inner.startswith("<p>"):
                    inner = re.sub(r"^<p>(.*?)</p>", r"\1", inner, count=1, flags=re.S)
                parts.append("<li>%s</li>" % inner)
            else:
                parts.append("<li>%s</li>" % self.inline(" ".join(x.strip() for x in b)))
        if steps:
            return j, '<ol class="steps">\n%s\n</ol>' % "\n".join(parts)
        tag = "ol" if ordered else "ul"
        return j, "<%s>\n%s\n</%s>" % (tag, "\n".join(parts), tag)

    def step(self, body, line):
        html_ = self.blocks(body, line)
        # the "You should see" paragraph becomes the check line under the step
        return re.sub(r"<p><b>You should see</b>(.*?)</p>", r'<p class="see"><b>You should see</b>\1</p>', html_, flags=re.S)

    def quote(self, body, line):
        m = ALERT.match(body[0].strip()) if body else None
        if not m:
            return "<blockquote>%s</blockquote>" % self.blocks(body, line)
        kind = m.group(1)
        rest = body[1:]
        title = ""
        if rest and re.match(r"^\*\*[^*]+\*\*\s*$", rest[0].strip()):
            title = self.inline(rest[0].strip()[2:-2])
            rest = rest[1:]
        warn = kind in ("WARNING", "CAUTION", "IMPORTANT")
        icon = "i-alert" if warn else "i-info"
        inner = self.blocks(rest, line + 1)
        return ('<div class="note%s" role="note"><svg aria-hidden="true"><use href="#%s"/></svg><b>%s</b>%s</div>'
                % ("" if warn else " info", icon, title or ("Warning" if warn else "Tip" if kind == "TIP" else "Note"), inner))

    def table(self, head, rows, line):
        def cells(ln):
            ln = ln.strip()
            if ln.startswith("|"):
                ln = ln[1:]
            if ln.endswith("|") and not ln.endswith("\\|"):
                ln = ln[:-1]
            out, cur, tick = [], "", False
            for ch in ln:
                if ch == "`":
                    tick = not tick
                if ch == "|" and not tick:
                    out.append(cur)
                    cur = ""
                else:
                    cur += ch
            out.append(cur)
            return [c.strip().replace("\\|", "|") for c in out]
        heads = [self.inline(c) for c in cells(head)]
        body = [cells(r) for r in rows]
        for k, r in enumerate(body):
            if len(r) != len(heads):  # GitHub drops or pads the difference, so the two would show different tables
                self.fail(line + 2 + k, "a table row with %d cells under a header of %d" % (len(r), len(heads)))
        # each cell names its column (data-label): on a phone, a table of three or more columns shows its rows as cards;
        # there the display change drops the table semantics in some engines, so the roles say them again
        wide = len(heads) >= 3
        role = (lambda r: ' role="%s"' % r) if wide else (lambda r: "")  # noqa: E731
        labels = [html.escape(html.unescape(re.sub(r"<[^>]+>", "", h)), quote=True) for h in heads]
        th = "".join('<th scope="col"%s>%s</th>' % (role("columnheader"), h) for h in heads)
        trs = "\n".join("<tr%s>%s</tr>" % (role("row"), "".join('<td%s data-label="%s">%s</td>' % (role("cell"), labels[k],
                                                                                                 self.inline(c))
                                                                for k, c in enumerate(r))) for r in body)
        return ('<div class="tw"><table%s>\n<thead%s><tr%s>%s</tr></thead>\n<tbody%s>\n%s\n</tbody>\n</table></div>'
                % (role("table"), role("rowgroup"), role("row"), th, role("rowgroup"), trs))


def render(text, source, link, outline=None, figure=None):
    """(meta, html, toc [(id, text)], ids) of one Markdown file; link(target) maps a link target to an href.
    A list given as outline gets the page's h2s and h3s as (level, id, text); figure(alt, target) -> the html of a photo
    (without it, an image is refused); figure(alt, target, caption html) -> the html of a photo."""
    meta, body, first = front_matter(text.replace("\r\n", "\n"))
    d = Doc(source, link, first, figure)
    out = d.render(body)
    if d.first_h1 and "title" not in meta:
        meta["title"] = d.first_h1
    if outline is not None:
        outline.extend(d.outline)
    return meta, out, d.toc, d.ids
