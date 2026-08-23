#!/usr/bin/env python3
"""Digitise a spectrum-analyser screenshot into (frequency, level) traces.

Written for ABT #810, whose problem is that published conducted-emission data
is a picture. Vendors ship rendered figures — no CSV anywhere, on Zenodo or
IEEE DataPort or a manufacturer's site — so if the pipeline is to be compared
against a real measurement at all, the picture has to be read.

Reading a picture is normally where a validation quietly stops being one, so
this script refuses to hand back numbers it cannot corroborate. Two independent
checks are built in, and both come from the screenshot itself:

  * THE MARKER. The instrument printed a frequency and a level in the header.
    Digitise the trace, read it at the marker's frequency, and the two must
    agree. That checks the axis mapping end to end, in one number, against the
    instrument rather than against my arithmetic.

  * THE LIMIT LINES. A compliance capture has the standard's limit drawn on it.
    Those are known constants — Faraday implements the same table in
    Emissions.hpp — so recovering them from the red pixels checks the mapping
    a second time, at several levels and frequencies at once, AND checks
    Faraday's own limit table against what an accredited lab's receiver
    actually drew.

If the marker disagrees, the digitisation is wrong and the traces are worthless.
That is the point of doing it this way rather than eyeballing gridlines.

Usage:
    digitize_ce_screenshot.py IMAGE --calib f1=x1 f2=x2 --ref-level DBUV \\
        [--db-per-div 10] [--flip vertical] [--out traces.csv]

The two --calib points are pixel columns of two labelled frequency gridlines
(e.g. the 1 MHz and 10 MHz lines); the x axis is assumed logarithmic between
them, which is what every conducted sweep 150 kHz-30 MHz uses. --ref-level is
the instrument's Ref Lvl, which sits on the TOP graticule line.
"""
from __future__ import annotations

import argparse
import csv
import math
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:  # pragma: no cover - dependency message, not logic
    sys.exit("needs numpy and pillow: pip install numpy pillow")


# Trace colours as R&S / Keysight receivers draw them. Generous thresholds:
# the screenshots are PNG-in-PDF and have been through at least one resample,
# so pure #FFFF00 is not what survives.
COLOURS = {
    "peak": lambda r, g, b: (r > 150) & (g > 150) & (b < 120),      # yellow
    "average": lambda r, g, b: (r < 150) & (g > 150) & (b > 150),   # cyan
}
LIMIT = lambda r, g, b: (r > 150) & (g < 110) & (b < 110)           # noqa: E731  red


def find_graticule(dark: "np.ndarray") -> tuple[int, int, int, int]:
    """Bounding box of the plot frame: the longest full-width/height dark lines."""
    h, w = dark.shape
    rows = [i for i in range(h) if dark[i].sum() > 0.6 * w]
    cols = [j for j in range(w) if dark[:, j].sum() > 0.6 * h]
    if len(rows) < 2 or len(cols) < 2:
        raise SystemExit("could not find the plot frame — is this a graticule screenshot?")
    return cols[0], cols[-1], rows[0], rows[-1]


def read_traces(a, box, f_of_x, db_of_y):
    """Topmost coloured pixel per column. Topmost, not centroid, because a
    spectrum trace is an envelope: the peak of the pen IS the reading, and
    averaging down the stroke would shave a decibel or two off every point in
    the same direction — a bias, not noise."""
    x0, x1, y0, y1 = box
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    inside = np.zeros(a.shape[:2], bool)
    inside[y0 + 1:y1, x0 + 1:x1] = True
    out = {}
    for name, test in COLOURS.items():
        mask = test(r, g, b) & inside
        pts = []
        for x in range(x0 + 1, x1):
            ys = np.nonzero(mask[:, x])[0]
            if ys.size:
                pts.append((f_of_x(x), db_of_y(ys.min())))
        out[name] = pts
    return out


