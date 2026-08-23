#!/usr/bin/env python3
"""PADS PowerPCB ASCII -> IPC-D-356 netlist, for vendor packs that ship no nets.

ABT #858, in service of #810. Pre-X2 vendor Gerbers carry copper and nothing
else: no `%TO.N` net attributes, no IPC member. Faraday refuses them, because
without nets most of the analysis is meaningless. But the connectivity is
usually still IN the pack, in the layout database beside the Gerbers — it is
just in a format nobody downstream reads.

PADS `*ROUTE*` is a complete description:

    *SIGNAL* N16777546 2 -2
    E15.1                           U1.1
    37147500 107632500 4 457200 1280  THERMAL  TEARDROP N 90 90 L
    ...

— the signal name, the PIN PAIR the run connects, then a polyline of
(x, y, layer, width, flags) with vias named in the flags. That is exactly the
(net, refdes, pin, x, y) seed set IPC-D-356 exists to carry, and Faraday
already pairs an IPC-D-356 file with a plain Gerber set. So this is a
converter, not a new importer.

THE UNIT IS MEASURED, NOT ASSUMED. This is the whole difficulty and the reason
the script is not ten lines. PADS coordinates are in database counts whose
scale is not stated in the file, and the header constants do not settle it:
they fit `38100 counts = 1 mil` cleanly (TEXTSIZE 100 mil, USERGRID 5 mil,
REAL WIDTH 1 mil — all classic defaults), but the via drill diameters under
that scale match the drill file's tool list only in part, because PADS states
finished sizes and the drill file states drilled ones. A scale factor picked on
"probably" is invisible when wrong: the board still looks like a board and
every extracted parasitic is out by a constant nobody would ever see.

So the scale is DERIVED from the data and then CHECKED against different data:

  1. Candidate scales come from the trace widths. Every PADS segment width must
     equal some Gerber aperture diameter, because they are the same traces.
     Each (width, aperture) pair proposes a scale; the true one is proposed by
     many independent widths at once and the rest are coincidences.

  2. The winner is checked against the DRILLS, which the candidate scales had
     no part in choosing. Every PADS via must land on a hole in the Excellon
     file. That check also solves the CAM ORIGIN OFFSET, which no amount of
     arithmetic on header constants would ever have revealed — on the board
     this was written for it is exactly +1.000 inch in both axes.

  3. A residual above --max-residual-mil is a refusal, not a warning.

Usage:
    pads_to_ipc356.py LAYOUT.ASC --gerber-dir DIR [-o netlist.ipc]
"""
from __future__ import annotations

import argparse
import math
import re
import sys
from collections import Counter, defaultdict

# A PADS route record: x, y, layer, width, flags, then optional words
ROUTE_PT = re.compile(r"^\s*(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*(.*)$")
# "REFDES.PIN" pairs introducing a run. PADS pads the columns generously.
PIN_PAIR = re.compile(r"^\s*([A-Za-z_][\w/+.\-]*)\.(\w+)\s+([A-Za-z_][\w/+.\-]*)\.(\w+)\s*$")
VIA_FLAG = re.compile(r"\b(\w*VIA\w*|[\w/]*VIA[\w/]*)\b")
APERTURE = re.compile(r"%ADD(\d+)C,([0-9.]+)")
EXCELLON_XY = re.compile(r"^X([-+]?\d+)Y([-+]?\d+)")


def read_routes(path):
    """Every *SIGNAL* run: (net, pinA, pinB, [(x, y, layer, width, is_via)])."""
    runs, cur_net, cur_pins, pts = [], None, None, []
    in_route = False
    for raw in open(path, errors="replace"):
        line = raw.rstrip("\n")
        if line.startswith("*ROUTE*"):
            in_route = True
            continue
        if not in_route:
            continue
        if line.startswith("*") and not line.startswith(("*SIGNAL*", "*REMARK*")):
            break
        if line.startswith("*SIGNAL*"):
            if cur_pins and pts:
                runs.append((cur_net, *cur_pins, pts))
            parts = line.split()
            cur_net = parts[1] if len(parts) > 1 else None
            cur_pins, pts = None, []
            continue
        if line.startswith("*REMARK*"):
            continue
        m = PIN_PAIR.match(line)
        if m:
            if cur_pins and pts:
                runs.append((cur_net, *cur_pins, pts))
            cur_pins = (f"{m.group(1)}.{m.group(2)}", f"{m.group(3)}.{m.group(4)}")
            pts = []
            continue
        m = ROUTE_PT.match(line)
        if m:
            pts.append((int(m.group(1)), int(m.group(2)), int(m.group(3)),
                        int(m.group(4)), bool(VIA_FLAG.search(m.group(6) or ""))))
    if cur_pins and pts:
        runs.append((cur_net, *cur_pins, pts))
    return runs


