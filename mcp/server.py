"""Faraday MCP server — automated EMC layout review, reachable by an assistant.

Faraday screens a PCB layout with computational geometry and closed-form transmission-line
physics and returns ranked crosstalk/EMC findings, each with the mechanism, the number, a
confidence tier and a remediation hint. Until now the only ways in were the browser app and
the CLI, so the layout review — the thing an FAE actually reacts to — was unreachable from a
chat (ABT #664).

    review_board(board, stackup?)   screen a layout, ranked findings + the board to look at
    list_findings(review, ...)      filter a completed review by severity, rule or net
    explain_finding(review, id)     one finding in full: mechanism, numbers, remediation
    faraday_capabilities()          what it reads, what it screens, what a stackup is for
    extract_bom(board)              the board's parts as a BOM — no stackup, no catalogue
    crossref_board(board, ...)      every part identified and cross-referenced, exactly as
                                    the web app does it (parts.js + Kelvin, in node);
                                    compact per line, full detail kept under a handle
    crossref_line(crossref, ref)    one cross-referenced line in full

THE BOARD DOES NOT LEAVE THE MACHINE. Faraday's whole premise is local analysis, so this
server is an ADDITIONAL entry point, not a replacement: it reads a path on the host it runs
on, runs the same `faraday_cli` the operator would run, and writes its reports under the
user's own home. Nothing is uploaded anywhere. Run it on the engineer's machine and the
property holds exactly as it does for the web app; run it centrally and the boards are on the
central machine — which is a deployment decision, and the README says so.

Run:
    python3 mcp/server.py               # 127.0.0.1:8407/mcp
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import threading
import time
import uuid
import zipfile
from contextlib import contextmanager
from pathlib import Path

from mcp.server.fastmcp import FastMCP
from mcp.server.transport_security import TransportSecuritySettings
from mcp.types import CallToolResult, TextContent

from artifacts import display_name, resolved

_REPO = Path(__file__).resolve().parent.parent
PORT = 8407     # Hertz 8400, Kirchhoff 8401, Kelvin 8402, Moebius 8404, Heaviside 8405, OMFEM 8406

# --- MCP Apps ---------------------------------------------------------------
UI_RESOURCE_MIME = "text/html;profile=mcp-app"
UI_BOARD_URI = "ui://faraday/board.html"
UI_BUNDLES = {UI_BOARD_URI: Path(__file__).parent / "dist" / "board.html"}


def _ui_meta(uri: str) -> dict:
    """registerAppTool() emits BOTH the flat key and the nested object, so hosts reading
    either form find it."""
    return {"ui/resourceUri": uri, "ui": {"resourceUri": uri}}


UI_BOARD_META = _ui_meta(UI_BOARD_URI)


def assert_widgets_resolve() -> None:
    """Refuse to start rather than advertise a UI the host cannot fetch (ABT #651)."""
    missing = [f"{uri} -> {path}" for uri, path in UI_BUNDLES.items() if not path.exists()]
    if missing:
        raise FileNotFoundError(
            "widget bundle(s) missing, so these tools would advertise a UI the host cannot "
            "fetch: " + "; ".join(missing) + " -- build them: cd mcp && npm install && npm run build")


# --- transport --------------------------------------------------------------
# The allowlist must be built from the port the server ACTUALLY binds, not from the default:
# an operator who moves the port with FARADAY_MCP_PORT would otherwise fail every request with
# a bare "421 Invalid Host header", which hosts routinely surface as a sign-in error and which
# says nothing about the port.
_PORT = int(os.environ.get("FARADAY_MCP_PORT", PORT))
_public = os.environ.get("FARADAY_PUBLIC_HOST", "").strip()
if "://" in _public:
    _public = _public.split("://", 1)[1]
_public = _public.split("/", 1)[0].strip()
if os.environ.get("FARADAY_ALLOW_ANY_HOST") == "1":
    _security = TransportSecuritySettings(enable_dns_rebinding_protection=False)
else:
    _allowed = [f"127.0.0.1:{_PORT}", f"localhost:{_PORT}", "127.0.0.1", "localhost"]
    if _public:
        _allowed += [_public, f"{_public}:443"]
    _origins = ["https://claude.ai", "https://www.claude.ai",
                "http://localhost:*", "http://127.0.0.1:*"]
    if _public:
        _origins.append(f"https://{_public}")
    _origins += [o.strip() for o in os.environ.get("FARADAY_ALLOWED_ORIGINS", "").split(",")
                 if o.strip()]
    _security = TransportSecuritySettings(allowed_hosts=_allowed, allowed_origins=_origins)

mcp = FastMCP("Faraday", host=os.environ.get("FARADAY_MCP_HOST", "127.0.0.1"),
              port=_PORT, transport_security=_security)

SEVERITIES = ("high", "medium", "low", "info")
# Every rule the engine can fire — `f.rule = …` across cpp/include/faraday/ (Screener.hpp and
# Report.hpp; pdn-antiresonance is set in the latter, which is exactly how the first version of
# this list came out one rule short).
#
# Listed rather than derived because nothing in a report enumerates the rules that did NOT
# fire, and "what do you screen for" is the question faraday_capabilities exists to answer.
# smoke.py asserts every rule a corpus review produces is in here, so the list cannot drift
# silently when the engine grows one — it caught pdn-antiresonance on its first run. It did NOT
# catch coupled-bundle, the esd-*, filter-* and y-cap-return rules, which a corpus review fires
# only some of, or none; so it now also compares this list with every `rule = "..."` literal
# in cpp/include/faraday.
RULES = ("3w", "cap-via-stub", "commutation-loop", "connector-ground-spread", "coupled-bundle",
         "coupled-run", "critical-mesh-ground", "dangling-stub", "decoupling-distance",
         "diff-pair", "diff-skew", "edge-radiation", "esd-clamp-distance", "esd-clamp-return",
         "esd-unprotected-pin", "filter-bypass", "filter-io-coupling", "no-reference-plane",
         "pdn-antiresonance", "plane-cavity-mode", "plane-crossing", "sparse-reference",
         "switch-node", "via-stub", "y-cap-return")
# A review is milliseconds, but its report is the object every other tool reads, so it is kept
# rather than recomputed: a finding id must mean the same thing in explain_finding as it did in
# the list the caller is reading from.
REVIEW_ROOT = Path(os.environ.get("FARADAY_REVIEW_DIR") or (Path.home() / ".faraday" / "reviews"))
REVIEW_TIMEOUT_S = float(os.environ.get("FARADAY_TIMEOUT_S", "600"))


def _cli() -> Path:
    path = Path(os.environ.get("FARADAY_CLI") or (_REPO / "build" / "faraday_cli"))
    if not path.exists():
        raise FileNotFoundError(
            f"the Faraday CLI is not at {path} -- build it (cmake -S cpp -B build && "
            f"cmake --build build -j) or set FARADAY_CLI")
    return path


def _document_result(summary: str, *, schema: str, operation: str, document,
                     subject: str | None = None) -> CallToolResult:
    """A `document` result — here, part of an engine report read back by handle."""
    payload = {"mode": "document", "schema": {"name": schema}, "operation": operation,
               "document": document}
    if subject:
        payload["subject"] = subject
    return _result(summary, payload)


def _result(summary: str, payload: dict) -> CallToolResult:
    """Two channels: a digest for the model, the payload for the widget.

    A board report is megabytes of geometry — it belongs in structuredContent, where the
    widget reads it, and never in the text the model has to carry.
    """
    return CallToolResult(content=[TextContent(type="text", text=summary)],
                          structuredContent=payload)


def _load(review: str) -> dict:
    """A stored report, by review id."""
    path = REVIEW_ROOT / review / "report.json"
    if not path.exists():
        raise ValueError(
            f"no review {review!r} -- it was never run here, or its directory was removed from "
            f"{REVIEW_ROOT}. Run review_board again; a review takes milliseconds.")
    return json.loads(path.read_text(encoding="utf-8"))


def _meta_of(review: str) -> dict:
    """What review_board recorded about a run — the board it screened, the stackup, the time.

    Kept beside the report so a follow-up call can say WHICH board it is talking about. The
    payload names its subject, and a review id is not a name an engineer recognises.
    """
    path = REVIEW_ROOT / review / "meta.json"
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def _net_names(report: dict) -> list[str]:
    """The board's net table, indexed the way findings reference it.

    A finding names its nets by INDEX (`netA`/`netB`), and -1 means the finding is not about a
    particular net — a plane-crossing rollup is about the plane. Printing the raw index would
    put "8 <-> 12" in front of an engineer, which names nothing.
    """
    return [n.get("name") or "(unnamed)" for n in (report.get("board") or {}).get("nets") or []]


def _nets_of(f: dict, names: list[str]) -> list[str]:
    out = []
    for key in ("netA", "netB"):
        idx = f.get(key)
        if isinstance(idx, int) and 0 <= idx < len(names):
            out.append(names[idx])
        elif isinstance(idx, str) and idx:
            out.append(idx)
    return out


def _finding_brief(f: dict, names: list[str] | None = None) -> str:
    where = " <-> ".join(_nets_of(f, names or [])) or "board-wide"
    return (f"  {f.get('id')}  [{f.get('severityLabel')}]  {f.get('rule')}: {f.get('title')}\n"
            f"      {where}"
            + (f" · {f['coupledLenMm']:.1f} mm coupled" if isinstance(f.get("coupledLenMm"),
                                                                      (int, float)) else "")
            + (f" · min sep {f['minSepMm']:.3f} mm" if isinstance(f.get("minSepMm"),
                                                                  (int, float)) else "")
            + f" · confidence {f.get('confidence')}")


def _counts(findings: list[dict]) -> dict:
    out = {s: 0 for s in SEVERITIES}
    for f in findings:
        label = f.get("severityLabel")
        if label in out:
            out[label] += 1
    return out


# --- the pipeline contract --------------------------------------------------
# Every payload below is a `findings` result under Moebius's
# contracts/pipeline_result.json. Written to the contract rather than to this
# engine's own report shape, because the report is what FARADAY produces and the
# payload is what a CONSUMER reads: a widget, an orchestrator, the next server.
#
# Three things the retrofit changes, and each one was a real defect:
#
#   * `mode` was "review", which is not a value in the contract's enum. Moebius
#     validates at the boundary, so every call raised there.
#   * numbers carried their unit in the field NAME (`minSepMm`, `coupledLenMm`,
#     `nextDb`), which has to be renamed the day a board is reported in mils.
#     They are now `{value, unit}` pairs.
#   * nets were INDICES. `8 <-> 12` names nothing to an engineer, and a consumer
#     could not resolve it without the board's net table.
#
# The engine's own report still travels, whole, as `subject.document` — that is
# what BoardView draws, and it is the same object the CLI writes. `findings` is
# the contract projection of the same set, in the same order.
CONFIDENCE_TIERS = ("exact", "geometric-only", "screening-estimate", "heuristic", "user-declared")


def _metric(value, unit: str | None = None, label: str | None = None) -> dict | None:
    """One named scalar, unit BESIDE the value. None when the engine did not measure it —
    an absent metric and a metric of zero are different facts."""
    if value is None:
        return None
    out: dict = {"value": value}
    if unit:
        out["unit"] = unit
    if label:
        out["label"] = label
    return out


def _finding_metrics(f: dict) -> dict:
    """The numbers behind a finding, named without their units.

    `solve` is the closed-form's INPUTS — the geometry it was evaluated on. They are here
    because a screening estimate whose inputs are invisible cannot be checked against a field
    solve, which is the whole point of the confidence tier.
    """
    solve = f.get("solve") or {}
    pairs = {
        "coupledLength": _metric(f.get("coupledLenMm"), "mm", "coupled length"),
        "minimumSeparation": _metric(f.get("minSepMm"), "mm", "minimum separation"),
        "nearEndCrosstalk": _metric(f.get("nextDb"), "dB", "NEXT (saturated)"),
        "severityScore": _metric(f.get("severity"), "1", "severity score"),
        "gap": _metric(solve.get("gapMm"), "mm"),
        "substrateHeight": _metric(solve.get("hMm"), "mm"),
        "copperThickness": _metric(solve.get("tMm"), "mm"),
        "trackWidthA": _metric(solve.get("w1Mm"), "mm"),
        "trackWidthB": _metric(solve.get("w2Mm"), "mm"),
        "relativePermittivity": _metric(solve.get("epsR"), "1"),
        "transmissionLineMode": _metric(solve.get("mode")),
    }
    return {k: v for k, v in pairs.items() if v is not None}


def _involves(f: dict, names: list[str], copper: list[str]) -> list[dict]:
    """What the finding is ABOUT, by name — nets first, then the copper layers it spans.

    netA/netB of -1 means 'not about a particular net' (a plane-crossing rollup is about the
    plane), and that is an omission rather than a net called '-1'.
    """
    out = [{"kind": "net", "name": n} for n in _nets_of(f, names)]
    for key in ("cuA", "cuB"):
        idx = f.get(key)
        if isinstance(idx, int) and 0 <= idx < len(copper):
            layer = {"kind": "layer", "name": copper[idx]}
            if layer not in out:
                out.append(layer)
    return out


def _location(f: dict, copper: list[str]) -> dict | None:
    """Where it is, in board millimetres, so a consumer can pin it rather than paraphrase it.

    The engine's line segments are {x1, y1, x2, y2, cu, w}; the contract's are [x1, y1, x2, y2]
    — the layer is on the location, and the width is copper geometry the drawing already holds.
    """
    geom = f.get("geom") or {}
    points = [[p[0], p[1]] for p in (geom.get("markers") or []) if isinstance(p, list) and len(p) >= 2]
    lines = [[ln["x1"], ln["y1"], ln["x2"], ln["y2"]] for ln in (geom.get("lines") or [])
             if isinstance(ln, dict) and {"x1", "y1", "x2", "y2"} <= ln.keys()]
    if not points and not lines:
        return None
    out: dict = {"unit": "mm"}
    idx = f.get("cuA")
    if isinstance(idx, int) and 0 <= idx < len(copper):
        out["layer"] = copper[idx]
    if points:
        out["points"] = points
    if lines:
        out["lines"] = lines
    return out


def _contract_finding(f: dict, names: list[str], copper: list[str]) -> dict:
    """One engine finding as the contract's `finding`.

    Raises rather than substituting when the engine gives a confidence tier the contract does
    not know: a consumer that must treat a heuristic differently from an exact geometric fact
    cannot be handed an unrecognised tier quietly, and a new tier is a contract change.
    """
    confidence = f.get("confidence")
    if confidence not in CONFIDENCE_TIERS:
        raise ValueError(
            f"finding {f.get('id')} carries confidence {confidence!r}, which the pipeline "
            f"contract does not define — it knows {', '.join(CONFIDENCE_TIERS)}. Either the "
            f"engine grew a tier or the report is from an older build; the contract has to "
            f"learn it before this finding can cross a boundary.")
    severity = f.get("severityLabel")
    if severity not in SEVERITIES:
        raise ValueError(f"finding {f.get('id')} has severity {severity!r}, not one of "
                         f"{', '.join(SEVERITIES)}")
    out = {
        "id": f["id"],
        "severity": severity,
        "rule": f.get("rule") or "unnamed-rule",
        "summary": f.get("title") or f.get("rule") or f["id"],
        "confidence": confidence,
    }
    for key, field in (("detail", "detail"), ("remediation", "remediation")):
        if f.get(key):
            out[field] = f[key]
    metrics = _finding_metrics(f)
    if metrics:
        out["metrics"] = metrics
    involves = _involves(f, names, copper)
    if involves:
        out["involves"] = involves
    location = _location(f, copper)
    if location:
        out["location"] = location
    return out


def _dropped(meta: dict) -> list[dict]:
    """What the SCREEN found and did not report, by reason.

    A number would not do: 'the per-report cap' and 'below the reporting floor' are different
    facts about coverage, and a reader has to be able to tell which one happened. An empty
    list means nothing was dropped, which is itself a fact.
    """
    out = []
    if meta.get("droppedByFindingCap"):
        out.append({"count": int(meta["droppedByFindingCap"]),
                    "reason": "the per-report finding cap — this is the top of a longer list"})
    if meta.get("droppedBelowFloorDb"):
        out.append({"count": int(meta["droppedBelowFloorDb"]),
                    "reason": f"below the {meta.get('reportFloorDb')} dB reporting floor"})
    return out


def _findings_payload(review: str, board: str, report: dict, findings: list[dict],
                      dropped: list[dict] | None = None, held_back: int = 0,
                      tally: list[dict] | None = None) -> dict:
    """A `findings` result: the contract projection, plus the engine's report for the widget.

    `findings` and `subject.document.findings` are the SAME set in the same order — one in the
    contract's vocabulary for consumers, one in the engine's for the drawing. They are built
    from one list here rather than in each tool, because two tools that filtered differently
    would render a board that disagrees with the answer beside it.
    """
    names = _net_names(report)
    copper = (report.get("board") or {}).get("copperNames") or []
    return {
        "mode": "findings",
        "review": review,
        "subject": {
            "kind": "board",
            "name": display_name(board),
            "reference": str(board),
            "schema": {"name": "faraday.report", "version": str(report.get("faraday") or "")},
            # NO `document`. A widget that must DRAW the copper needs the report; the model
            # does not — and on a real board it is 860,566 characters, which a client refuses
            # outright: the model then sees nothing, retries, and the turn ends with no answer
            # while this engine reports success. The widget fetches it for itself with
            # fetch_report(review), which is what the app bridge's callTool is for, and finds
            # the id in this payload's own `review` field rather than in a new one — the
            # contract's subject is closed, and it was right to refuse the field I invented.
        },
        # Geometry screening, never a compliance statement. Explicit for the same reason the
        # contract makes it explicit on a verdict: silence would read as 'established defect'.
        "provisional": True,
        **({"caveat": " ".join(caveats)} if (caveats := _payload_caveats(
            report, review, held_back)) else {}),
        # counts describes the REVIEW; `reported` describes this payload. They differ when a
        # payload is truncated, and that difference is how the widget knows to draw from the
        # report rather than from a list that names five findings on a board with two hundred.
        "counts": _counts(tally if tally is not None else findings),
        "reported": len(findings),
        "dropped": dropped or [],
        "findings": [_contract_finding(f, names, copper) for f in findings],
    }


def _stackup_assumed(report: dict) -> str:
    """The engine's stackup source when the dielectric was ASSUMED, else ''.

    review_board passes --stackup auto: a board whose file carries no stackup is screened on
    default-<N>layer for the N copper layers the importer counted, and the engine stamps the
    source "assumed:default-<N>layer (N copper layers counted; the file carries no stackup)".
    Every impedance, coupling and dB figure rests on that dielectric, so it is said first.
    """
    source = str((report.get("board") or {}).get("stackupSource")
                 or (report.get("meta") or {}).get("stackupSource") or "")
    return source if source.startswith("assumed:") else ""


def _stackup_warning(source: str) -> str:
    return (f"ASSUMED STACKUP: screened on {source}. Every impedance, coupling and dB figure "
            f"rests on this assumed dielectric — pass the real stackup to review_board to "
            f"replace it.")


def _payload_caveats(report: dict, review: str, held_back: int) -> list[str]:
    caveats = []
    if assumed := _stackup_assumed(report):
        caveats.append(_stackup_warning(assumed))
    if held_back > 0:
        caveats.append(f"{held_back} further finding(s) are not in this payload — "
                       f"list_findings(review='{review}') returns them, filtered.")
    return caveats


def _truncation_note(meta: dict) -> str:
    """What the engine did NOT report, said out loud.

    A review that returns 200 findings while dropping 228 more reads as a complete answer.
    The cap and the reporting floor are both real omissions and both are in the report, so
    they belong in the first sentence a reader sees, not in a meta block nobody opens.
    """
    notes = []
    if meta.get("droppedByFindingCap"):
        notes.append(f"{meta['droppedByFindingCap']} further finding(s) were dropped by the "
                     f"per-report cap — this list is the top of a longer one")
    if meta.get("droppedBelowFloorDb"):
        notes.append(f"{meta['droppedBelowFloorDb']} were below the {meta.get('reportFloorDb')} dB "
                     f"reporting floor")
    return ("\n" + "; ".join(notes) + "." if notes else "")


# --- tools ------------------------------------------------------------------

@mcp.tool(
    title="What Faraday reviews",
    description=(
        "The layout formats Faraday reads, the EMC rules it screens for, and what a stackup "
        "is needed for. Read this before submitting a board if you are unsure what to pass."
    ),
    structured_output=False,
)
def faraday_capabilities() -> CallToolResult:
    """Formats, rules and the stackup question."""
    formats = {
        "KiCad": ".kicad_pcb (KiCad 5-9)",
        "HyperLynx": ".hyp — carries its own stackup with permittivity",
        "IPC-2581": ".xml (rev B/C)",
        "ODB++": "a job directory or one zip",
        "Gerber X2": "the file set, a directory, or one zip",
        "Gerber + IPC-D-356": "classic RS-274X plus the .ipc netlist for exact nets/refdes",
    }
    return _result(
        "Faraday screens a PCB layout for crosstalk and EMC risk: coupled runs (edge, "
        "broadside, to pour boundaries), 3W, differential pairs and skew, return-path breaks "
        "(plane crossings, sparse reference), via and dangling stubs, decoupling distance and "
        "cap-via stubs, PDN antiresonance, plane-cavity modes, connector ground spread, edge "
        "radiation, and — for converters — the switch node and the commutation loop whose "
        "enclosed area dominates emissions.\n"
        "Formats (detected from CONTENT, not filename): " + "; ".join(formats) + ".\n"
        "A board whose file carries no stackup is screened on default-<N>layer for the N "
        "copper layers counted off the board, and the review says so first: the dielectric "
        "is ASSUMED and decides every impedance and coupling number in the report. Pass the "
        "real stackup (stackup='default-2layer', 'default-<N>layer' or a custom stackup JSON "
        "path) to replace it, or stackup='none' to have such a board refused instead.\n"
        "The board is read from a path on THIS machine and never leaves it.",
        # A `catalogue` result: what this pipeline can answer about. Rules, formats and
        # stackups are three different KINDS of thing and each item says which it is —
        # a reader picking a stackup must never mistake it for a rule that fired.
        {"mode": "catalogue",
         "families": (
             [{"name": rule, "kind": "rule"} for rule in RULES]
             + [{"name": name, "kind": "format", "detail": detail}
                for name, detail in formats.items()]
             + [{"name": s, "kind": "stackup"} for s in
                ("auto", "none", "default-2layer", "default-4layer", "default-<N>layer")]
             + [{"name": s, "kind": "severity"} for s in SEVERITIES]),
         "units": "mm for geometry, dB for coupling"})


# --- zips ------------------------------------------------------------------
# faraday_cli walks a DIRECTORY (recursively, because an ODB++ job is a tree)
# but it has no zip reader: handed an archive it slurps the bytes, finds a NUL
# in the first 8 kB and refuses it as binary. Every caller here has been
# promised "a directory or one zip" since this tool existed, so the archive is
# opened on this side, where zipfile is already in the standard library.
#
# THE CASE THAT MADE IT URGENT. An Altium project zip was refused with "this is
# a native CAD database (.PcbDoc) — export ODB++ or Gerber X2 and drop that",
# while the very same archive carried `Project Outputs/.../odb/` — a complete
# ODB++ job, already exported, four copper layers, 124 findings when pointed at
# directly. The advice was not merely unhelpful, it was wrong: the user had
# done the thing they were being told to do. The engine was never the problem;
# nothing had unzipped the file.
#
# The whole tree is handed over, .PcbDoc and spreadsheets and all: import_board_set
# looks for an ODB++ matrix first and skips binary members before sniffing the
# rest, so the export is found wherever in the project it happens to sit. Picking
# a subdirectory here would mean re-implementing that search in Python, worse.

ZIP_MAGIC = b"PK\x03\x04"
# What a board zip may cost once opened. A zip bomb is 42 kB on disk and
# petabytes expanded, and this runs on the machine holding the boards.
MAX_UNZIPPED_BYTES = 2 * 1024 * 1024 * 1024
MAX_ZIP_MEMBERS = 20000


def _is_zip(path: Path) -> bool:
    """By CONTENT. An artifact:// fetch names its temp file from the URL, so the
    extension is whatever the orchestrator chose — often nothing at all."""
    try:
        with path.open("rb") as handle:
            return handle.read(4) == ZIP_MAGIC
    except OSError:
        return False


def _safe_extract(archive: zipfile.ZipFile, dest: Path) -> int:
    """Extract every member UNDER dest, refusing any that would escape it.

    zipfile.extractall sanitises paths, but silently — a member named
    ../../etc/x lands somewhere unexpected and nothing says so. A board zip has
    no business containing one, so it is an error here rather than a repair.
    """
    total = 0
    members = archive.infolist()
    if len(members) > MAX_ZIP_MEMBERS:
        raise ValueError(f"this zip holds {len(members)} entries, over the "
                         f"{MAX_ZIP_MEMBERS} a board export should ever need")
    root = dest.resolve()
    for member in members:
        target = (dest / member.filename).resolve()
        if not target.is_relative_to(root):
            raise ValueError(f"refusing {member.filename!r}: it points outside the archive")
        total += member.file_size
        if total > MAX_UNZIPPED_BYTES:
            raise ValueError(
                f"this zip expands past the "
                f"{MAX_UNZIPPED_BYTES // (1024 * 1024)} MB limit — pass the "
                f"exported job as a directory instead")
    archive.extractall(dest)
    return len(members)


@contextmanager
def board_tree(source: Path, reference: str):
    """The path to hand the CLI: a zip becomes a directory, anything else passes through.

    The extracted copy is removed on the way out. A directory or a single file
    the user already had is never touched, let alone deleted.
    """
    if not _is_zip(source):
        yield source
        return
    workdir = Path(tempfile.mkdtemp(prefix="faraday-zip-"))
    try:
        try:
            with zipfile.ZipFile(source) as archive:
                _safe_extract(archive, workdir)
        except zipfile.BadZipFile as error:
            raise ValueError(f"{display_name(reference)} starts like a zip but "
                             f"cannot be read as one: {error}") from error
        # A single top-level directory is the usual shape ("Project/…"); descend
        # into it so relative member names in the report read as the exporter
        # wrote them rather than gaining a wrapper nobody chose.
        entries = list(workdir.iterdir())
        yield entries[0] if len(entries) == 1 and entries[0].is_dir() else workdir
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


@mcp.tool(
    title="Review a board",
    description=(
        "Screen a PCB layout for EMC and crosstalk risk. Takes a path to a layout file, a "
        "Gerber/ODB++ directory or a zip, and returns the ranked findings plus the board "
        "itself, rendered with every finding pinned to the copper it concerns. A board whose "
        "file carries no stackup is screened on an ASSUMED default-<N>layer stackup for the "
        "copper layers it has, and the first line of the answer says so; pass the real "
        "stackup to replace the assumption."
    ),
    meta=UI_BOARD_META,
    structured_output=False,
)
def review_board(board: str, stackup: str = "auto",
                 switch_nets: list[str] | None = None, top: int = 15) -> CallToolResult:
    """Screen a layout.

    Args:
        board: the layout — a .kicad_pcb / .hyp / IPC-2581 .xml, or an ODB++ / Gerber
            directory or zip. Give a local path, file://, artifact://<id> (resolved against
            FARADAY_ARTIFACT_BASE) or an https:// URL; the bytes never travel through the
            tool arguments.
        stackup: 'auto' (default): the file's own stackup, else an ASSUMED default-<N>layer
            for the N copper layers counted, stated in the digest's first line and the
            payload's caveat. 'default-2layer' / 'default-<N>layer' / a custom stackup .json
            path overrides the file. 'none': refuse a board that carries no stackup.
        switch_nets: nets to screen as switch nodes when the converter's switching node is
            not detected automatically.
        top: how many findings to name in the digest; the widget always gets all of them.
    """
    review = uuid.uuid4().hex[:12]
    out_dir = REVIEW_ROOT / review
    out_dir.mkdir(parents=True, exist_ok=True)
    report_path = out_dir / "report.json"

    with resolved(board, "FARADAY", "board") as fetched, \
            board_tree(fetched, board) as source:
        cmd = [str(_cli()), str(source), "-o", str(report_path)]
        if stackup and stackup != "none":
            cmd += ["--stackup", stackup]
        for net in switch_nets or []:
            cmd += ["--switch-net", net]
        started = time.time()
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=REVIEW_TIMEOUT_S)
        board_name = display_name(board)
    if proc.returncode != 0 or not report_path.exists():
        shutil.rmtree(out_dir, ignore_errors=True)
        # The CLI's own refusals are good — "this board has 2 copper layers; choose
        # default-2layer" is more useful than anything this layer could paraphrase.
        raise ValueError((proc.stderr or proc.stdout or "").strip()
                         or f"faraday_cli exited {proc.returncode} with no message")

    report = json.loads(report_path.read_text(encoding="utf-8"))
    (out_dir / "meta.json").write_text(json.dumps({
        "review": review, "board": str(board), "stackup": stackup,
        "switchNets": switch_nets or [], "elapsed_s": time.time() - started,
    }, indent=1), encoding="utf-8")

    findings = report.get("findings") or []
    counts = _counts(findings)
    board_meta = report.get("board") or {}
    head = (f"{len(findings)} finding(s) on {board_name}: "
            + ", ".join(f"{n} {s}" for s, n in counts.items() if n)
            + f" — {len(board_meta.get('nets') or [])} nets, "
              f"{len(board_meta.get('segments') or [])} segments, stackup "
              f"{board_meta.get('stackupSource')}"
            + _truncation_note(report.get("meta") or {}))
    names = _net_names(report)
    if assumed := _stackup_assumed(report):
        head = _stackup_warning(assumed) + "\n" + head
    listing = "\n".join(_finding_brief(f, names) for f in findings[:max(1, int(top))])
    if len(findings) > top:
        listing += f"\n  … {len(findings) - top} more — list_findings(review='{review}') filters them."
    # The payload carries the findings the digest NAMES, not all 200 of them. Carrying every
    # finding put 285,383 characters of geometry into a result the model is then refused —
    # and nothing was reading it: the widget draws from the engine report it fetches for
    # itself, and a consumer wanting the rest has list_findings, which is what the digest
    # already tells it. The set stays in the engine's order so the two agree.
    shown = findings[:max(1, int(top))]
    return _result(
        f"{head}\n{listing}\n(review {review} — pass it to list_findings / explain_finding)",
        _findings_payload(review, board, report, shown,
                          dropped=_dropped(report.get("meta") or {}),
                          held_back=len(findings) - len(shown), tally=findings))


