"""tools/mslp_normals.py -- generates firmware/fauxcloud/src/mslp_norm.h, L7's built-in long-term mean
sea-level pressure (learn.cpp learnMslpNormal), from NOAA PSL's NCEP/NCAR Reanalysis 1 long-term monthly means
1991-2020 (`slp.mon.ltm.1991-2020.nc`, public domain; https://psl.noaa.gov/data/gridded/data.ncep.reanalysis.derived.html).

No network: the source files are fetched once by hand and passed on the command
line. The table: two seasons (DJF = Dec, Jan, Feb; JJA = Jun, Jul, Aug) x latitude -60 .. 75 every 15 deg (10 rows) x
longitude 0 .. 340 E every 20 deg (18 columns), each cell the mean of the reanalysis grid points within +-7.5 deg lat /
+-10 deg lon (cos-lat weighted), stored as int8 hPa - 1013 (clamped to -127 .. 127). 360 B.

-128 = no normal, for a cell whose mean surface height (the same box, cos-lat weighted) is over
1 000 m: sea-level pressure there is an extrapolation, and a console set to the altimeter setting (common in the US)
differs from it by up to ~10 hPa, so L7 must not judge it (learnMslpNormal drops a -128 corner from the bilinear
weights and returns false when the rest weigh less than half).
The surface height is the same reanalysis' `hgt.sfc.nc` (NCEP/NCAR R1 "surface" set, variable `hgt` in m on the same
2.5-deg grid; https://downloads.psl.noaa.gov/Datasets/ncep.reanalysis/surface/hgt.sfc.nc).

The box mean dilutes the Andes, the East African and Ethiopian highlands, the Mexican plateau:
a town there sets its console to the local QNH, which differs from the reduced sea-level pressure by up to ~15 hPa. So
the same file also gives a 5-deg high-ground bitmap (lat -60 .. 80 x lon 0 .. 355 E = 28 x 72 bits = 252 B, bit
row * 72 + col, LSB first): set where any 2.5-deg surface point of the box (its edges included: 9 points) is over
1 000 m. L7 is silent where the station's own 5-deg box is set (learnMslpHighAt).

Usage:  python -I tools/mslp_normals.py slp.mon.ltm.1991-2020.nc --orog hgt.sfc.nc [--out firmware/fauxcloud/src/mslp_norm.h]
Needs:  numpy and netCDF4. PSL serves the files as NetCDF-4 (HDF5 magic), which only netCDF4 reads (scipy's
        netcdf_file reads NetCDF-3 classic only, so there is no fallback). A one-off run, not part of CI, so the two are
        not pinned in tools/requirements.txt: install them beside this script, where `python -I` finds them (the same
        place as zopfli, gitignored):
            python -m pip install --only-binary :all: --target tools/.pylib numpy netCDF4
        (tested with numpy 2.5.4, netCDF4 1.7.5). The output is deterministic for given files: fixed grid, float64
        means in a fixed order, round-half-even to whole hPa; the header names only the files' basenames.
"""
import argparse
import os
import sys

_PYLIB = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".pylib")
if os.path.isdir(_PYLIB) and _PYLIB not in sys.path:
    sys.path.append(_PYLIB)  # after the stdlib, as gen_web_assets.py

ROWS = [-60 + 15 * i for i in range(10)]
COLS = [20 * j for j in range(18)]
SEASONS = (("DJF", (11, 0, 1)), ("JJA", (5, 6, 7)))  # month indices 0..11
ORO_MAX_M = 1000.0  # mean surface height above which a cell has no normal
HIGH_ROWS, HIGH_COLS = 28, 72  # 5-deg boxes, lat -60 .. 80, lon 0 .. 355 E
NONE = -128
HDF5_MAGIC = bytes([0x89, 0x48, 0x44, 0x46])


def load(path, var):
    try:
        import numpy as np
        from netCDF4 import Dataset
    except ImportError as e:
        raise SystemExit("mslp_normals: %s. Install: python -m pip install --only-binary :all: --target tools/.pylib "
                         "numpy netCDF4" % e)
    with open(path, "rb") as f:
        if f.read(4) != HDF5_MAGIC:
            print("mslp_normals: %s is not NetCDF-4/HDF5; netCDF4 reads NetCDF-3 too, going on" % path)
    with Dataset(path) as ds:
        ds.set_auto_maskandscale(True)  # packed shorts -> physical units through scale_factor / add_offset
        v = np.asarray(ds[var][:], dtype="float64")
        lat, lon = np.asarray(ds["lat"][:], dtype="float64"), np.asarray(ds["lon"][:], dtype="float64")
    return v, lat, lon


def box_mean(field, lat, lon, la, lo):
    """The cos-lat weighted mean of `field` (lat, lon) over the grid points within +-7.5 deg lat / +-10 deg lon."""
    import numpy as np
    mlat = np.abs(lat - la) <= 7.5
    dlon = np.abs(((lon - lo) + 180.0) % 360.0 - 180.0)
    mlon = dlon <= 10.0
    sub = field[np.ix_(mlat, mlon)]
    w = np.cos(np.radians(lat[mlat]))[:, None] * np.ones((1, mlon.sum()))
    return float((sub * w).sum() / w.sum())