def read_apertures(gerber_dir):
    """Circular aperture diameters, in whatever unit the file states."""
    import os
    out = []
    for name in sorted(os.listdir(gerber_dir)):
        p = os.path.join(gerber_dir, name)
        if not os.path.isfile(p):
            continue
        try:
            text = open(p, errors="replace").read(400_000)
        except OSError:
            continue
        if "%FS" not in text:
            continue
        unit = 1.0 if "%MOIN*%" in text else (1.0 / 25.4 if "%MOMM*%" in text else None)
        if unit is None:
            continue
        for _, d in APERTURE.findall(text):
            try:
                out.append(float(d) * unit)   # inches
            except ValueError:
                pass
    return sorted(set(out))


def read_drills(gerber_dir):
    """Excellon hole centres in inches. Format from the file's own header."""
    import os
    holes = []
    for name in sorted(os.listdir(gerber_dir)):
        p = os.path.join(gerber_dir, name)
        if not os.path.isfile(p):
            continue
        try:
            text = open(p, errors="replace").read()
        except OSError:
            continue
        if "M48" not in text[:200]:
            continue
        metric = "METRIC" in text[:400]
        nint, ndec = 2, 4
        fm = re.search(r"FILE_FORMAT\s*=\s*(\d+)\s*:\s*(\d+)", text)
        if fm:
            nint, ndec = int(fm.group(1)), int(fm.group(2))
        # LZ = leading zeros present, trailing suppressed; TZ is the reverse.
        trailing_suppressed = "LZ" in text[:400]
        for line in text.splitlines():
            m = EXCELLON_XY.match(line.strip())
            if not m:
                continue
            vals = []
            for tok in (m.group(1), m.group(2)):
                neg = tok.startswith("-")
                digits = tok.lstrip("+-")
                if trailing_suppressed:
                    digits = (digits + "0" * (nint + ndec))[: nint + ndec]
                else:
                    digits = ("0" * (nint + ndec) + digits)[-(nint + ndec):]
                v = int(digits) / (10 ** ndec)
                vals.append(-v if neg else v)
            x, y = vals
            if metric:
                x, y = x / 25.4, y / 25.4
            holes.append((x, y))
    return holes


def candidate_scales(widths, apertures, tol=2e-3):
    """Scales proposed by (PADS width, Gerber aperture) pairs, ranked by how
    many DISTINCT widths support each. The right scale is agreed on by several
    independent widths; a coincidence is agreed on by one."""
    votes = defaultdict(set)
    for w in widths:
        for a in apertures:
            if w <= 0:
                continue
            votes[round(a / w, 14)].add(w)
    merged = {}
    for s, ws in sorted(votes.items()):
        for m in merged:
            if abs(s - m) <= tol * m:
                merged[m] |= ws
                break
        else:
            merged[s] = set(ws)
    return sorted(merged.items(), key=lambda kv: (-len(kv[1]), kv[0]))