@mcp.tool(
    title="Read a review's report",
    description=(
        "The engine's own report for a completed review, whole or by dotted path. The board "
        "widget calls this to draw the copper; a model should ask for a narrow path, since "
        "the whole report of a real board runs to hundreds of thousands of characters."
    ),
    structured_output=False,
)
def fetch_report(review: str, path: str = "") -> CallToolResult:
    """Part of a stored review report.

    Args:
        review: the id review_board returned.
        path: dotted path, e.g. `board.stackup` or `findings.0.metrics`. Empty returns the
            whole report — what the widget wants, and rarely what a model should ask for.
    """
    node = _load(review)
    walked = []
    for step in [x for x in path.split(".") if x]:
        walked.append(step)
        if isinstance(node, list):
            try:
                node = node[int(step)]
            except (ValueError, IndexError):
                raise ValueError(f"{'.'.join(walked)} does not exist: that level is a list of "
                                 f"{len(node)} item(s), so the step must be an index.")
        elif isinstance(node, dict):
            if step not in node:
                raise ValueError(f"{'.'.join(walked)} does not exist. Available here: "
                                 f"{', '.join(sorted(node)[:14]) or '(nothing)'}")
            node = node[step]
        else:
            raise ValueError(f"{'.'.join(walked[:-1])} is a {type(node).__name__}, which has "
                             f"no {step!r} inside it.")
    size = len(json.dumps(node, separators=(",", ":")))
    if not isinstance(node, (dict, list)):
        node = {path.rsplit(".", 1)[-1] or "value": node}
    return _document_result(
        f"{path or 'the whole report'} for review {review}: {size:,} characters"
        + ("  — large; ask for a narrower path if this is refused" if size > 60_000 else ""),
        schema="faraday.report", operation="read", document=node)


