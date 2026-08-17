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

import json
import os
import shutil
import subprocess
import time
import uuid
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
# silently when the engine grows one — it caught pdn-antiresonance on its first run.
RULES = ("3w", "cap-via-stub", "commutation-loop", "connector-ground-spread", "coupled-run",
         "critical-mesh-ground", "dangling-stub", "decoupling-distance", "diff-pair",
         "diff-skew", "edge-radiation", "no-reference-plane", "pdn-antiresonance",
         "plane-cavity-mode", "plane-crossing", "sparse-reference", "switch-node", "via-stub")
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
                      dropped: list[dict] | None = None) -> dict:
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
            # The engine's own report, whole. A widget that must DRAW the copper needs it in
            # hand — the same reason documentResult carries `document` rather than a path.
            "document": {**report, "findings": findings},
        },
        # Geometry screening, never a compliance statement. Explicit for the same reason the
        # contract makes it explicit on a verdict: silence would read as 'established defect'.
        "provisional": True,
        "counts": _counts(findings),
        "reported": len(findings),
        "dropped": dropped or [],
        "findings": [_contract_finding(f, names, copper) for f in findings],
    }


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
        "A stackup is required when the file does not carry one: pass stackup='default-2layer' "
        "or 'default-<N>layer' matching the board's copper count. Faraday never assumes one — "
        "the dielectric decides every impedance and coupling number in the report.\n"
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
                ("default-2layer", "default-4layer", "default-<N>layer")]
             + [{"name": s, "kind": "severity"} for s in SEVERITIES]),
         "units": "mm for geometry, dB for coupling"})


@mcp.tool(
    title="Review a board",
    description=(
        "Screen a PCB layout for EMC and crosstalk risk. Takes a path to a layout file, a "
        "Gerber/ODB++ directory or a zip, and returns the ranked findings plus the board "
        "itself, rendered with every finding pinned to the copper it concerns."
    ),
    meta=UI_BOARD_META,
    structured_output=False,
)
def review_board(board: str, stackup: str | None = None,
                 switch_nets: list[str] | None = None, top: int = 15) -> CallToolResult:
    """Screen a layout.

    Args:
        board: the layout — a .kicad_pcb / .hyp / IPC-2581 .xml, or an ODB++ / Gerber
            directory or zip. Give a local path, file://, artifact://<id> (resolved against
            FARADAY_ARTIFACT_BASE) or an https:// URL; the bytes never travel through the
            tool arguments.
        stackup: 'default-2layer' / 'default-<N>layer' — required when the file carries no
            stackup of its own. Faraday refuses to assume one.
        switch_nets: nets to screen as switch nodes when the converter's switching node is
            not detected automatically.
        top: how many findings to name in the digest; the widget always gets all of them.
    """
    review = uuid.uuid4().hex[:12]
    out_dir = REVIEW_ROOT / review
    out_dir.mkdir(parents=True, exist_ok=True)
    report_path = out_dir / "report.json"

    with resolved(board, "FARADAY", "board") as source:
        cmd = [str(_cli()), str(source), "-o", str(report_path)]
        if stackup:
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
    listing = "\n".join(_finding_brief(f, names) for f in findings[:max(1, int(top))])
    if len(findings) > top:
        listing += f"\n  … {len(findings) - top} more — list_findings(review='{review}') filters them."
    return _result(
        f"{head}\n{listing}\n(review {review} — pass it to list_findings / explain_finding)",
        _findings_payload(review, board, report, findings,
                          dropped=_dropped(report.get("meta") or {})))


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
