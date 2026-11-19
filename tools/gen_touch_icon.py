"""Renders the home-screen icon:
firmware/fauxcloud/web/touch-icon.png, a 512 x 512 opaque PNG -- the mark from web/favicon.svg in white on the dark sky
#14366a -- for <link rel="apple-touch-icon">. iOS draws a transparent icon on black, so there is no alpha.

The mark's paths are taken from favicon.svg as they are (one source); only the colour, the scale and the background change.
resvg renders the SVG (the renderer ESPHome uses), Pillow reduces it to a 16-colour palette (4-bit, no dithering, no alpha)
and writes it with optimize=True. Both steps are deterministic: the same inputs give the same bytes.

Needs pillow and resvg_py (not in the project's toolchain on purpose: the PNG is committed, CI never re-renders it):
e.g. a venv with pillow 12.2.0 and resvg_py 0.2.6 (the versions the committed PNG came from) or
`pip install pillow resvg-py`; another version may render a byte-different PNG (--check then differs).

Usage: python tools/gen_touch_icon.py [--size N]           (writes the PNG, prints its size; N default 512)
       python tools/gen_touch_icon.py [--size N] --check   (exit 1 when the committed PNG differs from a fresh render)
512 px: iOS 18+ has a large-icon Home Screen mode (a tile of about 215 px), and iOS 26
composites Liquid Glass over a flat web-clip icon, where a larger bitmap stays sharper; 180 / 360 regenerate with --size.
"""
import io
import pathlib
import re
import sys

import resvg_py
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB = ROOT / "firmware" / "fauxcloud" / "web"
SRC, OUT = WEB / "favicon.svg", WEB / "touch-icon.png"
SIZE = 512          # apple-touch-icon (iOS scales it; 180 is the @3x tile); iOS rounds the corners itself
SKY = "#14366a"     # the dark sky: the dashboard's first paint (index.html) and the dark theme-color
INK = "#ffffff"
FILL = 0.66         # the mark's width (strokes included) as a share of the icon: clear of iOS's rounded mask
COLOURS = 16        # 4-bit palette: the sky, white and the anti-aliased edge between them
MAX_BYTES = 4300    # the ceiling for the PNG (<= 4.2 KB at 512 px)


def mark_paths(svg):
    """The <path> elements of the mark and its stroke width, as favicon.svg has them."""
    g = re.search(r'<g\b[^>]*stroke-width="([\d.]+)"[^>]*>(.*?)</g>', svg, re.S)
    if not g:
        raise SystemExit("favicon.svg: the mark's <g> group was not found")
    return float(g.group(1)), g.group(2)


def icon_svg():
    width, paths = mark_paths(SRC.read_text(encoding="utf-8"))
    # the mark's extent in favicon.svg's 256 x 256 box, strokes included (x 26..230, y 55..180, half a stroke each side)
    x0, x1, y0, y1 = 26 - width / 2, 230 + width / 2, 55 - width / 2, 180 + width / 2
    s = SIZE * FILL / (x1 - x0)
    tx, ty = SIZE / 2 - (x0 + x1) / 2 * s, SIZE / 2 - (y0 + y1) / 2 * s
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
            '<rect width="%d" height="%d" fill="%s"/>'
            '<g transform="translate(%.3f %.3f) scale(%.5f)" fill="none" stroke="%s" stroke-width="%g" '
            'stroke-linecap="round" stroke-linejoin="round">%s</g></svg>'
            % (SIZE, SIZE, SIZE, SIZE, SIZE, SIZE, SKY, tx, ty, s, INK, width, paths))


def render():
    png = bytes(resvg_py.svg_to_bytes(svg_string=icon_svg(), width=SIZE, height=SIZE))
    img = Image.open(io.BytesIO(png)).convert("RGB")  # opaque: the background rect covers every pixel
    pal = img.quantize(colors=COLOURS, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    out = io.BytesIO()
    pal.save(out, format="PNG", optimize=True, bits=4)
    return out.getvalue()


def main():
    global SIZE
    if "--size" in sys.argv:
        SIZE = int(sys.argv[sys.argv.index("--size") + 1])
    data = render()
    if len(data) > MAX_BYTES:
        raise SystemExit("touch-icon.png: %d B > %d B" % (len(data), MAX_BYTES))
    if "--check" in sys.argv[1:]:
        same = OUT.is_file() and OUT.read_bytes() == data
        print("touch-icon.png is up to date" if same else "touch-icon.png differs from a fresh render")
        sys.exit(0 if same else 1)
    OUT.write_bytes(data)
    print("wrote %s: %d B (%d x %d, %d colours, no alpha)" % (OUT.relative_to(ROOT), len(data), SIZE, SIZE, COLOURS))


if __name__ == "__main__":
    main()
