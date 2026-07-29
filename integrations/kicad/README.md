# Faraday KiCad plugin

Adds a **Review in Faraday** button to pcbnew. It serves the current board file
from a localhost-only HTTP server and opens
`faraday.openconverters.com/#load=<localhost url>` — the browser fetches the
board from your own machine and analyses it in WASM. Nothing is uploaded.

## Install (manual, until the PCM submission is accepted)

Copy `plugins/` into your KiCad plugin directory
(`~/.local/share/kicad/<ver>/scripting/plugins/faraday/` on Linux,
`Documents/KiCad/<ver>/scripting/plugins/faraday/` on Windows) and refresh
plugins in pcbnew.

## Package for the PCM

```
cd integrations/kicad && ./build_pcm.sh    # produces faraday-pcm.zip
```

Submitting to the official KiCad PCM repository is an external review process
(https://gitlab.com/kicad/addons/metadata) — the zip and metadata here are in
the required format, but the submission itself is a manual step.

**Status: exercised headless against real pcbnew 9.0.7** — `test_plugin.py`
(run it with `flatpak run --command=python3 org.kicad.KiCad
integrations/kicad/test_plugin.py <board>`) loads a real board through
pcbnew, runs the plugin, and asserts the served file is byte-identical to
the board on disk with the CORS header present, the opened URL has the
`#load=` shape, and a second click replaces the server instead of leaking
one. The `#load=` browser side is covered by Faraday's own e2e suite. The
one thing still unexercised is the toolbar button inside the live GUI —
that part is declarative (`defaults()`) and has no headless equivalent.
