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
#include <faraday/Diff.hpp>
#include <faraday/Fixes.hpp>
#include <faraday/Report.hpp>
#include <faraday/Import.hpp>
#include <faraday/Screener.hpp>

// The last imported board. radiationMap() re-runs over the WHOLE board on every
// slider move, and re-parsing a 4 MB layout each time would cost half a second;
// holding the IR makes it a few milliseconds. One board at a time is exactly
// what the UI does, so the state is bounded and its lifetime is obvious.
static std::optional<faraday::BoardIR> g_board;
static faraday::BoardFormat g_fmt;
// Session screening options: candidate switch nodes the user has promoted.
// EVERY Screener built in this binding must carry them — the near-field map
// and bench functions key on switch nodes, and a promoted node that reaches
// the report but not the near-field button would recreate ABT #410.
static std::vector<std::string> g_user_switch_nets;
static faraday::ScreenerParams session_params() {
    faraday::ScreenerParams sp;
    sp.user_switch_nets = g_user_switch_nets;
    return sp;
}

// "no stackup" is the one error the UI answers rather than reports, so it
// travels with the copper count the importer already knows — the card then
// offers the single builtin that FITS the board instead of a 2-or-4 guess.
static std::string stackup_needed_json(const faraday::StackupNeeded& e) {
    return nlohmann::json{{"error", e.what()},
                          {"needStackup", true},
                          {"copperCount", e.copper_count}}
        .dump();
}

