# Faraday as an MCP App

Exposes Faraday's EMC layout review as [MCP](https://modelcontextprotocol.io) tools over
streamable HTTP, with the reviewed **board itself** as an
[MCP Apps](https://modelcontextprotocol.io/extensions/apps/build) (SEP-1865) UI resource —
every finding pinned to the copper it concerns, and clickable (ABT #664, #665).

A list of findings in text is usable. A board with the findings on it is what makes an FAE
trust the tool.

## The board does not leave the machine

Faraday's premise is that your layout is analysed locally, and this server is an **additional**
entry point, not a replacement:

- the analysis happens **here** — it runs the same `faraday_cli` an engineer would run by hand,
  and reports are written under `~/.faraday/reviews/` on this host,
- **this server never sends your board anywhere.** There is no upload path, no telemetry and no
  outbound call in the review itself.

One honest caveat, added when the artifact convention landed: a board can now be named by a
remote reference (`artifact://…`, `https://…`), in which case the server FETCHES it from the
place the caller named. That is an inbound fetch the caller asked for, not the board leaving —
but if you want the property to be absolute, pass local paths only and leave
`FARADAY_ARTIFACT_BASE` unset, and a remote reference is then refused rather than resolved.

Run it on the engineer's machine and the property holds exactly as it does for the browser
app. Run it centrally and the boards are on the central machine — that is a deployment
decision, not a property of this server, and it is the one thing to be deliberate about
before exposing the port.

## Run

```bash
cmake -S cpp -B build && cmake --build build -j     # the engine
cd mcp && npm install && npm run build && cd ..     # the widget
python3 mcp/server.py                               # 127.0.0.1:8407/mcp
```

Port 8407 continues the house sequence: Hertz 8400, Kirchhoff 8401, Kelvin 8402, Moebius
bridge 8404, Heaviside 8405, OMFEM 8406.

| Variable | Meaning | Default |
|---|---|---|
| `FARADAY_CLI` | the review engine this server drives | `build/faraday_cli` |
| `FARADAY_REVIEW_DIR` | where reports are kept | `~/.faraday/reviews` |
| `FARADAY_MCP_PORT` / `FARADAY_MCP_HOST` | where the transport binds | `8407` / `127.0.0.1` |
| `FARADAY_PUBLIC_HOST` / `FARADAY_ALLOW_ANY_HOST` / `FARADAY_ALLOWED_ORIGINS` | tunnel allowlisting | — |
| `KELVIN_SHARD_DIR` | Kelvin's catalogue: the directory holding `manifest.json`, `<family>.kidx` and `<family>.ndjson` (Kelvin's `web/public/kelvin`). **Required by `crossref_board`**, which refuses by name when it is unset — and must be *exported* into the server's environment | — (no default) |
| `KELVIN_WEB_SRC` | Kelvin's web sources, which `crossref_board`'s worker imports (`crossref.js`, `engine.js`) | `../Kelvin/web/src` beside this repo |

`crossref_board` also needs **node ≥ 22.15** on `PATH` (the worker uses `module.registerHooks`)
and Kelvin's built WASM at `<KELVIN_WEB_SRC>/../public/kelvin.js`. The other tools need
neither.

The host allowlist is built from the port actually bound, so moving the port does not produce
a bare `421 Invalid Host header` — which hosts routinely surface as a sign-in failure that
says nothing about the port.

## Tools

| Tool | Answers | Widget |
|---|---|---|
| `faraday_capabilities` | what it reads, what it screens, what a stackup is for | — |
| `review_board` | screen a layout: ranked findings + the board to look at | board |
| `list_findings` | filter a completed review by severity, rule or net | board |
| `explain_finding` | one finding in full: mechanism, numbers, confidence, remediation | board |
| `fetch_report` | a stored review's engine report, whole or by dotted path (what the widget draws) | — |
| `extract_bom(board, top=40)` | the board's parts as a BOM — ref, value, footprint, the part number the export carries; no stackup, no catalogue | — |
| `crossref_board(board, target_manufacturers=None, same_type=True, max_results=5, top=30)` | every part identified in Kelvin's catalogue and cross-referenced by Kelvin's ranker, the way the web app does it; compact per line | — |
| `crossref_line(crossref, ref)` | one cross-referenced line in full: every candidate's spec table, every check, every note | — |

Every payload is a result under the **Moebius pipeline contract**
(`contracts/pipeline_result.json`): `review_board`, `list_findings` and `explain_finding`
return `mode: "findings"`, `faraday_capabilities` returns `mode: "catalogue"`, `fetch_report`
returns `mode: "document"`, and `extract_bom` / `crossref_board` / `crossref_line` return
`mode: "bom"` — one line per position, keyed by reference designator. That means
units travel beside their values (`{"value": 489.3, "unit": "mm"}`, not `coupledLenMm`), nets
are named rather than indexed, the per-report cap is a `dropped` field rather than a sentence,
and one finding is a list of one rather than a shape of its own. The engine's own report does
NOT travel in the payload — on a real board it is ~860k characters, which clients refuse — the
widget fetches it with `fetch_report(review)`, and `review_board` carries only the findings its
digest names (`list_findings` has the rest; the `caveat` says how many).

### Parts: `extract_bom`, `crossref_board`, `crossref_line`

`extract_bom` is the CLI's `--components-out`: components only, no stackup. A line whose
export names a part is `exact`; one that names none is `unsourced` and says so in `notes`.

`crossref_board` runs the web app's own identification (`web/src/parts.js`) and Kelvin's
cross-reference in a long-lived node worker (`crossref.mjs`, with `kelvin-node.mjs` standing
in for Kelvin's browser engine), restarted when any source it loaded changes. Only lines
identified **exactly** by part number are cross-referenced; value-and-package or partial
part-number matches list catalogue rows unranked, and unidentifiable lines are `unsourced`
with the reason.

Its payload is **compact on purpose**: a 189-part board came back at 691,751 characters in
full and is ~131k compact (glasgow, 272 parts: ~141k). Each line keeps its identification,
the original, the best substitute and its status, the best candidate's failing checks and
notes (`_paramsPassed` counts the rest), one alternate's mpn/status/grade, and up to 3
unranked rows; spec tables, passed checks, the alternates' detail and the search trail are
held back, counted in `_candidatesHeldBack` / `_rowsHeldBack`, and named in `caveat`. The full
lines are stored under `FARADAY_REVIEW_DIR/crossref-<id>/`, and `crossref_line(crossref, ref)`
returns one exactly as the ranker left it — no recomputation, so it cannot disagree with the
summary. The handle is in the digest and in `caveat` (the contract's `bom` result has no field
for one).

Check it against the contract from this repo:

```bash
python3 ~/wuerth/moebius-orchestrator/scripts/conformance.py \
    http://127.0.0.1:8407/mcp --calls ~/wuerth/moebius-orchestrator/contracts/calls/faraday.json
```

A review takes **tens of milliseconds**, so these are ordinary blocking tools — no job queue.
(OMFEM's async contract exists because an FEA solve takes minutes to hours; a geometry screen
does not.) The report is still kept on disk per review, because a finding id has to mean the
same thing in `explain_finding` as it did in the list the caller is reading from.

Two refusals are deliberate:

- **A board with no stackup is refused, not assumed.** The dielectric decides every impedance
  and coupling number in the report, so the engine names the copper count and the builtin that
  fits (`default-2layer`) rather than guessing. The tool passes that message through verbatim.
- **An unknown rule or severity names the real ones.** A filter that silently matches nothing
  reads exactly like a clean board.

## What the digest will tell you that the report buries

The engine caps how many findings it reports. On the corpus MPPT board that is **200 returned
and 228 dropped** — so a naive summary of "200 findings" describes less than half of them.
The cap and the reporting floor are both stated in the first lines of every review:

```
200 finding(s) on mppt-1210-hus.kicad_pcb: 172 high, 13 medium, 13 low, 2 info
  — 88 nets, 1023 segments, stackup user:default-2layer
228 further finding(s) were dropped by the per-report cap — this list is the top of a longer one.
```

Findings reference their nets by **index**; the digest resolves them against the board's net
table, because `8 <-> 12` names nothing to an engineer.

## The widget

`ui://faraday/board.html` — the board drawn by the web app's own `BoardView.vue`, imported
from `../web/src` rather than reimplemented, so the board in a chat and the board in the
browser cannot disagree about what the copper looks like. Findings are pinned to their
geometry and selectable, in the drawing or in the list beside it; choosing one reports it back
to the model through `updateModelContext`, so the next question can be *"why is this a risk,
and which part fixes it"* about that finding.

Built single-file with `vite-plugin-singlefile`, because MCP App resources render in a
deny-by-default CSP iframe. `assert_widgets_resolve()` refuses to start if the bundle is
missing — advertising a UI the host cannot fetch is ABT #651's failure mode.

## Testing

```bash
KELVIN_SHARD_DIR=../Kelvin/web/public/kelvin python3 mcp/smoke.py [--skip-http]
```

`KELVIN_SHARD_DIR` and the Moebius contract (`MOEBIUS_CONTRACT`, default
`~/wuerth/moebius-orchestrator/contracts/pipeline_result.json`) are required; the run refuses
without them rather than skipping the checks that need them. Every BOM payload is validated
against the contract (Draft 2020-12).

Screens a real corpus board (the MPPT converter, 88 nets, 1023 segments) with the real engine
and asserts the answers are the engine's: that a converter board finds its commutation loop,
that the severity tally matches the findings, that the dropped-by-cap count is reported, that
filters refuse unknown rules and severities by naming the real ones, that a review persists
and an unknown one is refused rather than answered empty, that capabilities names exactly the
rules the engine's source can fire — then extracts and cross-references the parts of
`corpus/glasgow.kicad_pcb` (KiCad, MPN fields) and `cpp/tests/fixtures/odb_job.zip` (ODB++):
a catalogued part number is identified exactly and cross-referenced, one the catalogue lacks
is `unsourced` with the reason, an unset `KELVIN_SHARD_DIR` is refused by name, the compact
payload stays under 800 characters per line, and `crossref_line` agrees with it — then starts
the HTTP transport and drives it with a real MCP client.

## Not here

No TLS (terminate it at a reverse proxy), no rate limiting, and no board-editing tools:
Faraday reviews, it does not route.

## Files and auth

Anything this server reads from disk takes ONE reference argument, the shared convention across
the OpenConverters MCP servers (`mcp/artifacts.py`, identical in Faraday, Hertz and Kirchhoff):

```
/path/to/file            a path on the machine running the server
file:///path/to/file     the same, as a URI
artifact://<id>          resolved against FARADAY_ARTIFACT_BASE, with FARADAY_ARTIFACT_TOKEN as a bearer token
https://host/path        fetched as-is
```

One argument rather than a path field and a URI field, so the orchestrator implements the
reference once. Large inputs therefore never travel through the tool arguments — which is to
say, never through the model context.

Auth is **off unless `FARADAY_AUTH_TOKEN` is set**. Set it and every request must carry
`Authorization: Bearer <token>`, and one that does not gets a plain 401 rather than a redirect.
It is a gate, not an identity: one shared token says the caller is allowed in, not who they
are. Anything needing per-user identity, audit or revocation wants a real IdP in front, and
TLS belongs on the proxy.
