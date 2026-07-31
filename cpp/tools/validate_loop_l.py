#!/usr/bin/env python3
"""Tier-2 validation (ABT #421): the commutation loop's inductance estimate
vs FastHenry, on known rectangles AND on the real hulls Faraday derived.

The estimate under test is Screener::hull_loop_inductance_nh — the
equivalent-rectangle Grover form with strip GMD r = 0.2235(w+t). This script
re-implements it verbatim (kept in sync BY HAND; the C++ test pins one shared
value so drift is caught), generates a FastHenry deck for the same geometry,
and reports the error band.

Usage:
  validate_loop_l.py --fasthenry /path/to/fasthenry [report.json ...]
With report JSONs, every commutation-loop finding's hull is validated too.
"""
import argparse, json, math, os, re, subprocess, sys, tempfile

MU0_PI = 0.4  # nH/mm


def grover_rect_nh(a, b, w, t=0.035):
    r = 0.2235 * (w + t)
    d = math.hypot(a, b)
    return MU0_PI * (a * math.log(2 * a * b / (r * (a + d))) +
                     b * math.log(2 * a * b / (r * (b + d))) + 2 * d -
                     2 * (a + b))


def estimate_from_hull(hull, w):
    per = 0.0
    area2 = 0.0
    n = len(hull)
    for i in range(n):
        p, q = hull[i], hull[(i + 1) % n]
        per += math.hypot(q[0] - p[0], q[1] - p[1])
        area2 += p[0] * q[1] - q[0] * p[1]
    A = abs(area2) / 2
    s = per / 4
    disc = max(0.0, s * s - A)
    a = s + math.sqrt(disc)
    b = max(A / a, 0.01)
    return grover_rect_nh(a, b, w), a, b


def fasthenry_loop_nh(fh, pts, w, t=0.035, f_hz=1e6):
    """Closed polygon loop (mm), one tiny port gap at the first vertex."""
    lines = ["* faraday tier-2 validation deck", ".units mm",
             f".default sigma=58000.0 nwinc=5 nhinc=3"]
    n = len(pts)
    # nodes: N0 at first vertex (port side A), then around, NEND back near N0
    for i, (x, y) in enumerate(pts):
        lines.append(f"N{i} x={x:.4f} y={y:.4f} z=0")
    gx = pts[0][0] + 0.05 * (pts[1][0] - pts[0][0]) / max(
        1e-9, math.hypot(pts[1][0] - pts[0][0], pts[1][1] - pts[0][1]))
    gy = pts[0][1] + 0.05 * (pts[1][1] - pts[0][1]) / max(
        1e-9, math.hypot(pts[1][0] - pts[0][0], pts[1][1] - pts[0][1]))
    lines.append(f"NP x={gx:.4f} y={gy:.4f} z=0")   # port node, 50um from N0
    seg = []
    for i in range(1, n):
        seg.append((f"N{i - 1 if i > 1 else 1}", None))
    # segments: NP->N1->N2->...->N{n-1}->N0 ; port across N0-NP
    lines.append(f"EP NP N1 w={w:.3f} h={t:.3f}")
    for i in range(1, n - 1):
        lines.append(f"E{i} N{i} N{i + 1} w={w:.3f} h={t:.3f}")
    lines.append(f"EL N{n - 1} N0 w={w:.3f} h={t:.3f}")
    lines.append(".external N0 NP")
    lines.append(f".freq fmin={f_hz:.0f} fmax={f_hz:.0f} ndec=1")
    lines.append(".end")
    with tempfile.TemporaryDirectory() as td:
        deck = os.path.join(td, "loop.inp")
        open(deck, "w").write("\n".join(lines) + "\n")
        r = subprocess.run([fh, deck], cwd=td, capture_output=True, text=True)
        zc = os.path.join(td, "Zc.mat")
        if not os.path.exists(zc):
            raise RuntimeError("fasthenry produced no Zc.mat:\n" +
                               r.stdout[-800:] + r.stderr[-400:])
        txt = open(zc).read()
        # last row: "Re+Imj" single element
        m = re.findall(r"([-+eE.\d]+)\s*([-+][eE.\d]+)j", txt)
        if not m:
            raise RuntimeError("cannot parse Zc.mat:\n" + txt)
        im = float(m[-1][1])
        return im / (2 * math.pi * f_hz) * 1e9


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fasthenry", required=True)
    ap.add_argument("reports", nargs="*")
    args = ap.parse_args()

    rows = []
    print(f"{'case':<28} {'est nH':>8} {'FH nH':>8} {'err %':>7}")
    print("-" * 56)
    # known rectangles across the span real hot loops occupy
    for a, b, w in [(10, 10, 1.0), (20, 10, 1.0), (30, 5, 0.5),
                    (50, 20, 2.0), (15, 8, 0.3), (8, 4, 1.5)]:
        pts = [(0, 0), (a, 0), (a, b), (0, b)]
        est = grover_rect_nh(a, b, w)
        fh = fasthenry_loop_nh(args.fasthenry, pts, w)
        err = 100 * (est - fh) / fh
        rows.append(err)
        print(f"{'rect %gx%g w=%g' % (a, b, w):<28} {est:8.1f} {fh:8.1f} {err:7.1f}")

    for rep_path in args.reports:
        rep = json.load(open(rep_path))
        name = os.path.basename(rep_path).replace(".json", "")
        for f in rep.get("findings", []):
            if f.get("rule") != "commutation-loop":
                continue
            hull = [(l["x1"], l["y1"]) for l in f.get("geom", {}).get("lines", [])]
            if len(hull) < 3:
                continue
            emit = f.get("emit", {})
            est = emit.get("loopNh", 0)
            # width backed out is not in the report; use 1 mm as the deck's
            # width too so BOTH sides see the same conductor
            est2, _, _ = estimate_from_hull(hull, 1.0)
            fh = fasthenry_loop_nh(args.fasthenry, hull, 1.0)
            err = 100 * (est2 - fh) / fh
            rows.append(err)
            print(f"{(name + ':' + emit.get('net', '?'))[:28]:<28} "
                  f"{est2:8.1f} {fh:8.1f} {err:7.1f}   (report loopNh={est})")

    if rows:
        mx = max(abs(e) for e in rows)
        print("-" * 56)
        print(f"worst |err| {mx:.1f}%  over {len(rows)} cases")
        sys.exit(0 if mx <= 25.0 else 1)


if __name__ == "__main__":
    main()