@mcp.tool(
    title="Filter a review's findings",
    description=(
        "The findings of a completed review, filtered by severity, rule or net. Use after "
        "review_board when the board has more findings than one answer can hold."
    ),
    meta=UI_BOARD_META,
    structured_output=False,
)
def list_findings(review: str, severity: str | None = None, rule: str | None = None,
                  net: str | None = None, limit: int = 25) -> CallToolResult:
    """Filter one review.

    Args:
        review: the id review_board returned.
        severity: 'high', 'medium', 'low' or 'info'.
        rule: e.g. 'commutation-loop', 'plane-crossing', '3w'.
        net: only findings touching this net (substring match).
    """
    report = _load(review)
    findings = report.get("findings") or []
    if severity:
        if severity not in SEVERITIES:
            raise ValueError(f"unknown severity {severity!r} — one of: {', '.join(SEVERITIES)}")
        findings = [f for f in findings if f.get("severityLabel") == severity]
    if rule:
        rules = sorted({f.get("rule") for f in (report.get("findings") or [])})
        if rule not in rules:
            raise ValueError(f"no finding from rule {rule!r} in this review — it screened: "
                             f"{', '.join(r for r in rules if r)}")
        findings = [f for f in findings if f.get("rule") == rule]
    names = _net_names(report)
    if net:
        needle = net.lower()
        findings = [f for f in findings
                    if any(needle in n.lower() for n in _nets_of(f, names))]

    shown = findings[:max(1, int(limit))]
    filters = ", ".join(f"{k}={v}" for k, v in
                        (("severity", severity), ("rule", rule), ("net", net)) if v)
    return _result(
        f"{len(findings)} finding(s)" + (f" matching {filters}" if filters else "")
        + f" in review {review}"
        + (f" (showing {len(shown)})" if len(shown) < len(findings) else "") + ":\n"
        + ("\n".join(_finding_brief(f, names) for f in shown) if shown else "  (none)"),
        # The widget draws the board with exactly the findings this filter kept — the payload
        # builder takes ONE list and produces both, so the drawing cannot disagree with the list.
        #
        # `dropped` carries what the caller did not get and did not ask to lose: the review's
        # own cap and floor, plus this call's `limit` when it truncated. The filter itself is
        # not a drop — a caller who asked for severity=high was not denied the low ones.
        _findings_payload(
            review, (_meta_of(review) or {}).get("board") or review, report, shown,
            dropped=_dropped(report.get("meta") or {})
            + ([{"count": len(findings) - len(shown),
                 "reason": f"beyond limit={limit} for this call"}]
               if len(shown) < len(findings) else [])))


