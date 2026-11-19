"""tools/check_rtc_layout.py -- the RTC_NOINIT words survive a software restart, so an image after an
OTA reads what the image before it wrote -- at the address it was built with. A word that moves breaks that silently (it
happened once: the clock word moved by 16 B). This check compares the built ELF's .rtc_noinit symbols with the committed list
tools/rtc_layout.txt and fails when:
  - the section start moved;
  - a listed symbol is missing, moved, shrank, or grew without its line updated (growth at the end is allowed -- the
    struct is append-only -- but the committed list always holds the built size);
  - a new symbol sits inside the listed span (new words go after the last listed one, or into the struct's end).

Usage (build.ps1 runs it after every compile; CI too):
  python -I tools/check_rtc_layout.py --nm <xtensa-esp32-elf-nm> --elf <fauxcloud.ino.elf> [--layout tools/rtc_layout.txt]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def parse_nm(text):
    """nm -S -C lines -> (start, end, {symbol: (address, size)}) of the .rtc_noinit section."""
    syms, start, end = {}, None, None
    for line in text.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 3 and parts[2] == "_rtc_noinit_start":
            start = int(parts[0], 16)
        elif len(parts) == 3 and parts[2] == "_rtc_noinit_end":
            end = int(parts[0], 16)
        elif len(parts) == 4 and parts[2] in ("b", "B", "d", "D"):
            syms[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
    if start is None or end is None:
        raise ValueError("no _rtc_noinit_start / _rtc_noinit_end in the nm output")
    return start, end, {k: v for k, v in syms.items() if start <= v[0] < end}


def parse_layout(text):
    """'start 0x50000200' + 'symbol offset size' lines (# comments) -> (start, {symbol: (offset, size)})."""
    start, words = None, {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        f = line.split()
        if f[0] == "start":
            start = int(f[1], 0)
        else:
            words[f[0]] = (int(f[1], 0), int(f[2], 0))
    return start, words


def check(nm_text, layout_text):
    """[] when the layout holds, else the problems."""
    start, _, syms = parse_nm(nm_text)
    want_start, words = parse_layout(layout_text)
    bad = []
    if start != want_start:
        bad.append("section start 0x%x, the layout says 0x%x" % (start, want_start))
    span = 0
    for name, (off, size) in words.items():
        span = max(span, off + size)
        if name not in syms:
            bad.append("%s missing" % name)
            continue
        addr, got = syms[name]
        if addr - start != off:
            bad.append("%s moved: offset %d, the layout says %d" % (name, addr - start, off))
        if got < size:
            bad.append("%s shrank: %d B, the layout says %d" % (name, got, size))
        elif got > size:
            bad.append("%s grew: %d B, the layout says %d (record the new size in rtc_layout.txt)" % (name, got, size))
    for name, (addr, size) in syms.items():
        if name not in words and addr - start < span:
            bad.append("%s (new) sits inside the listed words (offset %d < %d): new RTC words are appended" %
                       (name, addr - start, span))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--layout", default=os.path.join(HERE, "rtc_layout.txt"))
    a = ap.parse_args()
    out = subprocess.run([a.nm, "-S", "-C", a.elf], capture_output=True, text=True, check=True).stdout
    with open(a.layout, encoding="utf-8") as f:
        bad = check(out, f.read())
    start, end, syms = parse_nm(out)
    print(">> RTC layout: .rtc_noinit 0x%x..0x%x, %s" % (start, end, ", ".join(
        "%s @+%d (%d B)" % (k, v[0] - start, v[1]) for k, v in sorted(syms.items(), key=lambda kv: kv[1][0]))))
    if bad:
        print("RTC layout check FAILED: " + "; ".join(bad))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
