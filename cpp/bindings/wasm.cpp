// Faraday WASM engine (embind). The browser calls analyze() with the raw
// .kicad_pcb text; nothing ever leaves the machine. stackup_name selects a
// built-in stackup ("" = use the board file's own; the importer refuses when
// neither exists — the GUI surfaces that as the stackup card).
//
// solvePair() runs the deep tier on ONE cross-section: a 2D boundary-element
// field extraction followed by a transient of the coupled pair. Both are
// milliseconds, and both run HERE, in the page, on the same principle as the
// screener — the board never leaves the machine and there is no server to be
// down. It is the same code path the native CLI uses.
//
// Errors are returned as {"error": "..."} JSON rather than thrown across the
// embind boundary, so the UI can show the exact refusal message.

#include <emscripten/bind.h>

#include <faraday/Bench.hpp>
#include <faraday/Import.hpp>
#include <faraday/Screener.hpp>

// The last imported board. radiationMap() re-runs over the WHOLE board on every
// slider move, and re-parsing a 4 MB layout each time would cost half a second;
// holding the IR makes it a few milliseconds. One board at a time is exactly
// what the UI does, so the state is bounded and its lifetime is obvious.
static std::optional<faraday::BoardIR> g_board;

static std::string analyze(std::string board_text, std::string stackup_name) {
    try {
        std::optional<faraday::Stackup> user;
        if (!stackup_name.empty()) user = faraday::builtin_stackup(stackup_name);
        faraday::BoardFormat fmt;
        faraday::BoardIR board =
            faraday::import_board(board_text, std::move(user), &fmt);
        nlohmann::json out = faraday::analyze_board(board);
        out["format"] = faraday::format_name(fmt);
        g_board = std::move(board);
        return out.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string solve_pair(std::string request_json) {
    try {
        const nlohmann::json j = nlohmann::json::parse(request_json);
        return faraday::bench::run(faraday::bench::request_from_json(j)).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string predict_emissions(std::string request_json) {
    try {
        return faraday::bench::predict_emissions(
                   nlohmann::json::parse(request_json)).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string cm_budget(std::string request_json) {
    try {
        return faraday::bench::cm_budget_json(
                   nlohmann::json::parse(request_json)).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string return_path(std::string request_json) {
    try {
        if (!g_board)
            throw std::runtime_error(
                "no board loaded — open a layout before asking where the "
                "returns flow");
        const nlohmann::json j = nlohmann::json::parse(request_json);
        faraday::Screener sc(*g_board);
        return faraday::bench::return_path_json(
                   *g_board, sc, faraday::bench::rp_params_from_json(j)).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string near_field(std::string request_json) {
    try {
        if (!g_board)
            throw std::runtime_error(
                "no board loaded — open a layout before asking what the field "
                "above it looks like");
        const nlohmann::json j = nlohmann::json::parse(request_json);
        faraday::Screener sc(*g_board);
        return faraday::bench::near_field_json(
                   *g_board, sc, faraday::bench::nfmap_params_from_json(j)).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string shielding(std::string request_json) {
    try {
        return faraday::bench::shielding_json(
                   nlohmann::json::parse(request_json)).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string shield_materials() {
    return faraday::bench::shield_materials_json().dump();
}

static std::string victim_classes() {
    return faraday::bench::victim_classes_json().dump();
}

static std::string pdn_map(std::string request_json) {
    try {
        if (!g_board)
            throw std::runtime_error(
                "no board loaded — open a layout before asking about its PDN");
        const nlohmann::json j = nlohmann::json::parse(request_json);
        faraday::Screener sc(*g_board);
        return faraday::bench::pdn_json(*g_board, sc, j).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string limit_lines() { return faraday::bench::limit_lines_json().dump(); }

static std::string logic_families() {
    return faraday::bench::families_json().dump();
}

static std::string version() { return "0.2.0"; }

EMSCRIPTEN_BINDINGS(faraday) {
    emscripten::function("analyze", &analyze);
    emscripten::function("solvePair", &solve_pair);
    emscripten::function("predictEmissions", &predict_emissions);
    emscripten::function("cmBudget", &cm_budget);
    emscripten::function("returnPath", &return_path);
    emscripten::function("pdn", &pdn_map);
    emscripten::function("nearField", &near_field);
    emscripten::function("victimClasses", &victim_classes);
    emscripten::function("shielding", &shielding);
    emscripten::function("shieldMaterials", &shield_materials);
    emscripten::function("limitLines", &limit_lines);
    emscripten::function("logicFamilies", &logic_families);
    emscripten::function("version", &version);
}