@mcp.tool(
    title="Explain one finding",
    description=(
        "One finding in full: the mechanism, the numbers behind it, the confidence tier and "
        "the remediation — plus the board with that finding pinned on it."
    ),
    meta=UI_BOARD_META,
    structured_output=False,
)
def explain_finding(review: str, finding: str) -> CallToolResult:
    """One finding, in full.

    Args:
        finding: the finding id, e.g. 'F-0007'.
    """
    report = _load(review)
    findings = report.get("findings") or []
    match = next((f for f in findings if f.get("id") == finding), None)
    if match is None:
        raise ValueError(
            f"no finding {finding!r} in review {review} — it holds "
            f"{findings[0].get('id') if findings else 'none'}"
            f"{' … ' + findings[-1].get('id') if len(findings) > 1 else ''}")

    numbers = {k: match[k] for k in
               ("minSepMm", "coupledLenMm", "severity", "cuA", "cuB") if k in match}
    return _result(
        f"{match['id']}  [{match.get('severityLabel')}]  {match.get('rule')}\n"
        f"{match.get('title')}\n\n{match.get('detail')}\n\n"
        f"Remediation: {match.get('remediation') or '(none given)'}\n"
        f"Confidence: {match.get('confidence')}"
        + (f"\nNets: {' <-> '.join(_nets_of(match, _net_names(report)))}"
           if _nets_of(match, _net_names(report)) else "")
        + (f"\nNumbers: " + ", ".join(f"{k} {v}" for k, v in numbers.items()) if numbers else ""),
        # One finding is still a `findings` result with one in it, not a branch of its own:
        # a consumer that renders a list should not need a second code path to render one.
        # Nothing is dropped here beyond what the review itself dropped — the caller asked
        # for exactly this finding and got it.
        _findings_payload(review, (_meta_of(review) or {}).get("board") or review,
                          report, [match], dropped=_dropped(report.get("meta") or {})))


