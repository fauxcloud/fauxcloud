"""site_images.py -- the site's photo variants, made once on a dev machine (not in CI): PIL, every metadata block dropped.

Each use of a photo on the site gets two widths (1x and 2x of its largest CSS width), JPEG quality 80 (79 where
QUALITY_FOR says), progressive,
4:2:0, no EXIF/ICC/XMP/comment: the encoder is given pixels only. The originals are private and stay outside the site;
the variants land in site/assets/ under <name>-<width>.jpg and are committed.

Usage (PIL from the dev machine's Python; python -I sees no user site-packages, so run it plainly):
    python tools/site_images.py SRC_DIR            # write the variants into site/assets/, print the bytes
    python tools/site_images.py SRC_DIR --list     # the table only
"""
import argparse
import io
import pathlib
import sys

from PIL import Image, ImageOps

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "site" / "assets"
QUALITY = 80

# source photo -> (variant name, widths): the 1x and 2x of the largest width the page gives the slot
PHOTOS = {
    "board-devkit.jpg": ("board", (300, 600)),            # "What you need": the board, portrait 3:4
    "case-two-v2.jpg": ("cases", (560, 1120)),             # "Flash it. Print a case. Done.", landscape
    "case-two-v2-portrait.jpg": ("cases-tall", (400, 800)),  # the same at phone widths (the <picture> source)
    "station-post.jpg": ("station", (380, 760)),           # Compatibility, portrait 3:4
    "case-render-exploded.png": ("case", (340, 680)),      # Boards, the case block: the designer's render, 4:3
    "phone-dashboard.jpg": ("phone", (200, 400)),          # Dashboard, "On your phone": a docs portrait, 3:4
    # demo screenshots (375 px at 2x, dark): the docs slot is 200 px; the 750 px width is what the popout shows
    "shot-dashboard.png": ("shot-dashboard", (300, 750)),
    "shot-airport.png": ("shot-airport", (300, 750)),
    "shot-online.png": ("shot-online", (300, 750)),
    "shot-outlook.png": ("shot-outlook", (300, 750)),
    "shot-settings.png": ("shot-settings", (300, 750)),
}
# variants encoded at another quality: cases-560 and the case renders are re-encoded one step lower (79); the
# screenshots are q79 for size (their text stays crisp at 79, and 85 costs about 81 KB more over the five); every other
# variant keeps QUALITY
QUALITY_FOR = {"cases-560.jpg": 79, "case-340.jpg": 79, "case-680.jpg": 79}
QUALITY_FOR.update({"shot-%s-%d.jpg" % (n, w): 79 for n in ("dashboard", "airport", "online", "outlook", "settings") for w in (300, 750)})


def clean(im):
    """A new image with the pixels only: no info dict, no EXIF, no ICC profile, no comments."""
    im = im.convert("RGB")
    return Image.frombytes("RGB", im.size, im.tobytes())


def variant(src, width):
    im = ImageOps.exif_transpose(Image.open(src))  # a rotation kept in EXIF is applied before the metadata goes
    im.load()
    if width > im.width:
        raise SystemExit("site_images: %s is %d px wide, %d asked (never upscale)" % (src.name, im.width, width))
    h = round(im.height * width / im.width)
    return clean(im.resize((width, h), Image.LANCZOS))


def encode(im, quality=QUALITY):
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=quality, optimize=True, progressive=True, subsampling="4:2:0")
    return buf.getvalue()


def run(table, src_dir, write):
    rows = []
    for name, (base, widths) in table.items():
        src = src_dir / name
        if not src.is_file():
            raise SystemExit("site_images: missing %s" % src)
        for w in widths:
            im = variant(src, w)
            dest = OUT / ("%s-%d.jpg" % (base, w))
            data = encode(im, QUALITY_FOR.get(dest.name, QUALITY))
            if write:
                OUT.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
            rows.append((dest.name, im.width, im.height, len(data)))
    return rows


def main(argv):
    ap = argparse.ArgumentParser(prog="site_images.py", description="Make the site's photo variants (metadata stripped).")
    ap.add_argument("src", help="the folder that holds the original photos")
    ap.add_argument("--list", action="store_true", help="print the table, write nothing")
    args = ap.parse_args(argv)
    rows = run(PHOTOS, pathlib.Path(args.src), not args.list)
    for name, w, h, n in rows:
        print("%-26s %5d x %-5d %8d B" % (name, w, h, n))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