def high_bits(oro, lat, lon):
    """The 5-deg high-ground bitmap: any 2.5-deg surface point of the box (edges included) over ORO_MAX_M."""
    import numpy as np
    bits = bytearray(HIGH_ROWS * HIGH_COLS // 8 + (1 if HIGH_ROWS * HIGH_COLS % 8 else 0))
    for r in range(HIGH_ROWS):
        la0 = -60.0 + 5.0 * r
        mlat = (lat >= la0 - 1e-6) & (lat <= la0 + 5.0 + 1e-6)
        for c in range(HIGH_COLS):
            lo0 = 5.0 * c
            mlon = ((lon - lo0) % 360.0) <= 5.0 + 1e-6
            sub = oro[np.ix_(mlat, mlon)]
            if sub.size and float(sub.max()) > ORO_MAX_M:
                i = r * HIGH_COLS + c
                bits[i // 8] |= 1 << (i % 8)
    return bits


def table(slp, oro, lat, lon):
    high = [[box_mean(oro, lat, lon, la, lo) > ORO_MAX_M for lo in COLS] for la in ROWS]
    out = []
    for _, months in SEASONS:
        season = slp[list(months)].mean(axis=0)  # hPa (lat, lon)
        rows = []
        for r, la in enumerate(ROWS):
            row = []
            for c, lo in enumerate(COLS):
                v = box_mean(season, lat, lon, la, lo)
                row.append(NONE if high[r][c] else max(-127, min(127, int(round(v - 1013.0)))))
            rows.append(row)
        out.append(rows)
    return out, high


def render(t, src, oro, bits):
    lines = ["// mslp_norm.h -- the built-in long-term mean sea-level pressure for L7 (learn.cpp, learnMslpNormal).",
             "// GENERATED by tools/mslp_normals.py from %s and %s (NOAA PSL NCEP/NCAR" % (os.path.basename(src),
                                                                                       os.path.basename(oro)),
             "// Reanalysis 1: long-term monthly means 1991-2020, the surface height; public domain). Do not edit by hand.",
             "// Included once, by learn.cpp. [season DJF, JJA][lat -60 .. 75 every 15][lon 0 .. 340 E every 20], int8",
             "// hPa - 1013; -128 = none (mean surface height over 1 000 m).",
             "#define MSLP_NORM_PRESENT 1",
             "static const int8_t MSLP_NORM[2 * 10 * 18] = {"]
    for (name, _), rows in zip(SEASONS, t):
        lines.append("    // %s" % name)
        for la, row in zip(ROWS, rows):
            lines.append("    " + ", ".join("%4d" % v for v in row) + ",  // %+d" % la)
    lines.append("};")
    lines.append("// the 5-deg high-ground bitmap [lat -60 .. 80 every 5][lon 0 .. 355 E every 5], bit row * 72 + col,")
    lines.append("// LSB first: a 2.5-deg surface point of the box over 1 000 m")
    lines.append("#define MSLP_HIGH_PRESENT 1")
    lines.append("static const uint8_t MSLP_HIGH[%d] = {" % len(bits))
    for i in range(0, len(bits), 18):
        lines.append("    " + ", ".join("0x%02x" % b for b in bits[i:i + 18]) + ",")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--orog", required=True, help="hgt.sfc.nc of the same reanalysis (for the high-ground mask)")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "firmware", "fauxcloud", "src", "mslp_norm.h"))
    a = ap.parse_args()
    import numpy as np
    slp, lat, lon = load(a.src, "slp")
    if slp.shape[0] != 12:
        raise SystemExit("expected 12 monthly means, got shape %s" % (slp.shape,))
    oro, olat, olon = load(a.orog, "hgt")
    oro = oro.reshape(oro.shape[-2:])  # (time = 1, lat, lon) -> (lat, lon)
    if oro.shape != slp.shape[1:] or not np.allclose(olat, lat) or not np.allclose(olon, lon):
        raise SystemExit("the surface height is not on the pressure's grid (%s vs %s)" % (oro.shape, slp.shape[1:]))
    t, high = table(slp, oro, lat, lon)
    bits = high_bits(oro, lat, lon)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(t, a.src, a.orog, bits))
    print("wrote %s (%d B of table)" % (a.out, 2 * len(ROWS) * len(COLS)))
    masked = ["%+d/%dE" % (ROWS[r], COLS[c]) for r in range(len(ROWS)) for c in range(len(COLS)) if high[r][c]]
    print("  no normal (mean surface height > %d m): %d cells: %s" % (ORO_MAX_M, len(masked), ", ".join(masked) or "none"))
    print("  high-ground 5-deg boxes: %d of %d" % (sum(bin(b).count("1") for b in bits), HIGH_ROWS * HIGH_COLS))
    for name, la, lo in (("Bogota", 4.6, -74.1), ("Nairobi", -1.3, 36.8), ("Addis Ababa", 9.0, 38.7), ("Mexico City", 19.4, -99.1),
                         ("Denver", 39.7, -105.0), ("Wichita", 37.7, -97.3), ("Warsaw", 52.2, 21.0), ("Reykjavik", 64.1, -21.9)):
        r, c = int((la + 60) // 5), int((lo % 360) // 5)
        i = r * HIGH_COLS + c
        print("  %-12s 5-deg box %s" % (name, "high (L7 silent)" if bits[i // 8] >> (i % 8) & 1 else "low"))
    # sanity: a few places the host test checks (learnMslpNormal's ranges)
    for name, la, lo, s in (("Warsaw Jan", 52.2, 21.0, 0), ("Reykjavik Jan", 64.1, -21.9, 0), ("Irkutsk Jan", 52.3, 104.3, 0)):
        r, c = min(range(10), key=lambda i: abs(ROWS[i] - la)), int(round((lo % 360) / 20)) % 18
        v = t[s][r][c]
        print("  %-14s ~ %s (nearest cell)" % (name, "none (high ground)" if v == NONE else "%d hPa" % (1013 + v)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