# --- the parts on the board -------------------------------------------------
# A bill of materials is a `bom` result under the pipeline contract: N positions, one answer
# each, every line carrying its reference designator. Both tools below answer in it, so the
# extracted BOM and its cross-reference read line for line against each other — and against
# Kirchhoff's select_parts and Heaviside's cross_reference, which answer in the same branch.
#
# The parts come from `faraday_cli --components-out`, which imports the board for its
# components ONLY: a parts list depends on no dielectric, so neither tool asks for a stackup
# (the screen still does; nothing here screens).

def _board_parts(board: str) -> dict:
    """The board's components and pads, as the engine reads them.

    {format, copperNames, components: [{ref, footprint, partNumber, value, x, y, rot}],
     pads: [{component, pin, net, x, y, w, h, th, cu}]} — the same two arrays a review's
    report carries for the board widget, written by the same serialiser.
    """
    with resolved(board, "FARADAY", "board") as fetched, \
            board_tree(fetched, board) as source, \
            tempfile.TemporaryDirectory(prefix="faraday-bom-") as work:
        out = Path(work) / "components.json"
        proc = subprocess.run([str(_cli()), str(source), "--components-out", str(out)],
                              capture_output=True, text=True, timeout=REVIEW_TIMEOUT_S)
        if proc.returncode != 0 or not out.exists():
            raise ValueError((proc.stderr or proc.stdout or "").strip()
                             or f"faraday_cli exited {proc.returncode} with no message")
        return json.loads(out.read_text(encoding="utf-8"))


def _duplicate_refs(components: list[dict]) -> list[str]:
    seen: dict[str, int] = {}
    for c in components:
        seen[c.get("ref") or ""] = seen.get(c.get("ref") or "", 0) + 1
    return [f"{ref or '(no reference)'} appears {n} times on the board"
            for ref, n in seen.items() if n > 1]


def _bom_brief(line: dict) -> str:
    bits = [f"  {line['ref']:<8}"]
    bits.append(line["mpn"] if line.get("mpn") else "(no part number)")
    if line.get("value"):
        bits.append(f"value {line['value']}")
    if (line.get("specs") or {}).get("footprint"):
        bits.append(line["specs"]["footprint"])
    return "  ".join(bits)


@mcp.tool(
    title="List a board's parts",
    description=(
        "The board's components as a bill of materials: reference designator, value, "
        "footprint and the part number the layout export carries (Altium's ODB++ and KiCad "
        "MPN fields). Needs no stackup and asks no catalogue. Lines without a part number "
        "say so; nothing is invented."
    ),
    structured_output=False,
)
def extract_bom(board: str, top: int = 40) -> CallToolResult:
    """The parts on a board, line by line.

    Args:
        board: the layout — a .kicad_pcb / .hyp / IPC-2581 .xml, or an ODB++ / Gerber
            directory or zip (an Altium project zip with its ODB++ export inside works).
            A local path, file://, artifact://<id> or an https:// URL.
        top: how many lines to name in the digest; the payload always carries all of them.
    """
    doc = _board_parts(board)
    components = doc.get("components") or []
    with_pads = {p.get("component") for p in doc.get("pads") or []}
    lines = []
    for c in components:
        pn = (c.get("partNumber") or "").strip() or None
        # `exact` here means what the contract says it means — the part on the line IS the
        # original — because nothing has been substituted: the export names the part. An
        # `unsourced` line is one whose part the export never named, which is not a lookup
        # that failed; no lookup happened.
        line: dict = {"ref": c.get("ref") or "(no reference)",
                      "status": "exact" if pn else "unsourced",
                      "mpn": pn}
        if c.get("value"):
            line["value"] = c["value"]
        if c.get("footprint"):
            line["specs"] = {"footprint": c["footprint"]}
        notes = []
        if not pn:
            notes.append("the export carries no part number for this position"
                         + (" — only its value" if c.get("value") else ""))
        if c.get("ref") not in with_pads:
            notes.append("it has no pads on the board (a mechanical item or a placeholder, "
                         "not something a catalogue sells)")
        if notes:
            line["notes"] = "; ".join(notes)
        lines.append(line)

    sourced = sum(1 for line in lines if line["mpn"])
    diagnostics = _duplicate_refs(components)
    if len(lines) > sourced:
        diagnostics.append(
            f"{len(lines) - sourced} of {len(lines)} position(s) carry no part number in the "
            f"export; crossref_board can still identify those with a value by value and "
            f"package")
    payload: dict = {
        "mode": "bom", "lines": lines, "total": len(lines), "sourced": sourced,
        "caveat": ("Extracted from the layout file, not sourced: 'exact' means the export "
                   "itself names the part on that line, as written by the CAD tool. Nothing "
                   "has been checked against a catalogue — crossref_board does that."),
    }
    if diagnostics:
        payload["diagnostics"] = diagnostics
    shown = lines[:max(1, int(top))]
    digest = (f"{len(lines)} component(s) on {display_name(board)} ({doc.get('format')}): "
              f"{sourced} carry a part number, {len(lines) - sourced} do not.\n"
              + "\n".join(_bom_brief(line) for line in shown)
              + (f"\n  … {len(lines) - len(shown)} more lines in the payload"
                 if len(lines) > len(shown) else "")
              + "\nAs a BOM line elsewhere: ref_des = ref, original_mpn = mpn (only where one "
                "is stated), description = value + footprint.")
    return _result(digest, payload)


# --- the cross-reference worker ---------------------------------------------
# crossref_board identifies and cross-references parts with the Faraday WEB APP's own code —
# web/src/parts.js and Kelvin's crossref.js — run in node by mcp/crossref.mjs. See that file
# for why: a Python copy would disagree with the board in the browser within a release.
#
# The worker is long-lived (catalogue shards are tens of MB and load once), and it is
# restarted when any file it loaded changes on disk. It reports that list itself, from its
# module-resolution hook, so the restart check covers exactly what is running and cannot
# drift from a list kept by hand on this side.

