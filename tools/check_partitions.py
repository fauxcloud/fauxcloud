"""check_partitions.py -- the static partition-table check and the 1.x table hash gate. Stdlib only; run by CI and tools/build.ps1.

For both tables (firmware/fauxcloud/partitions.csv = the WROOM-32 profile, partitions-psram.csv = the PSRAM profile):
- the partitions are contiguous from nvs at 0x9000 to the end of flash at 0x400000;
- app partitions sit on 64-KB boundaries, data partitions on 4-KB boundaries; app0 and app1 have the same size;
- the app slot, the littlefs offset and its size (LFS_BLOCKS x 4096) match the profile's constants in src/ring.h
  (TABLE_APP, TABLE_LFS_OFF, LFS_BLOCKS), which the firmware's tableOk() checks on the board.
And the hash gate: partitions.csv (line endings normalised to LF) must match partitions.csv.sha256. The WROOM-32 table is
frozen for all of 1.x: a new table is a new board profile, never an edit of this file.

Usage: python -I tools/check_partitions.py [--sketch DIR]   (exit 0 = all good, 1 = a finding, printed)
"""
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKETCH = os.path.join(ROOT, "firmware", "fauxcloud")
FLASH_END = 0x400000


def parse_csv(path):
    rows = []
    with open(path, encoding="utf-8") as fh:
        lines = fh.readlines()
    for line in lines:
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(",")]
        rows.append({"name": f[0], "type": f[1], "sub": f[2], "off": int(f[3], 0), "size": int(f[4], 0)})
    return rows


def profile_constants():
    """ring.h: {'psram': (app, lfs_off, blocks), 'wroom': (...)} from the #if FC_PROFILE_PSRAM ... #else ... #endif block"""
    with open(os.path.join(SKETCH, "src", "ring.h"), encoding="utf-8") as fh:
        src = fh.read()
    # the #if FC_PROFILE_PSRAM block that carries TABLE_APP (ring.h has another one for RING_SLOTS)
    m = next((b for b in re.finditer(r"#if FC_PROFILE_PSRAM\s*\n(.*?)#else\s*\n(.*?)#endif", src, re.S)
              if "TABLE_APP" in b.group(1) and "TABLE_APP" in b.group(2)), None)
    if not m:
        raise SystemExit("check_partitions: the profile block of src/ring.h (TABLE_APP, TABLE_LFS_OFF, LFS_BLOCKS) not found")
    out = {}
    for name, body in (("psram", m.group(1)), ("wroom", m.group(2))):
        blocks = int(re.search(r"LFS_BLOCKS\s*=\s*(\d+)", body).group(1))
        app = int(re.search(r"TABLE_APP\s*=\s*(0x[0-9A-Fa-f]+)", body).group(1), 16)
        off = int(re.search(r"TABLE_LFS_OFF\s*=\s*(0x[0-9A-Fa-f]+)", body).group(1), 16)
        out[name] = (app, off, blocks)
    return out


def check_table(path, want):
    errs = []
    rows = parse_csv(path)
    name = os.path.basename(path)
    by = {r["name"]: r for r in rows}
    if not rows or rows[0]["off"] != 0x9000:
        errs.append(f"{name}: the first partition must start at 0x9000")
    for a, b in zip(rows, rows[1:]):
        if a["off"] + a["size"] != b["off"]:
            errs.append(f"{name}: {a['name']} ends at 0x{a['off'] + a['size']:X}, {b['name']} starts at 0x{b['off']:X} (not contiguous)")
    if rows and rows[-1]["off"] + rows[-1]["size"] != FLASH_END:
        errs.append(f"{name}: the table ends at 0x{rows[-1]['off'] + rows[-1]['size']:X}, not 0x{FLASH_END:X}")
    for r in rows:
        align = 0x10000 if r["type"] == "app" else 0x1000
        if r["off"] % align or r["size"] % align:
            errs.append(f"{name}: {r['name']} not on a {align // 1024}-KB boundary")
    app0, app1, fs = by.get("app0"), by.get("app1"), by.get("littlefs")
    if not app0 or not app1 or app0["size"] != app1["size"]:
        errs.append(f"{name}: app0 and app1 must exist and have the same size")
    app, off, blocks = want
    if app0 and app0["size"] != app:
        errs.append(f"{name}: app slot 0x{app0['size']:X}, the profile (ring.h TABLE_APP) wants 0x{app:X}")
    if not fs or fs["off"] != off or fs["size"] != blocks * 4096:
        errs.append(f"{name}: littlefs must be at 0x{off:X} with {blocks} x 4096 B (ring.h TABLE_LFS_OFF, LFS_BLOCKS)")
    if app0:
        warn = (app0["size"] * 95 + 99) // 100
        print(f"{name}: app slots 0x{app0['size']:X} = {app0['size']:,} B (95 % line {warn:,} B), littlefs "
              f"{fs['size'] // 1024 if fs else 0} KB = {fs['size'] // 4096 if fs else 0} blocks: {'ok' if not errs else 'FAIL'}")
    return errs


# the same hash as the pin file, a second copy here: a new table then changes two files a review sees, never one
FROZEN_SHA256 = "ab52c316ed3734c01fed5211789e94fbe470a6455006b8181aa00d7ed28b4ec4"


def table_hash(path):
    data = open(path, "rb").read().replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def main(argv=None):
    global SKETCH
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) == 2 and argv[0] == "--sketch":  # the tests: a copy of the sketch folder
        SKETCH = argv[1]
    want = profile_constants()
    errs = check_table(os.path.join(SKETCH, "partitions.csv"), want["wroom"])
    errs += check_table(os.path.join(SKETCH, "partitions-psram.csv"), want["psram"])
    pin_path = os.path.join(SKETCH, "partitions.csv.sha256")
    pin = ""
    if os.path.exists(pin_path):
        with open(pin_path, encoding="utf-8") as fh:
            words = fh.read().split()  # an empty pin file is a gate failure, not a crash
            pin = words[0] if words else ""
    now = table_hash(os.path.join(SKETCH, "partitions.csv"))
    if now != pin or pin != FROZEN_SHA256:
        errs.append("partitions.csv: the 1.x table is frozen: a new table is a new board profile "
                    f"(sha256 {now}; the pin in partitions.csv.sha256 {pin or 'empty'}; FROZEN_SHA256 in this script)")
    else:
        print("partitions.csv: the frozen 1.x table (hash gate ok)")
    for e in errs:
        print("::error::" + e if os.environ.get("GITHUB_ACTIONS") else "ERROR: " + e)
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