def fit_offset(via_pts, holes, scale):
    """Offset that puts the scaled vias on the drills, plus the residual.

    Coarse-align on the medians (robust to vias with no hole and holes with no
    via), then refine once on the nearest-neighbour matches."""
    if not via_pts or not holes:
        return None
    med = lambda v: sorted(v)[len(v) // 2]
    sx = [p[0] * scale for p in via_pts]
    sy = [p[1] * scale for p in via_pts]
    ox = med([h[0] for h in holes]) - med(sx)
    oy = med([h[1] for h in holes]) - med(sy)
    for _ in range(2):
        dxs, dys, res = [], [], []
        for x, y in zip(sx, sy):
            px, py = x + ox, y + oy
            hx, hy = min(holes, key=lambda h: (px - h[0]) ** 2 + (py - h[1]) ** 2)
            dxs.append(hx - px)
            dys.append(hy - py)
            res.append(math.hypot(hx - px, hy - py))
        ox += med(dxs)
        oy += med(dys)
    res.sort()
    return ox, oy, res[len(res) // 2], res[int(len(res) * 0.9)], res[-1]


def ipc_record(kind, net, ref, pin, x_um, y_um, access):
    """One IPC-D-356A body line, fixed columns as the reader expects them."""
    net = (net or "N/C")[:14]
    line = f"{kind}{net:<14}   {ref[:6]:<6}"
    line += f"-{pin[:4]:<4}" if pin else "-    "
    line += " " * 8
    line += f"A{access:02d}"
    line += f"X{x_um:+07d}".replace("+", " ", 1) if x_um >= 0 else f"X{x_um:07d}"
    line += f"Y{y_um:+07d}".replace("+", " ", 1) if y_um >= 0 else f"Y{y_um:07d}"
    return line + " S0"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("asc", help="PADS PowerPCB ASCII layout (.ASC)")
    ap.add_argument("--gerber-dir", required=True,
                    help="the Gerber + Excellon set the netlist must match")
    ap.add_argument("-o", "--out", help="IPC-D-356 output (default: stdout)")
    ap.add_argument("--max-residual-mil", type=float, default=0.5,
                    help="refuse if the median via-to-drill residual exceeds this")
    args = ap.parse_args()

    runs = read_routes(args.asc)
    if not runs:
        sys.exit("no *ROUTE* runs found — is this a PADS PowerPCB ASCII layout?")
    pts = [p for r in runs for p in r[3]]
    widths = sorted({p[3] for p in pts})
    vias = sorted({(p[0], p[1]) for p in pts if p[4]})
    print(f"routes: {len(runs)} runs, {len(pts)} points, {len(vias)} via sites, "
          f"{len(widths)} distinct widths", file=sys.stderr)

    apertures = read_apertures(args.gerber_dir)
    holes = read_drills(args.gerber_dir)
    print(f"gerber: {len(apertures)} circular apertures, {len(holes)} drill hits",
          file=sys.stderr)
    if not apertures or not holes:
        sys.exit("need both Gerber apertures and an Excellon file to anchor the scale")

    ranked = candidate_scales(widths, apertures)
    if not ranked:
        sys.exit("no scale relates any PADS width to any Gerber aperture")
    print("scale candidates (inch per count), by how many widths agree:", file=sys.stderr)
    for s, ws in ranked[:4]:
        print(f"   {s:.6e}  {len(ws)}/{len(widths)} widths", file=sys.stderr)

    chosen = None
    for s, ws in ranked[:6]:
        fit = fit_offset(vias, holes, s)
        if not fit:
            continue
        ox, oy, med, p90, mx = fit
        print(f"   check {s:.6e}: offset ({ox:+.4f}, {oy:+.4f}) in, residual "
              f"median {med*1000:.4f} mil, p90 {p90*1000:.4f}, max {mx*1000:.4f}",
              file=sys.stderr)
        if med * 1000 <= args.max_residual_mil:
            chosen = (s, ox, oy, med, len(ws))
            break
    if not chosen:
        sys.exit(f"no candidate scale puts the vias on the drills within "
                 f"{args.max_residual_mil} mil — refusing to emit a netlist at a "
                 f"scale nobody verified. (A wrong scale is invisible: the board "
                 f"still looks like a board.)")
    scale, ox, oy, med, nwidths = chosen
    print(f"ANCHORED: 1 count = {scale:.6e} inch (agreed by {nwidths}/{len(widths)} "
          f"trace widths), CAM origin offset ({ox:+.4f}, {oy:+.4f}) inch, "
          f"median via-to-drill residual {med*1000:.4f} mil", file=sys.stderr)

    def to_um(x, y):
        return (round((x * scale + ox) * 25400), round((y * scale + oy) * 25400))

    lines = ["C  IPC-D-356 netlist generated by Faraday scripts/pads_to_ipc356.py",
             f"C  source: {args.asc}",
             f"C  1 PADS count = {scale:.6e} inch; CAM origin offset "
             f"({ox:+.4f}, {oy:+.4f}) inch",
             f"C  anchored on {len(vias)} vias against {len(holes)} drill hits, "
             f"median residual {med*1000:.4f} mil",
             "P  JOB PADS ROUTE EXTRACT",
             "P  UNITS CUST 1",
             "P  VER IPC-D-356A"]
    seen = set()
    seeds = 0
    for net, pin_a, pin_b, points in runs:
        if not points:
            continue
        # The run's endpoints ARE its two pins: PADS writes the polyline from
        # the first pin to the second. Interior points are corners and vias.
        for pin, pt in ((pin_a, points[0]), (pin_b, points[-1])):
            ref, _, pn = pin.partition(".")
            x, y = to_um(pt[0], pt[1])
            key = (ref, pn, x, y)
            if key in seen:
                continue
            seen.add(key)
            # access layer: PADS layer 1 is the top copper; the terminator 65
            # is not a layer, so fall back to "both sides" rather than invent
            # one.
            layer = pt[2] if 1 <= pt[2] <= 32 else 0
            lines.append(ipc_record("327", net, ref, pn, x, y, layer))
            seeds += 1
        for p in points:
            if not p[4]:
                continue
            x, y = to_um(p[0], p[1])
            key = ("VIA", "", x, y)
            if key in seen:
                continue
            seen.add(key)
            lines.append(ipc_record("317", net, "VIA", "", x, y, 0))
            seeds += 1
    lines.append("999")
    text = "\n".join(lines) + "\n"
    if args.out:
        open(args.out, "w").write(text)
        print(f"wrote {args.out}: {seeds} seed records", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