_xref_proc: subprocess.Popen | None = None
_xref_lock = threading.Lock()
_xref_id = 0
_xref_files: list[str] = []
_xref_fingerprint: str | None = None


def _files_fingerprint(files: list[str]) -> str:
    digest = hashlib.sha256()
    for f in sorted(files):
        digest.update(Path(f).read_bytes())
    return digest.hexdigest()[:16]


def _shard_dir() -> str:
    shard_dir = os.environ.get("KELVIN_SHARD_DIR", "").strip()
    if not shard_dir:
        raise ValueError(
            "KELVIN_SHARD_DIR is not set -- crossref_board runs the web app's parts pipeline "
            "over Kelvin's catalogue, and needs the directory holding its manifest.json, "
            "<family>.kidx shards and <family>.ndjson records (the same set the web app is "
            "served under /kelvin/).")
    if not (Path(shard_dir) / "manifest.json").exists() or not list(Path(shard_dir).glob("*.kidx")):
        raise ValueError(f"KELVIN_SHARD_DIR={shard_dir} holds no manifest.json and .kidx shards")
    return shard_dir


def _xref_start() -> tuple[subprocess.Popen, dict]:
    node = shutil.which("node")
    if not node:
        raise RuntimeError("node is not on PATH -- crossref_board runs the web app's own "
                           "JavaScript in it")
    proc = subprocess.Popen(
        [node, str(Path(__file__).parent / "crossref.mjs"), "--shards", _shard_dir()],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None, text=True, bufsize=1)
    hello = proc.stdout.readline()
    handshake = json.loads(hello) if hello else {}
    if not handshake.get("ready"):
        proc.kill()
        raise RuntimeError("the cross-reference worker did not start (see its stderr above)")
    # The worker hashed what it loaded; hash the same files again here. A difference means
    # they changed while it was starting, and it is not running what is on disk.
    if _files_fingerprint(handshake["files"]) != handshake.get("fingerprint"):
        proc.kill()
        raise RuntimeError("the cross-reference worker's sources changed while it started; "
                           "try again")
    return proc, handshake


def _xref(request: dict) -> dict:
    """One round-trip with the worker; (re)started when it died or its sources changed."""
    global _xref_proc, _xref_id, _xref_files, _xref_fingerprint
    with _xref_lock:
        alive = _xref_proc is not None and _xref_proc.poll() is None
        if alive and _files_fingerprint(_xref_files) != _xref_fingerprint:
            _xref_proc.terminate()
            try:
                _xref_proc.wait(timeout=10)
            except subprocess.TimeoutExpired:                   # pragma: no cover
                _xref_proc.kill()
            alive = False
        if not alive:
            _xref_proc, hello = _xref_start()
            _xref_files, _xref_fingerprint = hello["files"], hello["fingerprint"]
        _xref_id += 1
        try:
            _xref_proc.stdin.write(json.dumps({**request, "id": _xref_id}) + "\n")
            _xref_proc.stdin.flush()
            line = _xref_proc.stdout.readline()
        except (BrokenPipeError, ValueError) as error:
            _xref_proc = None
            raise RuntimeError(f"the cross-reference worker died mid-request: {error}") from error
        if not line:
            _xref_proc = None
            raise RuntimeError("the cross-reference worker closed its output (it died)")
        reply = json.loads(line)
    if not reply.get("ok"):
        raise ValueError(reply.get("error") or "cross-reference failed")
    return reply["result"]


# What a contract `candidate` may carry. Anything else Kelvin attaches keeps its meaning under
# an underscore, which the contract reserves for pipeline-internal fields.
CANDIDATE_FIELDS = ("mpn", "manufacturer", "specs", "status", "grade", "penalty", "direction",
                    "footprint", "params", "notes", "margins", "row", "sortKey", "evidence",
                    "record")


def _ranked_candidate(c: dict) -> dict:
    """One Kelvin cross-reference verdict as the contract's `candidate`."""
    out: dict = {"mpn": c.get("mpn")}
    for key, value in c.items():
        if key == "mpn" or value is None:
            continue
        out[key if key in CANDIDATE_FIELDS or key.startswith("_") else f"_{key}"] = value
    if isinstance(out.get("specs"), dict):
        out["specs"] = {k: v for k, v in out["specs"].items() if v is not None}
    return out


def _row_candidate(row: dict, match: str, family: str) -> dict:
    """A catalogue row the board MIGHT be — not ranked, and not a verdict.

    No `status`: the contract's statuses are the ranker's judgement, and nothing judged this
    row. It matched a part-number substring or the board's value and package, which is a
    different and weaker fact, carried in `_match`.
    """
    specs = {k: v for k, v in row.items() if k not in ("mpn", "manufacturer") and v is not None}
    out = {"mpn": row.get("mpn") or "(unnamed)", "specs": specs, "_match": match,
           "_family": family}
    if row.get("manufacturer") is not None:
        out["manufacturer"] = row["manufacturer"]
    return out


_MATCH_WORDS = {
    "exact": "identified by its part number",
    "substring": "only partial part-number matches",
    "value-package": "identified only by value and package",
    "none": "not in the catalogue",
    "unlookupable": "the board does not say what it is",
    "not-a-part": "not a catalogue part",
}


_IDENTIFIED_EXACTLY = "identified exactly as"
_UNSAID = "not cross-referenced — the board does not say which part this is"


def _crossref_line(w: dict) -> tuple[dict, str, list[str]]:
    """One worker line as a contract `bomLine`, its digest row, and its diagnostics."""
    line: dict = {"ref": w["ref"], "status": "unsourced", "mpn": None}
    if w.get("value"):
        line["value"] = w["value"]
    specs = {k: w[k] for k in ("footprint", "package") if w.get(k)}
    if specs:
        line["specs"] = specs
    ident = {"match": w["match"]}
    for key in ("family", "query", "tried", "families", "outsideSuggestedFamilies",
                "parsedValue"):
        if w.get(key) not in (None, [], ""):
            ident[key] = w[key]
    line["_identification"] = ident
    diags: list[str] = []
    notes: list[str] = []
    match = w["match"]
    board_said = w.get("partNumber") or w.get("value") or "nothing"

    if match == "exact":
        orig = w.get("original") or {}
        line["originalMpn"] = orig.get("mpn")
        line["kind"] = w["family"]
        line["_originalManufacturer"] = orig.get("manufacturer")
        notes.append(f"{_IDENTIFIED_EXACTLY} {orig.get('mpn')} ({orig.get('manufacturer')}, "
                     f"{w['family']}) from '{w.get('query')}'"
                     + (" — outside the families its refdes and footprint suggest"
                        if w.get("outsideSuggestedFamilies") else ""))
        x = w.get("xref") or {}
        if w.get("xrefError"):
            notes.append(f"the cross-reference failed: {w['xrefError']}")
            diags.append(f"{w['ref']}: cross-reference failed: {w['xrefError']}")
        elif x.get("skipped"):
            notes.append(f"not cross-referenced: {x['skipped']}")
        else:
            ranked = x.get("ranked") or []
            line["candidates"] = [_ranked_candidate(c) for c in ranked]
            line["_crossref"] = {k: x[k] for k in ("poolTotal", "poolScored", "origVerified",
                                                   "missingKeys", "targetsFromCatalogue")
                                 if k in x}
            line["_crossref"]["targets"] = len(x.get("targets") or [])
            if not ranked:
                line["status"] = "no_substitute"
                notes.append(f"no candidate from {len(x.get('targets') or [])} target "
                             f"manufacturer(s) survived the ranker's pre-gate "
                             f"({x.get('poolTotal', 0)} in the pool)")
            else:
                best = ranked[0]
                line["status"] = best.get("status") or "no_substitute"
                if line["status"] in ("recommended", "partial"):
                    line["mpn"] = best.get("mpn")
                    line["manufacturer"] = best.get("manufacturer")
            if x.get("origVerified") is False:
                notes.append(f"the original's own record does not state "
                             f"{', '.join(x.get('missing') or [])}, so no candidate can be "
                             f"'recommended'")
            if x.get("unknownTargets"):
                notes.append(f"not a manufacturer of {w['family']} parts in the catalogue: "
                             f"{', '.join(x['unknownTargets'])}")
        digest = f"{w['ref']}: {orig.get('mpn')} ({orig.get('manufacturer')}) -> " + (
            f"{line['mpn']} ({line.get('manufacturer')}) {line['status']}"
            + (f"/{(x.get('ranked') or [{}])[0].get('grade')}"
               if (x.get("ranked") or [{}])[0].get("grade") else "")
            if line.get("mpn") else
            (f"no substitute ({len(line.get('candidates') or [])} ranked)"
             if line["status"] == "no_substitute" else "not cross-referenced"))
    else:
        if w.get("partNumber"):
            line["originalMpn"] = w["partNumber"]
        cands = []
        for h in w.get("near") or []:
            cands.append(_row_candidate(h["row"], "substring", h["family"]))
        by_value = w.get("byValue") or {}
        for row in by_value.get("rows") or []:
            cands.append(_row_candidate(row, "value-package", by_value["family"]))
        if cands:
            line["candidates"] = cands
        if match == "value-package":
            line["kind"] = by_value["family"]
        if match in ("substring", "value-package"):
            bits = []
            if w.get("near"):
                bits.append(f"{w.get('nearTotal')} catalogue part(s) contain "
                            f"{' or '.join(repr(t) for t in w.get('tried') or [])}")
            if by_value.get("matched"):
                bits.append(f"{by_value['matched']} {by_value['family']}(s) match its value "
                            f"and package")
            notes.append(f"{_UNSAID}: " + "; ".join(bits) + " (listed as candidates, unranked)")
        else:
            notes.append(f"{_MATCH_WORDS[match]}: {w.get('why')}")
        if w.get("lookupError"):
            notes.append(f"a catalogue lookup failed: {w['lookupError']}")
            diags.append(f"{w['ref']}: catalogue lookup failed: {w['lookupError']}")
        digest = f"{w['ref']}: {board_said} — {_MATCH_WORDS[match]}"
        if match in ("substring", "value-package"):
            digest += f" ({len(cands)} candidate(s) listed, not cross-referenced)"
    line["notes"] = "; ".join(notes)
    return line, digest, diags


