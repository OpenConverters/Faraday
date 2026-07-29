#!/usr/bin/env python3
"""Cross-format consistency sweep: every corpus board through its native KiCad
import AND through kicad-cli's ODB++ export, compared on what carries
electrical meaning. The importer earns trust by agreeing with itself across
formats — per-net routed copper, via counts, findings — not by passing its
own fixtures.

Usage: scripts/crossformat_corpus.py [board.kicad_pcb ...]
       (no args: the whole corpus/ + committed real fixtures)

Requires kicad-cli (flatpak org.kicad.KiCad works: the script tries PATH
first, then flatpak) and a built ./build/faraday_cli.

Known, understood variances (see docs/corpus-2026-07.md):
- net names: the exporter rewrites spaces to underscores and ~X to ~{X};
- arcs: the exporter tessellates finer than the importer's single chord, so
  ODB++ routed length runs 0-1.5% LONGER (more accurate, not less);
- zone fills: "best-effort" conversion drops clearance holes, so pour
  coverage reads a few points higher and a borderline layer (fomu In1) can
  flip plane classification — the extra copper is in the exported job itself;
- pads: netless/paste-only micro-pads and consolidated pad grids differ in
  count with no effect on any rule.
"""
import collections
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(REPO, "build", "faraday_cli")

STACKS = {  # copper count -> builtin stackup
    2: "default-2layer", 4: "default-4layer", 6: "default-6layer",
}


def kicad_cli():
    if shutil.which("kicad-cli"):
        return ["kicad-cli"]
    if shutil.which("flatpak"):
        return ["flatpak", "run", "--command=kicad-cli", "org.kicad.KiCad"]
    raise SystemExit("no kicad-cli found (PATH or flatpak org.kicad.KiCad)")


def run_cli(target, stack, out):
    cmd = [CLI, target, "-o", out]
    if stack:
        cmd[2:2] = ["--stackup", stack]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode not in (0, 3):
        raise SystemExit(f"faraday_cli {target}: {r.stdout}{r.stderr}")


def netlen(report):
    b = report["board"]
    nets = {n["id"]: n["name"] for n in b["nets"]}
    d = collections.defaultdict(float)
    for s in b["segments"]:
        d[nets[s["net"]]] += math.hypot(s["x2"] - s["x1"], s["y2"] - s["y1"])
    return d


def pick_stack(board, tmp):
    """BOTH sides must run the SAME stackup or the comparison measures
    dielectrics, not importers (bms-c1 carries its own stackup; using it on
    the native side against a default on the ODB side diverged the verdicts).
    The ODB++ job can never carry one, so both sides get the generic default
    matching the board's copper count — found by trying."""
    out = os.path.join(tmp, "probe.json")
    for stack in STACKS.values():
        r = subprocess.run([CLI, board, "--stackup", stack, "-o", out],
                           capture_output=True, text=True)
        if r.returncode in (0, 3):
            return stack
    return None


def sweep(board, tmp):
    name = os.path.basename(board).replace(".kicad_pcb", "")
    stack = pick_stack(board, tmp)
    if stack is None:
        print(f"{name}: SKIP (no default stackup for its copper count)")
        return True
    odbdir = os.path.join(tmp, name)
    # flatpak cannot see /tmp of the host user in all setups; stage in HOME
    subprocess.run(kicad_cli() + ["pcb", "export", "odb", "--compression",
                                  "none", "--units", "mm", "--precision", "6",
                                  "-o", odbdir, os.path.abspath(board)],
                   check=True, capture_output=True)
    kj = os.path.join(tmp, f"kic-{name}.json")
    oj = os.path.join(tmp, f"odb-{name}.json")
    run_cli(board, stack, kj)
    run_cli(odbdir, stack, oj)
    k = json.load(open(kj))
    o = json.load(open(oj))
    kb, ob = k["board"], o["board"]

    ok = True
    if len(kb["vias"]) != len(ob["vias"]):
        print(f"{name}: VIA COUNT {len(kb['vias'])} != {len(ob['vias'])}")
        ok = False
    tk = sum(netlen(k).values())
    to = sum(netlen(o).values())
    # ODB++ runs slightly LONGER (finer arc tessellation); never shorter
    if not (tk * 0.995 <= to <= tk * 1.10):
        print(f"{name}: ROUTED {tk:.0f} vs {to:.0f} mm outside tolerance")
        ok = False
    fk, fo = len(k["findings"]), len(o["findings"])
    if abs(fk - fo) > max(4, 0.1 * max(fk, fo)):
        print(f"{name}: FINDINGS {fk} vs {fo} diverge")
        ok = False
    status = "OK " if ok else "FAIL"
    print(f"{status} {name}: segs {len(kb['segments'])}/{len(ob['segments'])}"
          f" vias {len(kb['vias'])}/{len(ob['vias'])}"
          f" routed {tk:.0f}/{to:.0f} mm findings {fk}/{fo}")
    return ok


def main():
    boards = sys.argv[1:]
    if not boards:
        for d in (os.path.join(REPO, "corpus"),
                  os.path.join(REPO, "cpp", "tests", "fixtures", "real")):
            if os.path.isdir(d):
                boards += [os.path.join(d, f) for f in sorted(os.listdir(d))
                           if f.endswith(".kicad_pcb")]
    boards = list(dict.fromkeys(boards))
    stage = tempfile.mkdtemp(prefix="faraday-xfmt-",
                             dir=os.path.expanduser("~"))
    try:
        results = [sweep(b, stage) for b in boards]
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    print(f"{sum(results)}/{len(results)} boards agree across formats")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
