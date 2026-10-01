"""End-to-end smoke test for the Faraday MCP server — every tool, on a real board.

Not a unit test: it screens boards from the corpus with the real engine and asserts the
answers are the engine's, then starts the HTTP transport and drives it with a real MCP
client. The point is that a broken tool fails HERE rather than in front of an FAE.

    KELVIN_SHARD_DIR=<Kelvin web/public/kelvin> python3 mcp/smoke.py [--skip-http]

KELVIN_SHARD_DIR is required (crossref_board cross-references against Kelvin's shards), and
every payload is validated against the Moebius pipeline contract at MOEBIUS_CONTRACT
(default: the moebius-orchestrator checkout beside this one). Neither is optional: a smoke run
that skipped them would report green over the two tools it did not run.
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parent
sys.path.insert(0, str(_HERE))

SKIP_HTTP = "--skip-http" in sys.argv
FAILURES: list[str] = []

# A real 2-layer MPPT converter board: the case Faraday exists for (a switching converter
# with a commutation loop), and it carries no stackup, which exercises the refusal too.
BOARD = _REPO / "corpus" / "mppt-1210-hus.kicad_pcb"
# A KiCad board whose parts carry MPN fields — some in Kelvin's catalogue, some not — and the
# ODB++ fixture, whose one resistor carries a value and no part number.
BOM_BOARD = _REPO / "corpus" / "glasgow.kicad_pcb"
ODB_BOARD = _REPO / "cpp" / "tests" / "fixtures" / "odb_job.zip"
# A part number on glasgow that Kelvin's catalogue carries (identified exactly), and one it
# does not (TMK105BJ104KV-F, Taiyo Yuden — the line must say so, never vanish).
KNOWN_MPN = "RC0402FR-132K2L"
UNKNOWN_MPN = "TMK105BJ104KV-F"
MOEBIUS_CONTRACT = Path(os.environ.get(
    "MOEBIUS_CONTRACT",
    str(Path.home() / "wuerth" / "moebius-orchestrator" / "contracts" / "pipeline_result.json")))
# What one cross-referenced line may cost in the payload, on average. A 189-part board came
# back at 691,751 characters before the payload was made compact (~3,700 per line), and
# clients refuse results around 285k; compact, the same board is ~131k (~700 per line).
XREF_CHARS_PER_LINE = 800
BOM_CHARS_PER_LINE = 300
# Every tool the server exposes. The HTTP check compares against this set, so a tool that is
# added or lost is noticed rather than counted.
TOOLS = {"faraday_capabilities", "review_board", "fetch_report", "list_findings",
         "explain_finding", "extract_bom", "crossref_board", "crossref_line"}


def check(label: str, condition: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if condition else 'FAIL'}  {label}" + (f" — {detail}" if detail else ""))
    if not condition:
        FAILURES.append(label)


def text(result) -> str:
    return "\n".join(c.text for c in result.content)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def check_http(port: int, review_dir: str) -> None:
    from mcp import ClientSession
    from mcp.client.streamable_http import streamablehttp_client

    env = {**os.environ, "FARADAY_MCP_PORT": str(port), "FARADAY_REVIEW_DIR": review_dir}
    proc = subprocess.Popen([sys.executable, "server.py"], cwd=str(_HERE), env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.time() + 60
        while time.time() < deadline:
            if proc.poll() is not None:
                check("the HTTP transport starts", False,
                      f"exited {proc.returncode}: {(proc.stderr.read() or '')[-300:]}")
                return
            with socket.socket() as s:
                s.settimeout(0.5)
                if s.connect_ex(("127.0.0.1", port)) == 0:
                    break
            time.sleep(0.5)
        else:
            check("the HTTP transport starts", False, "never bound its port")
            return

        async def drive():
            async with streamablehttp_client(f"http://127.0.0.1:{port}/mcp") as (r, w, _):
                async with ClientSession(r, w) as session:
                    await session.initialize()
                    names = [t.name for t in (await session.list_tools()).tools]
                    with_ui = {t.name for t in (await session.list_tools()).tools
                               if (t.meta or {}).get("ui/resourceUri")}
                    resources = (await session.list_resources()).resources
                    body = (await session.read_resource(resources[0].uri)).contents[0].text
                    out = await session.call_tool(
                        "review_board", {"board": str(BOARD), "stackup": "default-2layer"})
                    review = (out.structuredContent or {}).get("review")
                    report = await session.call_tool(
                        "fetch_report", {"review": review, "path": "board"}) if review else None
                    return names, with_ui, len(body), out, report

        names, with_ui, widget_len, out, report = asyncio.run(drive())
        check("the HTTP transport serves the whole tool surface", set(names) == TOOLS,
              f"{len(names)}: " + ", ".join(names))
        check("the board widget is on every tool that returns findings",
              with_ui == {"review_board", "list_findings", "explain_finding"},
              ", ".join(sorted(with_ui)))
        check("the widget is served over MCP", widget_len > 50_000, f"{widget_len:,} chars")
        sc = out.structuredContent or {}
        board_doc = ((report.structuredContent or {}).get("document") or {}) if report else {}
        check("a review over HTTP returns its findings, and fetch_report the board",
              sc.get("reported", 0) > 0 and len(board_doc.get("segments") or []) > 100,
              f"{sc.get('reported')} findings, "
              f"{len(board_doc.get('segments') or [])} segments")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:                        # pragma: no cover
            proc.kill()


def check_bom(S, validator) -> None:
    """extract_bom, crossref_board and crossref_line on two boards in two formats."""

    def valid(label: str, payload: dict) -> None:
        errors = [f"{'/'.join(map(str, e.absolute_path))}: {e.message[:160]}"
                  for e in validator.iter_errors(payload)]
        check(f"{label} validates against the pipeline contract", not errors,
              "; ".join(errors[:3]) or MOEBIUS_CONTRACT.name)

    # FIRST, before any call has started the long-lived worker: once it is running, the
    # variable is no longer read, and this check would pass for the wrong reason.
    print("crossref_board without KELVIN_SHARD_DIR")
    check("no worker is running yet (so the next check is real)", S._xref_proc is None)
    shards = os.environ.pop("KELVIN_SHARD_DIR")
    try:
        S.crossref_board(str(ODB_BOARD))
        check("an unset KELVIN_SHARD_DIR is refused, by name", False, "it answered")
    except ValueError as error:
        check("an unset KELVIN_SHARD_DIR is refused, by name",
              "KELVIN_SHARD_DIR is not set" in str(error), str(error)[:90])
    finally:
        os.environ["KELVIN_SHARD_DIR"] = shards

    print(f"extract_bom({BOM_BOARD.name})")
    r = S.extract_bom(str(BOM_BOARD))
    bom = r.structuredContent
    valid("the extracted BOM", bom)
    by_ref = {line["ref"]: line for line in bom["lines"]}
    check("every position is a line, and `total` counts them",
          bom["mode"] == "bom" and bom["total"] == len(bom["lines"]) > 200,
          f"{bom['total']} lines")
    check("`sourced` counts the lines that name a part",
          bom["sourced"] == sum(1 for line in bom["lines"] if line["mpn"]) > 100,
          f"{bom['sourced']} of {bom['total']}")
    known = [line for line in bom["lines"] if line["mpn"] == KNOWN_MPN]
    check(f"a part number the export carries comes back as stated ({KNOWN_MPN})",
          bool(known) and all(line["status"] == "exact" for line in known),
          f"{len(known)} line(s)")
    bare = [line for line in bom["lines"] if not line["mpn"]]
    check("a line with no part number is unsourced, and says why",
          bool(bare) and all(line["status"] == "unsourced"
                             and "no part number" in line.get("notes", "") for line in bare),
          f"{len(bare)} line(s), e.g. {bare[0]['ref'] if bare else '-'}")
    per_line = len(json.dumps(bom)) / max(1, bom["total"])
    check(f"the BOM payload stays small (<= {BOM_CHARS_PER_LINE} chars per line)",
          per_line <= BOM_CHARS_PER_LINE, f"{len(json.dumps(bom)):,} chars, {per_line:.0f}/line")

    print(f"crossref_board({BOM_BOARD.name})")
    r = S.crossref_board(str(BOM_BOARD))
    xref = r.structuredContent
    valid("the cross-reference", xref)
    check("the cross-reference answers for every line the BOM has",
          [line["ref"] for line in xref["lines"]] == [line["ref"] for line in bom["lines"]],
          f"{xref['total']} lines")
    hits = [line for line in xref["lines"] if line.get("originalMpn") == KNOWN_MPN]
    check(f"{KNOWN_MPN} is identified exactly, as YAGEO's resistor",
          bool(hits) and all(line["_identification"]["match"] == "exact"
                             and line.get("_originalManufacturer") == "YAGEO"
                             and line.get("kind") == "resistor" for line in hits),
          json.dumps(hits[0]["_identification"]) if hits else "not found")
    ranked = hits[0].get("candidates") or [] if hits else []
    check(f"{KNOWN_MPN} is cross-referenced: ranked candidates, the best one on the line",
          bool(ranked) and all(c.get("status") for c in ranked)
          and hits[0]["status"] == ranked[0]["status"]
          and (hits[0]["mpn"] == ranked[0]["mpn"]
               if ranked[0]["status"] in ("recommended", "partial") else True),
          f"{hits[0]['status']} -> {hits[0]['mpn']}" if hits else "-")
    missing = [line for line in xref["lines"] if line.get("originalMpn") == UNKNOWN_MPN]
    check(f"{UNKNOWN_MPN} (not in the catalogue) is unsourced, with the reason",
          bool(missing) and all(line["status"] == "unsourced" and line["mpn"] is None
                                and "not in the catalogue" in line.get("notes", "")
                                for line in missing),
          (missing[0].get("notes") or "")[:80] if missing else "not found")
    check("no line is left without a reason when nothing was sourced",
          all(line.get("notes") for line in xref["lines"] if line["status"] == "unsourced"))
    check("`sourced` counts the lines carrying a substitute",
          xref["sourced"] == sum(1 for line in xref["lines"] if line["mpn"]))
    size = len(json.dumps(xref))
    check(f"the cross-reference payload stays compact (<= {XREF_CHARS_PER_LINE} chars per line)",
          size / max(1, xref["total"]) <= XREF_CHARS_PER_LINE,
          f"{size:,} chars, {size / max(1, xref['total']):.0f}/line")
    check("what the compact payload holds back is said in its caveat",
          "COMPACT" in xref["caveat"] and "crossref_line" in xref["caveat"])

    print("crossref_line")
    handle = xref["caveat"].split("crossref_line(crossref='")[1].split("'")[0]
    check("the digest names the same handle as the caveat", f"crossref {handle}" in text(r))
    if hits:
        r = S.crossref_line(handle, hits[0]["ref"])
        line = r.structuredContent
        valid("one line in full", line)
        full = line["lines"][0]["candidates"]
        check("the full line carries every ranked candidate with its spec table and checks",
              len(full) >= len(ranked) and all(c.get("specs") and c.get("params") for c in full),
              f"{len(full)} candidates")
        check("the full line and the compact one agree on the answer",
              line["lines"][0]["status"] == hits[0]["status"]
              and line["lines"][0]["mpn"] == hits[0]["mpn"]
              and [c["mpn"] for c in full][:len(ranked)] == [c["mpn"] for c in ranked])
    try:
        S.crossref_line(handle, "NOPE999")
        check("an unknown reference is refused", False)
    except ValueError as error:
        check("an unknown reference is refused", "no line" in str(error))
    try:
        S.crossref_line("deadbeefcafe", "R1")
        check("an unknown cross-reference is refused", False)
    except ValueError as error:
        check("an unknown cross-reference is refused", "no cross-reference" in str(error))

    print(f"extract_bom / crossref_board({ODB_BOARD.name})  (ODB++)")
    bom = S.extract_bom(str(ODB_BOARD)).structuredContent
    valid("the ODB++ BOM", bom)
    check("the ODB++ job's one resistor is read, with no part number",
          [(line["ref"], line["status"], line["mpn"]) for line in bom["lines"]]
          == [("R1", "unsourced", None)], json.dumps(bom["lines"])[:120])
    xref = S.crossref_board(str(ODB_BOARD)).structuredContent
    valid("the ODB++ cross-reference", xref)
    line = xref["lines"][0] if xref["lines"] else {}
    check("a value that fits no catalogue part is unsourced, with the reason",
          line.get("status") == "unsourced" and line.get("mpn") is None
          and line["_identification"]["match"] == "none" and line.get("notes"),
          line.get("notes", "")[:80])


def main() -> int:
    # Preconditions, refused rather than skipped: the BOM checks are half of what this runs.
    if not os.environ.get("KELVIN_SHARD_DIR", "").strip():
        print("KELVIN_SHARD_DIR is not set — point it at Kelvin's web/public/kelvin (the "
              "catalogue shards crossref_board reads). Refusing to run without it.")
        return 2
    if not MOEBIUS_CONTRACT.exists():
        print(f"the pipeline contract is not at {MOEBIUS_CONTRACT} — set MOEBIUS_CONTRACT. "
              f"Refusing to run without it: every payload is validated against it.")
        return 2
    import jsonschema
    validator = jsonschema.Draft202012Validator(
        json.loads(MOEBIUS_CONTRACT.read_text(encoding="utf-8")))

    reviews = Path(tempfile.mkdtemp(prefix="faraday-smoke-"))
    os.environ["FARADAY_REVIEW_DIR"] = str(reviews)

    import server as S                                            # after the env is set

    try:
        for board in (BOARD, BOM_BOARD, ODB_BOARD):
            if not board.exists():
                print(f"no board at {board}")
                return 2

        print("faraday_capabilities")
        r = S.faraday_capabilities()
        families = r.structuredContent["families"]
        kinds = {f["kind"] for f in families}
        check("the formats it reads are named",
              len([f for f in families if f["kind"] == "format"]) >= 5,
              ", ".join(f["name"] for f in families if f["kind"] == "format"))
        check("rules, formats, stackups and severities are each named as such",
              kinds == {"rule", "format", "stackup", "severity"}, ", ".join(sorted(kinds)))
        check("the digest says the board stays local", "never leaves" in text(r))

        print("review_board without a stackup the file does not carry")
        try:
            S.review_board(str(BOARD))
            check("a board with no stackup is refused, not assumed", False)
        except ValueError as error:
            check("a board with no stackup is refused, not assumed",
                  "stackup" in str(error) and "2layer" in str(error), str(error)[:90])

        print("review_board(mppt-1210-hus, default-2layer)")
        r = S.review_board(str(BOARD), stackup="default-2layer")
        payload = r.structuredContent
        review = payload["review"]
        findings = payload["findings"]               # the contract projection, for consumers
        # The engine's own report is NOT in the payload (860k characters on a real board, which
        # a client refuses); the widget fetches it by the review id, and so does this check.
        check("the payload does not carry the engine report",
              "document" not in payload["subject"], ", ".join(payload["subject"]))
        report = S.fetch_report(review).structuredContent["document"]
        check("the payload is a `findings` result", payload["mode"] == "findings")
        check("it says it is a screening estimate", payload["provisional"] is True)
        check("findings came back", len(findings) > 10, f"{len(findings)} findings")
        check("every finding carries a rule, a severity, a tier and a mechanism",
              all(f.get("rule") and f.get("severity") and f.get("confidence") and f.get("detail")
                  for f in findings))
        check("the board itself came back for the widget",
              len(report["board"].get("segments") or []) > 100
              and len(report["board"].get("nets") or []) > 10,
              f"{len(report['board']['segments'])} segments, {len(report['board']['nets'])} nets")
        check("the payload's findings are the head of the report's, in the same order",
              [f["id"] for f in report["findings"]][:len(findings)]
              == [f["id"] for f in findings])
        check("the severity tally describes the whole review, not the payload",
              sum(payload["counts"].values()) == len(report["findings"]),
              json.dumps(payload["counts"]))
        check("`reported` is what the payload actually carries",
              payload["reported"] == len(findings))
        held = len(report["findings"]) - len(findings)
        check("findings held back from the payload are said to be",
              not held or (str(held) in payload.get("caveat", "")
                           and "list_findings" in payload["caveat"]),
              f"{held} held back")
        # Units beside the values, not inside the names: `coupledLenMm` cannot be reported in
        # mils without renaming the field, which is why the contract forbids it.
        coupled = next((f["metrics"]["coupledLength"] for f in findings
                        if "coupledLength" in (f.get("metrics") or {})), None)
        check("numbers carry their unit beside them",
              coupled is not None and coupled["unit"] == "mm" and isinstance(coupled["value"], float),
              json.dumps(coupled))
        # A finding that names its nets by INDEX names nothing to an engineer.
        # Over the WHOLE review (list_findings, unlimited): the payload carries only the head,
        # and the switch node need not be in it.
        # It is also the check that every finding of the review can cross the contract at all:
        # a finding the projection refuses is a finding no tool can return.
        try:
            every = S.list_findings(review, limit=10_000).structuredContent["findings"]
            check("list_findings with no filter returns the whole review",
                  len(every) == len(report["findings"]), f"{len(every)} findings")
        except ValueError as error:
            every = []
            check("list_findings with no filter returns the whole review", False,
                  str(error)[:220])
        # The switch node's own findings, by the net filter — a set that does not depend on
        # every other finding of the review crossing the contract.
        on_sw = S.list_findings(review, net="SW_NODE", limit=10_000).structuredContent["findings"]
        named = [n["name"] for f in every + on_sw for n in f.get("involves", [])
                 if n["kind"] == "net"]
        check("nets are named, never indexed",
              any("SW_NODE" in n for n in named) and not any(n.lstrip("-").isdigit() for n in named),
              ", ".join(sorted(set(named))[:3]))
        check("findings are pinned to the copper",
              sum(1 for f in findings if f.get("location")) > len(findings) // 2,
              f"{sum(1 for f in findings if f.get('location'))} of {len(findings)} located")
        # A review that returns 200 findings while dropping 228 more reads as complete.
        dropped = (report.get("meta") or {}).get("droppedByFindingCap") or 0
        check("findings dropped by the cap are reported, not hidden",
              not dropped or "dropped by the per-report cap" in text(r),
              f"{dropped} dropped")
        check("what was dropped is a FIELD, not only prose",
              sum(d["count"] for d in payload["dropped"]) >= dropped
              and all(d["reason"] for d in payload["dropped"]),
              json.dumps(payload["dropped"]))
        check("a converter board finds its commutation loop or switch node",
              any(f["rule"] in ("commutation-loop", "switch-node") for f in report["findings"]),
              ", ".join(sorted({f["rule"] for f in report["findings"]})[:6]))
        # RULES is hand-maintained from Screener.hpp and is what capabilities advertises, so
        # it has to be caught drifting rather than quietly under-reporting what Faraday screens.
        unlisted = sorted({f["rule"] for f in report["findings"]} - set(S.RULES))
        check("every rule the engine fired is one capabilities advertises",
              not unlisted, ", ".join(unlisted) or f"{len(S.RULES)} rules listed")

        # And against the engine's source: a rule no corpus board fires is still one Faraday
        # screens for, and capabilities must name it.
        import re
        in_source = set()
        for header in (_REPO / "cpp" / "include" / "faraday").rglob("*.hpp"):
            in_source |= set(re.findall(r'\.rule\s*=\s*"([a-z0-9-]+)"',
                                        header.read_text(encoding="utf-8")))
        check("capabilities names exactly the rules the engine's source can fire",
              len(in_source) > 10 and in_source == set(S.RULES),
              f"missing {sorted(in_source - set(S.RULES))}, extra {sorted(set(S.RULES) - in_source)}")

        print("list_findings")
        r = S.list_findings(review, severity="high", limit=5)
        high = r.structuredContent
        check("filtering by severity keeps only that severity",
              all(f["severity"] == "high" for f in high["findings"]),
              f"{high['reported']} shown")
        check("the payload carries exactly the filtered set, and counts it",
              len(high["findings"]) == high["reported"] == 5
              and "document" not in high["subject"])
        check("what the limit cut off is reported as dropped",
              any("limit" in d["reason"] for d in high["dropped"]),
              json.dumps(high["dropped"]))
        rule = findings[0]["rule"]
        r = S.list_findings(review, rule=rule, limit=100)
        check(f"filtering by rule '{rule}' works", r.structuredContent["reported"] > 0,
              f"{r.structuredContent['reported']} findings")
        try:
            S.list_findings(review, rule="not-a-rule")
            check("an unknown rule is refused, with the real ones named", False)
        except ValueError as error:
            check("an unknown rule is refused, with the real ones named", "screened" in str(error))
        try:
            S.list_findings(review, severity="catastrophic")
            check("an unknown severity is refused", False)
        except ValueError as error:
            check("an unknown severity is refused", "unknown severity" in str(error))

        print("explain_finding")
        target = next(f for f in findings if f["severity"] == "high")
        r = S.explain_finding(review, target["id"])
        check("the finding is explained in full",
              target["detail"][:40] in text(r) and target["summary"] in text(r))
        check("the remediation is carried", "Remediation:" in text(r))
        check("the payload carries that one finding, for the widget to pin",
              [f["id"] for f in r.structuredContent["findings"]] == [target["id"]]
              and r.structuredContent["reported"] == 1)
        try:
            S.explain_finding(review, "F-9999")
            check("an unknown finding id is refused", False)
        except ValueError as error:
            check("an unknown finding id is refused", "no finding" in str(error))

        print("a review that was never run here")
        try:
            S.list_findings("deadbeefcafe")
            check("an unknown review is refused, not answered empty", False)
        except ValueError as error:
            check("an unknown review is refused, not answered empty", "no review" in str(error))

        print("the review persists on disk, so a restart can still answer for it")
        check("the report was written", (reviews / review / "report.json").exists())
        check("what was reviewed is recorded beside it",
              json.loads((reviews / review / "meta.json").read_text())["board"] == str(BOARD))

        check_bom(S, validator)

        print("the MCP Apps widget")
        S.assert_widgets_resolve()
        widget = S.board_widget()
        check("the bundle is self-contained HTML",
              widget.lstrip().startswith("<") and "<script" in widget, f"{len(widget):,} bytes")
        check("no external fetch in the widget (deny-by-default CSP)",
              'src="http' not in widget and "src='http" not in widget)
        check("the widget carries the web app's own board renderer",
              "boardpane" in widget and "faraday" in widget.lower())

        if SKIP_HTTP:
            print("HTTP transport: SKIPPED (--skip-http)")
        else:
            print("the streamable-HTTP transport")
            check_http(free_port(), str(reviews))

        print()
        if FAILURES:
            print(f"{len(FAILURES)} FAILED: " + "; ".join(FAILURES))
            return 1
        print("all smoke checks passed")
        return 0
    finally:
        shutil.rmtree(reviews, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