# --- what a whole-board cross-reference carries inline ----------------------
# Every line in full is too much for one answer: a 189-part board came back at 691,751
# characters — 636 ranked candidates at ~900 characters each, two thirds of it spec tables,
# parameter verdicts and ranker notes. So the payload carries every LINE but a compact form of
# each, and the full lines are stored on disk under a handle, the way review_board keeps its
# report: crossref_line(crossref, ref) returns one line exactly as the ranker left it. Nothing
# is recomputed to answer that, so the detail cannot disagree with the summary.
#
# HOW COMPACT IS SET BY THE CLIENT THAT ACTUALLY READS IT, not by the one that refuses it.
# The first compact form (~650 characters a line; 122,959 for the 189-part PoE adapter board)
# was sized against clients that refuse results around 285k. Claude Code does not refuse — it
# writes any tool result above ~50k characters to a file and hands the model a notice
# instead (measured with Claude Code 2.1.289: 49,054 characters went inline, 50.8 KB was saved). On 2026-10-04
# that turned a 16-second engine call into a six-minute turn: the model spent five minutes
# grepping its own saved file seventeen times to rebuild a summary of 189 lines. Claude Code
# also passes the model this payload, NOT the digest built below, so the payload is the
# summary and has to be readable in one pass.
#
# So a line carries what a reader of the whole board needs and nothing per candidate:
# ref, status, the substitute, the original (part number, maker, family), the package, how
# the line was identified (`_match`), the best candidate's grade (`_grade`) and the checks it
# warned or failed on (`_flags`), how many other ranked candidates and unranked catalogue
# rows exist (`_alternates`, `_rows`), and the line's own notes — why nothing was sourced,
# what the original's record lacks (stock sentences are defined once, in `caveat`). The ranker's per-candidate notes, spec tables and verdicts
# are crossref_line's; `caveat` says so in every payload. ~180 characters a line.

def _inline_line(line: dict, target: str | None) -> dict:
    """A stored cross-reference line, compacted for the payload (see the block above)."""
    out: dict = {"ref": line["ref"], "status": line["status"], "mpn": line["mpn"]}
    # The maker is said once, in targetManufacturer, when the run had a single target.
    if line.get("manufacturer") is not None and line["manufacturer"] != target:
        out["manufacturer"] = line["manufacturer"]
    for key in ("originalMpn", "_originalManufacturer", "kind", "value"):
        if line.get(key) is not None:
            out[key] = line[key]
    specs = line.get("specs") or {}
    if specs.get("package") or specs.get("footprint"):
        out["specs"] = {"package": specs["package"]} if specs.get("package") \
            else {"footprint": specs["footprint"]}
    out["_match"] = (line.get("_identification") or {}).get("match")
    # Notes that only restate what the line already says are left to crossref_line: the
    # identification sentence (originalMpn, _originalManufacturer, kind) and the stock
    # sentence of each `_match` class, which `caveat` defines once instead of every line
    # repeating it — on a 272-part board those sentences were a third of the payload. What
    # stays is what only the note says: a failed cross-reference, a pre-gate nobody passed,
    # a record that lacks a value.
    match = out["_match"]
    stock = (f"{_MATCH_WORDS[match]}:" if match in ("none", "not-a-part", "unlookupable")
             else _UNSAID if match in ("substring", "value-package") else None)
    notes = [n for n in (line.get("notes") or "").split("; ")
             if n and not n.startswith(_IDENTIFIED_EXACTLY)
             and not (stock and n.startswith(stock))]
    if notes:
        out["notes"] = "; ".join(notes)
    cands = line.get("candidates") or []
    if cands and "status" in cands[0]:             # ranked by Kelvin
        best = cands[0]
        if best.get("grade") is not None:
            out["_grade"] = best["grade"]
        flags = [p["name"] for p in best.get("params") or []
                 if p.get("verdict") in ("warn", "fail")]
        if flags:
            out["_flags"] = flags
        if len(cands) > 1:
            out["_alternates"] = len(cands) - 1
    elif cands:                                    # unranked catalogue rows
        out["_rows"] = len(cands)
    return out


def _crossref_dir(crossref: str) -> Path:
    return REVIEW_ROOT / f"crossref-{crossref}"


