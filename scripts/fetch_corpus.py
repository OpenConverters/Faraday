#!/usr/bin/env python3
"""Fetch open-hardware KiCad boards for the Faraday validation corpus.

Each entry: (slug, owner/repo, kind). We list the repo tree via the GitHub
API, pick .kicad_pcb files, and download them. Nothing is committed — this
populates a local corpus dir only.
"""
import json, os, sys, urllib.request, urllib.parse

CORPUS = sys.argv[1] if len(sys.argv) > 1 else "corpus"
REPOS = [
    ("vesc",         "vedderb/bldc-hardware",           "motor drive (BLDC inverter)"),
    ("openinverter", "jsphuebner/inverter-hardware",    "EV traction inverter"),
    ("icebreaker",   "icebreaker-fpga/icebreaker",      "FPGA dev board"),
    ("orangecrab",   "gregdavill/OrangeCrab",           "ECP5 FPGA + DDR3"),
    ("mppt-2420-lc", "LibreSolar/mppt-2420-lc",         "solar charge controller (buck)"),
    ("bms-c1",       "LibreSolar/bms-c1",               "battery management"),
    ("glasgow",      "GlasgowEmbedded/glasgow",         "debug/interface tool"),
    ("fomu",         "im-tomu/fomu-hardware",           "tiny FPGA"),
    ("ulx3s",        "emard/ulx3s",                     "FPGA board"),
    ("openpnp",      "openpnp/openpnp-capacitive-probe","sensor board"),
    # Tier-4 validation additions (ABT #422): controller + DISCRETE-FET
    # converters, so the derived critical mesh has roles to infer
    ("mppt-1210-hus","LibreSolar/mppt-1210-hus",        "solar charge controller (buck)"),
    ("pwm-2420-lus", "LibreSolar/pwm-2420-lus",         "PWM charge controller (FET switch)"),
    ("moteus",       "mjbots/moteus",                   "brushless servo drive (3 half-bridges)"),
]

def api(url):
    req = urllib.request.Request(url, headers={"User-Agent": "faraday-corpus"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)

os.makedirs(CORPUS, exist_ok=True)
manifest = []
for slug, repo, kind in REPOS:
    try:
        info = api(f"https://api.github.com/repos/{repo}")
        branch = info["default_branch"]
        lic = (info.get("license") or {}).get("spdx_id") or "see repo"
        tree = api(f"https://api.github.com/repos/{repo}/git/trees/{branch}?recursive=1")
        pcbs = [e["path"] for e in tree.get("tree", [])
                if e["path"].endswith(".kicad_pcb")]
        if not pcbs:
            print(f"  {slug}: no .kicad_pcb found"); continue
        # biggest file = the main board, usually
        pcbs.sort(key=len)
        path = pcbs[0]
        url = f"https://raw.githubusercontent.com/{repo}/{branch}/{urllib.parse.quote(path)}"
        dest = os.path.join(CORPUS, f"{slug}.kicad_pcb")
        urllib.request.urlretrieve(url, dest)
        size = os.path.getsize(dest)
        manifest.append(dict(slug=slug, repo=repo, kind=kind, path=path,
                             branch=branch, license=lic, bytes=size))
        print(f"  {slug}: {path} ({size//1024} kB, {lic})")
    except Exception as e:
        print(f"  {slug}: FAILED {type(e).__name__}: {e}")

json.dump(manifest, open(os.path.join(CORPUS, "manifest.json"), "w"), indent=1)
print(f"{len(manifest)} boards -> {CORPUS}/")
