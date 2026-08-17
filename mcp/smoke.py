"""End-to-end smoke test for the Faraday MCP server — every tool, on a real board.

Not a unit test: it screens boards from the corpus with the real engine and asserts the
answers are the engine's, then starts the HTTP transport and drives it with a real MCP
client. The point is that a broken tool fails HERE rather than in front of an FAE.

    python3 mcp/smoke.py [--skip-http]
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
                    return names, with_ui, len(body), out

        names, with_ui, widget_len, out = asyncio.run(drive())
        check("the HTTP transport serves the whole tool surface", len(names) == 4,
              ", ".join(names))
        check("the board widget is on every tool that returns findings",
              with_ui == {"review_board", "list_findings", "explain_finding"},
              ", ".join(sorted(with_ui)))
        check("the widget is served over MCP", widget_len > 50_000, f"{widget_len:,} chars")
        sc = out.structuredContent or {}
        check("a review over HTTP returns the board and its findings",
              sc.get("reported", 0) > 0
              and "board" in ((sc.get("subject") or {}).get("document") or {}),
              f"{sc.get('reported')} findings")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:                        # pragma: no cover
            proc.kill()


def main() -> int:
    reviews = Path(tempfile.mkdtemp(prefix="faraday-smoke-"))
    os.environ["FARADAY_REVIEW_DIR"] = str(reviews)

    import server as S                                            # after the env is set

    try:
        if not BOARD.exists():
            print(f"no corpus board at {BOARD}")
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
        report = payload["subject"]["document"]      # the engine's own report, for the widget
        findings = payload["findings"]               # the contract projection, for consumers
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
        check("the drawing and the list hold the same findings, in the same order",
              [f["id"] for f in report["findings"]] == [f["id"] for f in findings])
        check("the severity tally matches the findings",
              sum(payload["counts"].values()) == len(findings), json.dumps(payload["counts"]))
        check("`reported` is what the payload actually carries",
              payload["reported"] == len(findings))
        # Units beside the values, not inside the names: `coupledLenMm` cannot be reported in
        # mils without renaming the field, which is why the contract forbids it.
        coupled = next((f["metrics"]["coupledLength"] for f in findings
                        if "coupledLength" in (f.get("metrics") or {})), None)
        check("numbers carry their unit beside them",
              coupled is not None and coupled["unit"] == "mm" and isinstance(coupled["value"], float),
              json.dumps(coupled))
        # A finding that names its nets by INDEX names nothing to an engineer.
        named = [n["name"] for f in findings for n in f.get("involves", []) if n["kind"] == "net"]
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
              any(f["rule"] in ("commutation-loop", "switch-node") for f in findings),
              ", ".join(sorted({f["rule"] for f in findings})[:6]))
        # RULES is hand-maintained from Screener.hpp and is what capabilities advertises, so
        # it has to be caught drifting rather than quietly under-reporting what Faraday screens.
        unlisted = sorted({f["rule"] for f in findings} - set(S.RULES))
        check("every rule the engine fired is one capabilities advertises",
              not unlisted, ", ".join(unlisted) or f"{len(S.RULES)} rules listed")

        print("list_findings")
        r = S.list_findings(review, severity="high", limit=5)
        high = r.structuredContent
        check("filtering by severity keeps only that severity",
              all(f["severity"] == "high" for f in high["findings"]),
              f"{high['reported']} shown")
        check("the widget gets exactly the filtered set",
              [f["id"] for f in high["subject"]["document"]["findings"]]
              == [f["id"] for f in high["findings"]] and high["reported"] == 5)
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
        check("the widget gets the board with that one finding pinned",
              [f["id"] for f in r.structuredContent["subject"]["document"]["findings"]]
              == [target["id"]] and r.structuredContent["reported"] == 1)
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