@mcp.tool(
    title="Cross-reference a board's parts",
    description=(
        "Identify every component on a board in the Kelvin catalogue and rank substitutes "
        "for each — deterministic, no LLM, exactly what the Faraday web app does when a "
        "board is loaded: part-number match first (exact or partial), value and package "
        "otherwise, then Kelvin's own cross-reference ranker. Each line says how sure the "
        "identification is; parts the catalogue cannot identify are reported, not dropped."
    ),
    structured_output=False,
)
def crossref_board(board: str, target_manufacturers: list[str] | None = None,
                   same_type: bool = True, max_results: int = 5,
                   top: int = 30) -> CallToolResult:
    """Every part on the board, identified and cross-referenced.

    Args:
        board: the layout, as for extract_bom (no stackup needed).
        target_manufacturers: substitutes only from these vendors (matched to the
            catalogue's own spelling, accents and case ignored). Omitted: every vendor but
            the original's own, as the web app's parts panel does.
        same_type: keep substitutes of the original's own type (technology / device type).
        max_results: ranked substitutes kept per line (the panel shows 12; the payload is
            per line, so a board of 200 parts carries 200 times this).
        top: how many lines to name in the digest; the payload carries every line.
    """
    doc = _board_parts(board)
    max_results = max(1, min(int(max_results), 12))
    result = _xref({"op": "crossref_board", "components": doc.get("components") or [],
                    "pads": doc.get("pads") or [],
                    "targets": [t for t in (target_manufacturers or []) if t and t.strip()],
                    "sameType": bool(same_type), "maxResults": max_results,
                    "listed": max_results})
    lines, digests, diagnostics = [], [], list(result.get("diagnostics") or [])
    for w in result["lines"]:
        line, digest, diags = _crossref_line(w)
        lines.append(line)
        digests.append(digest)
        diagnostics += diags

    matches: dict[str, int] = {}
    for w in result["lines"]:
        matches[w["match"]] = matches.get(w["match"], 0) + 1
    statuses: dict[str, int] = {}
    for line in lines:
        statuses[line["status"]] = statuses.get(line["status"], 0) + 1
    sourced = sum(1 for line in lines if line["mpn"])
    targets = [t for t in (target_manufacturers or []) if t and t.strip()]
    crossref = uuid.uuid4().hex[:12]
    caveat = (
        "Deterministic catalogue cross-reference (Kelvin's ranker), run the way the "
        "Faraday web app runs it. Only lines identified EXACTLY by part number were "
        "cross-referenced; a line matched by value and package, or by a partial part "
        "number, has catalogue parts it might be — unranked — because the board "
        "does not say which one it is. 'mpn' on a line is the best-ranked substitute when "
        f"Kelvin rates it recommended or partial; each line keeps at most {max_results} "
        "ranked candidates. COMPACT: each line here is a summary — the substitute, its grade "
        "(_grade) and the checks it warned or failed on (_flags), the original, the package and "
        "how the line was identified (_match). Held back: every candidate's spec table, "
        "verdicts and ranker notes, the other ranked candidates (counted in _alternates) and "
        "the unranked catalogue rows a line might be (counted in _rows), and the notes that "
        "only restate `_match`, which reads: exact = identified by its part number and "
        "cross-referenced; substring = only partial part-number matches, value-package = "
        "identified only by value and package (both: _rows catalogue parts it might be, not "
        "cross-referenced, because the board does not say which); none = not in the catalogue "
        "(no part carries its part number, or, with none given, no part of its value fits its "
        "footprint); unlookupable = the board gives neither a part number nor a value; "
        "not-a-part = a mounting hole, fiducial, test point, logo, jumper or similar. "
        f"crossref_line(crossref='{crossref}', ref=<ref>) returns any line in full.")
    payload: dict = {"mode": "bom",
                     "lines": [_inline_line(line, targets[0] if len(targets) == 1 else None)
                               for line in lines],
                     "total": len(lines), "sourced": sourced, "caveat": caveat}
    if len(targets) == 1:
        payload["targetManufacturer"] = targets[0]
    if diagnostics:
        payload["diagnostics"] = diagnostics

    # The full lines, kept for crossref_line. Same order, same statuses: the payload above is
    # a projection of exactly this list.
    out_dir = _crossref_dir(crossref)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "crossref.json").write_text(json.dumps({
        "crossref": crossref, "board": str(board), "targets": targets,
        "sameType": bool(same_type), "maxResults": max_results,
        "full": {**payload, "lines": lines,
                 "caveat": caveat.split(" COMPACT:")[0]},
    }), encoding="utf-8")

    order = ("exact", "value-package", "substring", "none", "unlookupable", "not-a-part")
    head = (f"{len(lines)} component(s) on {display_name(board)}: "
            + ", ".join(f"{matches[m]} {_MATCH_WORDS[m]}" for m in order if matches.get(m))
            + f".\nCross-reference: {sourced} line(s) carry a substitute ("
            + ", ".join(f"{n} {s}" for s, n in sorted(statuses.items())) + ")"
            + (f", targets {', '.join(targets)}" if targets
               else ", targets: every vendor but the original's own")
            + ".")
    shown = digests[:max(1, int(top))]
    return _result(
        head + "\n" + "\n".join("  " + d for d in shown)
        + (f"\n  … {len(digests) - len(shown)} more lines in the payload"
           if len(digests) > len(shown) else "")
        + ("\n" + "\n".join(f"  ! {d}" for d in diagnostics[:10]) if diagnostics else "")
        + f"\n(crossref {crossref} — crossref_line(crossref, ref) returns one line in full: "
          f"spec tables, every check, every note)",
        payload)


@mcp.tool(
    title="One cross-referenced line in full",
    description=(
        "One line of a completed crossref_board, exactly as the ranker left it: every "
        "candidate's spec table, every parameter check and every note. crossref_board's "
        "payload is compact on purpose; this is where its detail lives."
    ),
    structured_output=False,
)
def crossref_line(crossref: str, ref: str) -> CallToolResult:
    """One position of a stored cross-reference.

    Args:
        crossref: the id crossref_board returned.
        ref: the reference designator, e.g. 'C12'.
    """
    path = _crossref_dir(crossref) / "crossref.json"
    if not path.exists():
        raise ValueError(
            f"no cross-reference {crossref!r} -- it was never run here, or its directory was "
            f"removed from {REVIEW_ROOT}. Run crossref_board again.")
    stored = json.loads(path.read_text(encoding="utf-8"))
    full = stored["full"]
    found = [line for line in full["lines"] if line["ref"] == ref]
    if not found:
        refs = [line["ref"] for line in full["lines"]]
        raise ValueError(f"no line {ref!r} in cross-reference {crossref} — it holds "
                         f"{len(refs)} line(s): {', '.join(refs[:20])}"
                         + (" …" if len(refs) > 20 else ""))
    # A reference designator should be unique; when the board repeats one, every line that
    # carries it is returned rather than the first silently standing for all of them.
    payload: dict = {"mode": "bom", "lines": found, "total": len(found),
                     "sourced": sum(1 for line in found if line["mpn"]),
                     "caveat": full["caveat"]}
    if full.get("targetManufacturer"):
        payload["targetManufacturer"] = full["targetManufacturer"]
    line = found[0]
    cands = line.get("candidates") or []
    listing = []
    for c in cands:
        bits = [f"  {c['mpn']} ({c.get('manufacturer')})"]
        if c.get("status"):
            bits.append(f"{c['status']}/{c.get('grade')} penalty {c.get('penalty')}")
            off = [f"{p['name']} {p['verdict']}" for p in c.get("params") or []
                   if p.get("verdict") != "pass"]
            if off:
                bits.append("; ".join(off))
        else:
            bits.append(f"unranked, matched by {c.get('_match')}")
        listing.append("  ".join(bits) + "".join(f"\n      - {n}" for n in c.get("notes") or []))
    return _result(
        f"{ref} on {display_name(stored['board'])}: {line['status']}"
        + (f", original {line['originalMpn']}" if line.get("originalMpn") else "")
        + (f" -> {line['mpn']} ({line.get('manufacturer')})" if line.get("mpn") else "")
        + (f" [{len(found)} lines carry this reference]" if len(found) > 1 else "")
        + f"\n{line.get('notes') or ''}"
        + ("\n" + "\n".join(listing) if listing else "\n  (no candidates)"),
        payload)


# --- the MCP Apps UI resource -----------------------------------------------

@mcp.resource(
    UI_BOARD_URI,
    name="faraday-board",
    title="Faraday board review",
    mime_type=UI_RESOURCE_MIME,
)
def board_widget() -> str:
    """The board, drawn by the web app's own BoardView, with every finding pinned to the
    copper it concerns and clickable — selecting one reports it back to the model so the
    next question can be about that finding.

    MCP App resources render in a deny-by-default CSP iframe, so the widget is built as ONE
    self-contained file (vite-plugin-singlefile).
    """
    bundle = UI_BUNDLES[UI_BOARD_URI]
    if not bundle.exists():                                        # pragma: no cover
        raise FileNotFoundError(
            f"{bundle} missing -- build the widget first: cd mcp && npm install && npm run build")
    return bundle.read_text(encoding="utf-8")



def _auth_middleware(app, prefix: str):
    """Optional bearer-token auth in front of the transport.

    OFF unless {PREFIX}_AUTH_TOKEN is set, because the default deployment is loopback and a
    token nobody configured would be security theatre with a support cost. Set it and every
    request must carry `Authorization: Bearer <token>`; the MCP endpoints are all that is
    protected, and the failure is a plain 401 rather than a redirect, so a client sees what
    happened instead of guessing at OAuth (ABT #656).

    This is a gate, not an identity: one shared token says the caller is allowed in, not who
    they are. Anything needing per-user identity wants a real IdP in front, and this is not a
    substitute for one.
    """
    import os as _os

    token = _os.environ.get(f"{prefix}_AUTH_TOKEN", "").strip()
    if not token:
        return app

    from starlette.responses import PlainTextResponse

    class _BearerGate:
        def __init__(self, inner):
            self.inner = inner

        async def __call__(self, scope, receive, send):
            if scope.get("type") != "http":
                await self.inner(scope, receive, send)
                return
            headers = {k.decode().lower(): v.decode() for k, v in scope.get("headers") or []}
            if headers.get("authorization", "") != f"Bearer {token}":
                response = PlainTextResponse(
                    f"401 Unauthorized: this server requires a bearer token "
                    f"({prefix}_AUTH_TOKEN).", status_code=401)
                await response(scope, receive, send)
                return
            await self.inner(scope, receive, send)

    return _BearerGate(app)


def build_app():
    """Starlette app with CORS for the streamable-HTTP transport."""
    from starlette.middleware.cors import CORSMiddleware

    assert_widgets_resolve()
    _cli()                      # fail at startup if the engine is missing, not per call
    REVIEW_ROOT.mkdir(parents=True, exist_ok=True)
    app = mcp.streamable_http_app()
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["GET", "POST", "DELETE", "OPTIONS"],
        allow_headers=["*"],
        expose_headers=["Mcp-Session-Id"],
    )
    return _auth_middleware(app, "FARADAY")


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(build_app(), host=mcp.settings.host, port=mcp.settings.port)
