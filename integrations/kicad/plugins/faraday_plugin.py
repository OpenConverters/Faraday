# Faraday bridge for KiCad (pcbnew action plugin).
#
# One button: "Review in Faraday". It serves the CURRENT board file from a
# localhost-only HTTP server (with CORS, so the site may fetch it) and opens
# faraday.openconverters.com/#load=<that url>. The analysis then runs entirely
# in the browser's WASM engine — the board goes browser <- localhost, never to
# any server, so the privacy property of the tool is unchanged.
import http.server
import os
import threading
import webbrowser

import pcbnew  # type: ignore

FARADAY_URL = "https://faraday.openconverters.com"
_server = None


class _OneFileHandler(http.server.BaseHTTPRequestHandler):
    board_path = ""

    def do_GET(self):  # noqa: N802 (http.server API)
        try:
            with open(self.board_path, "rb") as f:
                data = f.read()
        except OSError:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        # The browser at faraday.openconverters.com fetches from localhost;
        # without CORS that fetch is blocked. Localhost-bound, single file.
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):  # keep the pcbnew console quiet
        pass


class FaradayPlugin(pcbnew.ActionPlugin):
    def defaults(self):
        self.name = "Review in Faraday"
        self.category = "EMC"
        self.description = (
            "Open the current board in Faraday's browser-local EMC review. "
            "The board is served from localhost only; nothing is uploaded."
        )
        self.show_toolbar_button = True
        self.icon_file_name = os.path.join(os.path.dirname(__file__), "icon.png")

    def Run(self):  # noqa: N802 (pcbnew API)
        global _server
        board = pcbnew.GetBoard()
        path = board.GetFileName()
        if not path or not os.path.exists(path):
            wx_error("Save the board first — Faraday reads the .kicad_pcb file.")
            return
        if _server is not None:
            _server.shutdown()
            _server = None
        _OneFileHandler.board_path = path
        _server = http.server.HTTPServer(("127.0.0.1", 0), _OneFileHandler)
        threading.Thread(target=_server.serve_forever, daemon=True).start()
        port = _server.server_address[1]
        name = os.path.basename(path)
        webbrowser.open(
            f"{FARADAY_URL}/#load=http://127.0.0.1:{port}/{name}"
        )


def wx_error(msg):
    try:
        import wx  # type: ignore

        wx.MessageBox(msg, "Faraday", wx.ICON_ERROR)
    except Exception:
        print(f"Faraday: {msg}")
