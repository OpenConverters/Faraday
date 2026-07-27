#!/usr/bin/env python3
"""Run faraday_cli over the corpus and emit a markdown report."""
import json, os, subprocess, sys, time
from collections import Counter

CORPUS = sys.argv[1]
CLI = os.path.expanduser("~/OpenConverters/Faraday/build/faraday_cli")
manifest = json.load(open(os.path.join(CORPUS, "manifest.json")))
# both fixtures already in-repo, screened alongside the fetched boards
manifest = [dict(slug="hackrf-one", repo="greatscottgadgets/hackrf",
                 kind="SDR (RF front end)", license="GPL-2.0"),
            dict(slug="mppt-2420-hc", repo="LibreSolar/mppt-2420-hc",
                 kind="solar charge controller (buck)", license="CERN-OHL-W-2.0"),
            ] + manifest
FIXTURES = os.path.expanduser("~/OpenConverters/Faraday/cpp/tests/fixtures/real")

rows = []
for m in manifest:
    src = os.path.join(FIXTURES, m["slug"] + ".kicad_pcb")
    if not os.path.exists(src):
        src = os.path.join(CORPUS, m["slug"] + ".kicad_pcb")
    out = os.path.join(CORPUS, m["slug"] + ".report.json")
    r = dict(m)
    # try the board's own stackup first; fall back to an EXPLICIT user choice
    for args, note in ((["-o", out], "board-file"),
                       (["--stackup", "default-4layer", "-o", out], "user:4layer"),
                       (["--stackup","default-2layer","-o",out],"user:2layer"),(["--stackup","default-6layer","-o",out],"user:6layer"),(["--stackup","default-8layer","-o",out],"user:8layer")):
        t0 = time.time()
        p = subprocess.run([CLI, src] + args, capture_output=True, text=True)
        r["ms"] = int((time.time() - t0) * 1000)
        if p.returncode == 0:
            r["stackup_mode"] = note
            break
        r["error"] = p.stderr.strip()[:220]
    if p.returncode != 0:
        r["status"] = "FAIL"
        rows.append(r); print(f"  {m['slug']}: FAIL {r.get('error','')[:90]}"); continue
    rep = json.load(open(out))
    meta = rep["meta"]
    r.update(status="ok",
             layers=len(rep["board"]["copperNames"]),
             segments=len(rep["board"]["segments"]),
             nets=len(rep["board"]["nets"]),
             findings=len(rep["findings"]),
             rules=dict(Counter(f["rule"] for f in rep["findings"])),
             sw=meta["switchNodes"], dp=meta["diffPairsRecognized"],
             capped=meta["droppedByFindingCap"],
             skipped=meta["crossingCheckSkippedPlanes"],
             polyonly=len(meta["polygonOnlyNets"]),
             stackup=meta["stackupSource"])
    rows.append(r)
    print(f"  {r['slug']:14s} {r['layers']}L {r['segments']:5d}seg "
          f"{r['findings']:3d}find {r['ms']:4d}ms  sw={len(r['sw'])} dp={r['dp']}")

json.dump(rows, open(os.path.join(CORPUS, "corpus_results.json"), "w"), indent=1)
ok = [r for r in rows if r["status"] == "ok"]
print(f"\n{len(ok)}/{len(rows)} screened, "
      f"max {max((r['ms'] for r in ok), default=0)} ms")
