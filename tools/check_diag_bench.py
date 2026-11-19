"""check_diag_bench.py -- the release image carries no bench diagnostic, the bench image carries them.

Checks one build: the ELF's symbols (nm -C, our bench producers by name), the .bin's bytes (the bench keys' JSON
strings: a key left in a format string shows here) and, optionally, the undefined references of one object
(uxTaskGetSystemState from app_mqtt.cpp.o). --expect absent (the release cells) fails on any hit; --expect present
(the bench cells) fails on any miss. A missing file is a failure, never a pass (a negative check
that reads nothing would pass vacuously). Run: python -I tools/check_diag_bench.py --nm NM --elf ELF --bin BIN
[--obj APP_MQTT_O] --expect absent|present"""
import argparse
import os
import re
import subprocess
import sys

# our bench producers: static state and functions that exist only at FC_DIAG_BENCH=1
SYMBOLS = ("taskSnapshot", "gTaskFree", "gTickPartMax", "gStartPartMax", "gTickLockUsMax", "gBusyWin", "gHttpIdleHist",
           "gSendHeapMin", "otaHeapFeed", "otaHeapBoot", "fwdNoteHeapLow", "diagTaskIndex")
# the bench keys as they sit in the format strings (the C escapes resolve to plain quotes in the image)
KEYS = (b'"tick_parts"', b'"tick_lock_us_max"', b'"http_busy_ms_max"', b'"ota_heap"', b'"tasks":{', b'"gates":{',
        b'"http_idle_hist"', b'"http_idle_ms"', b'"fwd_heap_min"', b'"rec_in_max"', b'"replay":{',
        # one key of each further bench group, so a partial un-gating is caught too
        b'"tick_own_us_max"', b'"send_min"', b'"http_during"', b'"learners"', b'"worst_job"')
OBJ_REFS = ("uxTaskGetSystemState",)


def symbol_hits(nm_lines):
    """The bench symbols an `nm -C` listing defines (name at the end of a line, with or without a namespace)."""
    hits = set()
    for line in nm_lines:
        parts = line.split(None, 2)
        if len(parts) < 3 or parts[1] in ("U", "w", "v"):
            continue
        name = parts[2].split("(")[0].strip()
        for s in SYMBOLS:
            if re.search(r"(^|::)%s$" % re.escape(s), name):
                hits.add(s)
    return hits


def key_hits(data):
    return {k.decode() for k in KEYS if k in data}


def obj_hits(nm_lines):
    return {r for r in OBJ_REFS for line in nm_lines if line.split() and line.split()[-1] == r}


def run(nm, path, undefined=False):
    if not os.path.isfile(path):
        raise SystemExit("check_diag_bench: missing file %s (a check that reads nothing must not pass)" % path)
    args = [nm, "-u", path] if undefined else [nm, "-C", path]
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout.splitlines()


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--obj")
    ap.add_argument("--expect", choices=("absent", "present"), required=True)
    a = ap.parse_args(argv)
    if not os.path.isfile(a.bin):
        raise SystemExit("check_diag_bench: missing file %s" % a.bin)
    sym = symbol_hits(run(a.nm, a.elf))
    keys = key_hits(open(a.bin, "rb").read())
    refs = obj_hits(run(a.nm, a.obj, undefined=True)) if a.obj else set()
    found = sorted(sym | keys | refs)
    if a.expect == "absent":
        if found:
            print("::error::bench diagnostics in a release image: " + ", ".join(found))
            return 1
        print("release image: no bench symbol, key or reference (%d symbols, %d keys checked)" % (len(SYMBOLS), len(KEYS)))
        return 0
    # present: every key and the snapshot reference (symbols can be inlined away: at least the state ones must be there)
    missing = [k.decode() for k in KEYS if k.decode() not in keys]
    missing += [r for r in OBJ_REFS if a.obj and r not in refs]
    missing += [s for s in ("gTickPartMax", "gBusyWin", "gSendHeapMin", "gTaskFree") if s not in sym]
    if missing:
        print("::error::bench image misses: " + ", ".join(missing))
        return 1
    print("bench image: every bench key, the state symbols and the snapshot reference present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