def read_limit_segments(a, box, f_of_x, db_of_y, min_px=60):
    """Long horizontal red runs = the standard's limit lines.

    min_px is deliberately blunt. Red is also the colour of the limit LABELS
    ("MW_PK5"), and letters produce short runs at every level they happen to
    cross; demanding a long run keeps the lines and drops the text. Narrow
    bands (CISPR 25's SW and CB are 0.3 and 2 MHz wide) are shorter than that
    on a log axis, so they are reported separately at a lower threshold and
    marked as such rather than silently mixed in with the confident ones.
    """
    x0, x1, y0, y1 = box
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    inside = np.zeros(a.shape[:2], bool)
    inside[y0 + 1:y1, x0 + 1:x1] = True
    red = LIMIT(r, g, b) & inside
    segs = []
    for y in range(y0 + 1, y1):
        xs = np.nonzero(red[y])[0]
        if xs.size < 8:
            continue
        runs, start, prev = [], xs[0], xs[0]
        for x in xs[1:]:
            if x - prev > 4:
                runs.append((start, prev))
                start = x
            prev = x
        runs.append((start, prev))
        for lo, hi in runs:
            if hi - lo >= 8:
                segs.append({"level_dbuv": round(db_of_y(y), 2),
                             "f_lo_hz": f_of_x(lo), "f_hi_hz": f_of_x(hi),
                             "px": int(hi - lo), "confident": (hi - lo) >= min_px})
    return segs


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("image")
    p.add_argument("--calib", nargs=2, required=True, metavar="F=X",
                   help="two 'frequency_hz=pixel_column' anchors on labelled gridlines")
    p.add_argument("--ref-level", type=float, required=True,
                   help="instrument Ref Lvl in dBuV — the TOP graticule line")
    p.add_argument("--db-per-div", type=float, default=10.0)
    p.add_argument("--divs", type=int, default=10)
    p.add_argument("--flip", choices=["none", "vertical", "horizontal", "rotate180"],
                   default="none", help="pdfimages often extracts the raster untransformed")
    p.add_argument("--marker", nargs=2, type=float, metavar=("F_HZ", "DBUV"),
                   help="the instrument's own marker readout — the primary check")
    p.add_argument("--marker-trace", default="peak")
    p.add_argument("--tolerance-db", type=float, default=1.0)
    p.add_argument("--out", help="write the traces as CSV")
    args = p.parse_args()

    im = Image.open(args.image).convert("RGB")
    if args.flip == "vertical":
        im = im.transpose(Image.FLIP_TOP_BOTTOM)
    elif args.flip == "horizontal":
        im = im.transpose(Image.FLIP_LEFT_RIGHT)
    elif args.flip == "rotate180":
        im = im.rotate(180)
    a = np.asarray(im).astype(int)

    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    dark = (r < 100) & (g < 100) & (b < 100)
    box = find_graticule(dark)
    x0, x1, y0, y1 = box
    print(f"frame x {x0}..{x1}  y {y0}..{y1}")

    (f1, xa), (f2, xb) = [(float(s.split("=")[0]), float(s.split("=")[1])) for s in args.calib]
    px_per_decade = (xb - xa) / math.log10(f2 / f1)

    def f_of_x(x):
        return f1 * 10 ** ((x - xa) / px_per_decade)

    span = args.db_per_div * args.divs
    px_per_db = (y1 - y0) / span

    def db_of_y(y):
        return args.ref_level - (y - y0) / px_per_db

    print(f"x: {px_per_decade:.1f} px/decade   span {f_of_x(x0)/1e6:.4g}"
          f"-{f_of_x(x1)/1e6:.4g} MHz")
    print(f"y: {args.ref_level:g} dBuV at top, {args.ref_level - span:g} at bottom")

    traces = read_traces(a, box, f_of_x, db_of_y)
    for name, pts in traces.items():
        print(f"{name}: {len(pts)} points")

    ok = True
    if args.marker:
        f_m, db_m = args.marker
        pts = traces[args.marker_trace]
        if not pts:
            sys.exit(f"no {args.marker_trace} trace found — cannot check the marker")
        got = min(pts, key=lambda q: abs(math.log10(q[0] / f_m)))[1]
        err = abs(got - db_m)
        verdict = "OK" if err <= args.tolerance_db else "FAILED"
        print(f"marker check [{verdict}]: instrument {db_m:g} dBuV at {f_m/1e6:g} MHz, "
              f"digitised {got:.2f} ({err:.2f} dB)")
        ok = err <= args.tolerance_db

    print("limit segments (confident):")
    for s in read_limit_segments(a, box, f_of_x, db_of_y):
        if s["confident"]:
            print(f"  {s['level_dbuv']:7.2f} dBuV  {s['f_lo_hz']/1e6:9.4f}"
                  f" - {s['f_hi_hz']/1e6:9.4f} MHz")

    if args.out:
        with open(args.out, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["frequency_hz", "detector", "level_dbuv"])
            for name, pts in traces.items():
                for f, db in pts:
                    w.writerow([f"{f:.6g}", name, f"{db:.2f}"])
        print(f"wrote {args.out}")

    # A failed marker check is an error, not a warning. The whole reason this
    # script exists is that "read off a picture" is otherwise unfalsifiable.
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
