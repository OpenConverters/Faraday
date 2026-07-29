#!/usr/bin/env python3
"""Exercise the Faraday plugin against REAL pcbnew, headless.

Run inside KiCad's own Python:
    flatpak run --command=python3 org.kicad.KiCad integrations/kicad/test_plugin.py <board.kicad_pcb>

What this proves: the ActionPlugin subclass registers against the real API,
Run() serves the real board file from a localhost-only CORS server, and the
opened URL is faraday.openconverters.com/#load=<that server>. What it cannot
prove: the toolbar button inside the pcbnew GUI — that part is API-declarative
(defaults()) and has no headless equivalent.

The GUI-only seams are patched, nothing else: pcbnew.GetBoard() does not
exist outside the editor, so it is pointed at a board loaded with the real
pcbnew.LoadBoard(); webbrowser.open is captured instead of spawning a
browser.
"""
import sys
import urllib.request

import pcbnew  # noqa: F401  (the real thing, or this test means nothing)

sys.path.insert(0, __file__.rsplit("/", 1)[0] + "/plugins")

board_path = sys.argv[1]
board = pcbnew.LoadBoard(board_path)
pcbnew.GetBoard = lambda: board          # the editor's accessor, headless

opened = []
import webbrowser
webbrowser.open = lambda url: opened.append(url)

import faraday_plugin
plugin = faraday_plugin.FaradayPlugin()
plugin.defaults()
assert plugin.name == "Review in Faraday", plugin.name
plugin.register()                        # the real ActionPlugin registry
plugin.Run()

assert len(opened) == 1, f"expected one browser open, got {opened}"
url = opened[0]
assert url.startswith("https://faraday.openconverters.com/#load=http://127.0.0.1:"), url

# the served file must be byte-identical to the board on disk, with CORS
served_url = url.split("#load=")[1]
with urllib.request.urlopen(served_url, timeout=5) as r:
    served = r.read()
    cors = r.headers.get("Access-Control-Allow-Origin")
assert cors == "*", f"CORS header missing/wrong: {cors}"
with open(board_path, "rb") as f:
    on_disk = f.read()
assert served == on_disk, (
    f"served {len(served)} bytes != on-disk {len(on_disk)}")

# a second Run() must replace the server, not leak one per click
plugin.Run()
assert len(opened) == 2
with urllib.request.urlopen(opened[1].split("#load=")[1], timeout=5) as r:
    assert r.read() == on_disk

print(f"OK: pcbnew {pcbnew.Version()}, {len(on_disk)} bytes served with CORS, "
      f"URL {url.split('#')[0]}#load=http://127.0.0.1:<port>/...")
