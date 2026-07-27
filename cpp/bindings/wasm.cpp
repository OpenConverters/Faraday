// Faraday WASM engine (embind). The browser calls analyze() with the raw
// .kicad_pcb text; nothing ever leaves the machine. stackup_name selects a
// built-in stackup ("" = use the board file's own; the importer refuses when
// neither exists — the GUI surfaces that as the stackup card).
//
// Errors are returned as {"error": "..."} JSON rather than thrown across the
// embind boundary, so the UI can show the exact refusal message.

#include <emscripten/bind.h>

#include <faraday/KicadImporter.hpp>
#include <faraday/Screener.hpp>

static std::string analyze(std::string kicad_text, std::string stackup_name) {
    try {
        std::optional<faraday::Stackup> user;
        if (!stackup_name.empty()) user = faraday::builtin_stackup(stackup_name);
        faraday::BoardIR board =
            faraday::import_kicad(kicad_text, std::move(user));
        return faraday::analyze_board(board).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string version() { return "0.1.0"; }

EMSCRIPTEN_BINDINGS(faraday) {
    emscripten::function("analyze", &analyze);
    emscripten::function("version", &version);
}