static std::string analyze(std::string board_text, std::string stackup_name) {
    try {
        // stackup_name: builtin name, or a full custom stackup as JSON
        std::optional<faraday::Stackup> user =
            faraday::resolve_stackup(stackup_name);
        faraday::BoardFormat fmt;
        faraday::BoardIR board =
            faraday::import_board(board_text, std::move(user), &fmt);
        g_user_switch_nets.clear();   // new board, new session
        g_fmt = fmt;
        nlohmann::json out = faraday::analyze_board(board);
        out["format"] = faraday::format_name(fmt);
        g_board = std::move(board);
        return out.dump();
    } catch (const faraday::StackupNeeded& e) {
        return stackup_needed_json(e);
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

// A Gerber board is a SET of files. The browser sends them as
// {"files":[{"name","text"},...],"stackup":"..."} — one call, one report,
// identical shape to analyze()'s.
static std::string analyze_set(std::string request_json) {
    try {
        const nlohmann::json j = nlohmann::json::parse(request_json);
        std::vector<faraday::gerber::NamedFile> files;
        for (const auto& f : j.at("files"))
            files.push_back({f.at("name").get<std::string>(),
                             f.at("text").get<std::string>()});
        std::optional<faraday::Stackup> user =
            faraday::resolve_stackup(j.value("stackup", ""));
        faraday::BoardFormat fmt;
        faraday::BoardIR board =
            faraday::import_board_set(files, std::move(user), &fmt);
        g_user_switch_nets.clear();   // new board, new session
        g_fmt = fmt;
        nlohmann::json out = faraday::analyze_board(board);
        out["format"] = faraday::format_name(fmt);
        g_board = std::move(board);
        return out.dump();
    } catch (const faraday::StackupNeeded& e) {
        return stackup_needed_json(e);
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

// Values the CAD export did not carry. Altium's ODB++ writes the manufacturer
// part number and no value at all, so a board full of real capacitors arrives
// with none — and the PDN, the input branch and the Y-capacitor rule all
// correctly refuse to invent them. This takes the refdes,value table back in;
// the caller then re-screens. partsWithoutValues() asks the question the table
// answers, so the browser can offer the same workflow the CLI has.
// The CATALOGUE's measured figures for the parts on this board, by refdes:
// [{"ref":"C12","mpn":"GRM188R71H104KA93D","cF":1e-7,"esrOhm":0.012,
//   "esrFreqHz":1e5,"eslH":5e-10}, ...]
//
// This is where an identified part stops being a label and starts changing the
// numbers. Faraday assumed 0.015 ohm of ESR for every capacitor on every board,
// and that constant reaches the conducted-emissions maths through Operating's
// input branch — so the noise estimate carried it. A part Kelvin has matched
// exactly publishes the real figure.
//
// Fields are taken INDIVIDUALLY: a part may publish an ESR and no ESL, and
// insisting on the pair would discard the half that is real. Anything absent,
// zero or non-finite is left alone rather than defaulted, and each branch
// reports which of its numbers ended up measured.
static std::string apply_part_data(std::string json_text) {
    try {
        if (!g_board)
            throw std::runtime_error("no board loaded");
        const nlohmann::json in = nlohmann::json::parse(json_text);
        if (!in.is_array())
            throw std::runtime_error(
                "applyPartData wants an array of {ref, ...} objects");
        size_t applied = 0, fields = 0;
        for (const auto& e : in) {
            if (!e.is_object() || !e.contains("ref")) continue;
            const std::string ref = e.at("ref").get<std::string>();
            if (ref.empty()) continue;
            faraday::values::PartData pd;
            auto num = [&](const char* k, double& dst) {
                if (e.contains(k) && e.at(k).is_number()) {
                    const double v = e.at(k).get<double>();
                    if (std::isfinite(v) && v > 0) { dst = v; ++fields; }
                }
            };
            num("cF", pd.c_f);
            num("esrOhm", pd.esr_ohm);
            num("esrFreqHz", pd.esr_freq_hz);
            num("eslH", pd.esl_h);
            num("cossF", pd.coss_f);
            num("qgC", pd.qg_c);
            num("rdsOnOhm", pd.rds_on_ohm);
            if (e.contains("mpn") && e.at("mpn").is_string())
                pd.mpn = e.at("mpn").get<std::string>();
            pd.source = "kelvin";
            g_board->part_data[ref] = pd;
            ++applied;
        }
        return nlohmann::json{{"parts", applied}, {"fields", fields}}.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string apply_values(std::string csv) {
    try {
        if (!g_board)
            throw std::runtime_error("no board loaded");
        faraday::values::ValueTable vt = faraday::values::parse_value_table(csv);
        const size_t applied = faraday::apply_values(*g_board, vt);
        return nlohmann::json{{"applied", applied},
                              {"ignored", vt.ignored},
                              {"offered", vt.by_refdes.size()}}
            .dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string parts_without_values() {
    try {
        if (!g_board)
            throw std::runtime_error("no board loaded");
        nlohmann::json out = nlohmann::json::array();
        for (const auto& [ref, part] : faraday::parts_without_values(*g_board))
            out.push_back({{"refdes", ref}, {"part", part}});
        return out.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

// Re-screen the LAST analyzed board with session options — the candidate-
// promotion path. No re-import: g_board IS the board the report came from.
// {"userSwitchNets":["NET", ...]} replaces the session's promoted set.
static std::string reanalyze(std::string options_json) {
    try {
        if (!g_board)
            return nlohmann::json{{"error", "no board loaded"}}.dump();
        const nlohmann::json j = nlohmann::json::parse(options_json);
        g_user_switch_nets.clear();
        for (const auto& n : j.value("userSwitchNets", nlohmann::json::array()))
            g_user_switch_nets.push_back(n.get<std::string>());
        nlohmann::json out = faraday::analyze_board(*g_board, session_params());
        out["format"] = faraday::format_name(g_fmt);
        return out.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

// Diff two full reports (baseline first). Same implementation the CLI gate
// uses — the UI and CI can never disagree about what counts as a regression.
static std::string diff_reports_js(std::string base_json, std::string cur_json) {
    try {
        return faraday::diff::diff_reports(nlohmann::json::parse(base_json),
                                           nlohmann::json::parse(cur_json))
            .dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

// Stitching-via fix: propose + apply on the ORIGINAL board text, returning a
// new .kicad_pcb (never mutating anything). KiCad boards only.
static std::string fix_stitching(std::string board_text, std::string stackup) {
    try {
        std::optional<faraday::Stackup> user = faraday::resolve_stackup(stackup);
        faraday::BoardIR board = faraday::import_board(board_text, std::move(user));
        faraday::Screener sc(board);
        faraday::fixes::StitchPlan plan = faraday::fixes::propose_stitching(board, sc);
        nlohmann::json out{{"unstitched", plan.unstitched_seen},
                           {"notes", plan.notes}};
        nlohmann::json vj = nlohmann::json::array();
        for (const auto& v : plan.vias)
            vj.push_back({{"x", v.x}, {"y", v.y}, {"net", v.net_name},
                          {"nearNet", v.near_net}, {"size", v.size},
                          {"drill", v.drill}});
        out["vias"] = vj;
        if (!plan.vias.empty())
            out["text"] = faraday::fixes::apply_stitching(board_text, board,
                                                          plan.vias);
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

// The input-capacitor branch derived from the LOADED board: the conducted
// panel asks for it once and stops asking the user for a capacitance the
// layout already contains.
static std::string input_branch() {
    try {
        if (!g_board)
            throw std::runtime_error(
                "no board loaded — the input capacitor branch is derived from "
                "the board's own parts");
        faraday::Screener sc(*g_board, session_params());
        return faraday::bench::input_branch_json(*g_board, sc).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string conducted_estimate_js(std::string request_json) {
    try {
        return faraday::bench::conducted_estimate(
                   nlohmann::json::parse(request_json))
            .dump();
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
        faraday::Screener sc(*g_board, session_params());
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
        faraday::Screener sc(*g_board, session_params());
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
        faraday::Screener sc(*g_board, session_params());
        return faraday::bench::pdn_json(*g_board, sc, j).dump();
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

static std::string limit_lines() { return faraday::bench::limit_lines_json().dump(); }

// The CONDUCTED limit lines (CISPR 32 mains, QP and AV) the verdict is judged
// against — the panel picks one, the same way it picks a radiated standard.
static std::string conducted_limits() {
    return faraday::bench::conducted_limits_json().dump();
}

static std::string logic_families() {
    return faraday::bench::families_json().dump();
}

static std::string version() { return "0.2.0"; }

EMSCRIPTEN_BINDINGS(faraday) {
    emscripten::function("analyze", &analyze);
    emscripten::function("analyzeSet", &analyze_set);
    emscripten::function("reanalyze", &reanalyze);
    emscripten::function("applyValues", &apply_values);
    emscripten::function("applyPartData", &apply_part_data);
    emscripten::function("partsWithoutValues", &parts_without_values);
    emscripten::function("diffReports", &diff_reports_js);
    emscripten::function("fixStitching", &fix_stitching);
    emscripten::function("solvePair", &solve_pair);
    emscripten::function("predictEmissions", &predict_emissions);
    emscripten::function("conductedEstimate", &conducted_estimate_js);
    emscripten::function("inputBranch", &input_branch);
    emscripten::function("cmBudget", &cm_budget);
    emscripten::function("returnPath", &return_path);
    emscripten::function("pdn", &pdn_map);
    emscripten::function("nearField", &near_field);
    emscripten::function("victimClasses", &victim_classes);
    emscripten::function("shielding", &shielding);
    emscripten::function("shieldMaterials", &shield_materials);
    emscripten::function("limitLines", &limit_lines);
    emscripten::function("conductedLimits", &conducted_limits);
    emscripten::function("logicFamilies", &logic_families);
    emscripten::function("version", &version);
}
