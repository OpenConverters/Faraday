// faraday_cli: board.kicad_pcb | gerber-dir/ | file1.gbr file2.gbr ...
//              [--stackup default-<N>layer] [-o report.json]
//              [--spice deck.cir] [--manifest deck.json]
//              [--chassis-gap-mm X] [--chassis-eps-r X] [--return-net NAME]
//              [--parts-out parts.csv] [--values values.csv]
//              [--layer-map L1=1,L2=2,...[,OUTLINE=profile]]
// Screens the board and prints the ranked findings; writes the full report
// JSON (board geometry + findings + meta) for the web viewer.

#include <faraday/Diff.hpp>
#include <faraday/Fixes.hpp>
#include <faraday/Report.hpp>
#include <faraday/Import.hpp>
#include <faraday/Screener.hpp>
#include <faraday/SpiceExport.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

static std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    // A board is one file (KiCad/HYP/IPC-2581) OR many (a Gerber X2 set) OR
    // a directory holding the set.
    std::vector<std::string> board_paths;
    std::string out_path, stackup_name, fail_on;
    // regression gate: compare against a previous report and fail only on
    // NEW or WORSENED findings — the gate a brownfield board can adopt today
    std::string baseline_path, fail_on_regression;
    std::string fix_out;   // --fix-stitching <out.kicad_pcb>
    faraday::gerber::LayerMap stated_layers;   // --layer-map (pre-X2 sets)
    // --spice/--manifest: the parasitic-annotated netlist (ABT #805) — the
    // copper's half of a conducted-emissions simulation, written where a
    // simulator can read it. --chassis-gap-mm is the one quantity a layout
    // cannot carry, so without it the common-mode element is not emitted.
    std::string spice_out, manifest_out;
    // Values the CAD export did not carry. --parts-out asks the question
    // (refdes,partNumber); --values answers it (refdes,value). Altium's ODB++
    // writes part numbers and no values at all, so without this a board full
    // of real capacitors has no PDN and no input branch.
    std::string parts_out, values_in;
    faraday::spice::ExportOptions spice_opt;
    // --switch-net NAME (repeatable): screen NAME as a switch node, recorded
    // with switchNodeSource "user" — the CLI face of candidate promotion
    std::vector<std::string> user_switch_nets;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--stackup" && i + 1 < argc) stackup_name = argv[++i];
        else if (a == "--switch-net" && i + 1 < argc)
            user_switch_nets.push_back(argv[++i]);
        else if (a == "-o" && i + 1 < argc) out_path = argv[++i];
        // CI gate: exit 3 when any finding reaches this severity. "high" or
        // "medium"; anything else is refused rather than silently ignored.
        else if (a == "--fail-on" && i + 1 < argc) fail_on = argv[++i];
        // --layer-map: the caller STATES what each Gerber file is, for a
        // pre-X2 vendor pack whose layer identity lives only in a file name.
        // Faraday will not infer it (see GerberImporter's LayerMap).
        else if (a == "--layer-map" && i + 1 < argc) {
            std::string spec = argv[++i];
            size_t at = 0;
            while (at <= spec.size()) {
                const size_t comma = spec.find(',', at);
                std::string item = spec.substr(at, comma == std::string::npos
                                                       ? std::string::npos
                                                       : comma - at);
                if (!item.empty()) {
                    const size_t eq = item.find('=');
                    if (eq == std::string::npos) {
                        std::cerr << "faraday: --layer-map wants STEM=INDEX or "
                                     "STEM=profile, got '" << item << "'\n";
                        return 2;
                    }
                    std::string key = item.substr(0, eq), val = item.substr(eq + 1);
                    for (char& c : key) c = (char)std::toupper((unsigned char)c);
                    for (char& c : val) c = (char)std::toupper((unsigned char)c);
                    stated_layers[key] = val;
                }
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
        else if (a == "--baseline" && i + 1 < argc) baseline_path = argv[++i];
        else if (a == "--fail-on-regression" && i + 1 < argc)
            fail_on_regression = argv[++i];
        else if (a == "--fix-stitching" && i + 1 < argc) fix_out = argv[++i];
        else if (a == "--spice" && i + 1 < argc) spice_out = argv[++i];
        else if (a == "--manifest" && i + 1 < argc) manifest_out = argv[++i];
        else if (a == "--chassis-gap-mm" && i + 1 < argc)
            spice_opt.chassis_gap_mm = std::stod(argv[++i]);
        else if (a == "--chassis-eps-r" && i + 1 < argc)
            spice_opt.chassis_eps_r = std::stod(argv[++i]);
        else if (a == "--return-net" && i + 1 < argc)
            spice_opt.return_net = argv[++i];
        else if (a == "--parts-out" && i + 1 < argc) parts_out = argv[++i];
        else if (a == "--values" && i + 1 < argc) values_in = argv[++i];
        else board_paths.push_back(a);
    }
    std::string dir_root;   // non-empty → paths become relative to it
    if (board_paths.size() == 1 &&
        std::filesystem::is_directory(board_paths[0])) {
        dir_root = board_paths[0];
        board_paths.clear();
        // recursive: an ODB++ job is a tree (matrix/matrix, steps/...)
        for (const auto& e :
             std::filesystem::recursive_directory_iterator(dir_root))
            if (e.is_regular_file()) board_paths.push_back(e.path().string());
        std::sort(board_paths.begin(), board_paths.end());
        if (board_paths.empty()) {
            std::cerr << dir_root << " contains no files\n";
            return 2;
        }
    }
    if (board_paths.empty()) {
        std::cerr << "usage: faraday_cli <board.kicad_pcb|board.hyp|board.xml> "
                     "[--stackup default-<N>layer|stackup.json] [-o report.json] "
                     "[--fail-on high|medium] [--baseline old.json "
                     "[--fail-on-regression high|medium]] "
                     "[--switch-net NAME]...\n"
                     "       format is detected from the file's contents\n";
        return 2;
    }

    try {
        std::vector<faraday::gerber::NamedFile> files;
        for (const auto& p : board_paths)
            files.push_back(
                {dir_root.empty()
                     ? std::filesystem::path(p).filename().string()
                     : std::filesystem::relative(p, dir_root).generic_string(),
                 slurp(p)});

        // --stackup takes a builtin name, or a path to a custom stackup
        // JSON file (ends in .json) — the same format the web editor saves
        if (stackup_name.size() > 5 &&
            stackup_name.compare(stackup_name.size() - 5, 5, ".json") == 0)
            stackup_name = slurp(stackup_name);
        std::optional<faraday::Stackup> user =
            faraday::resolve_stackup(stackup_name);
        faraday::BoardFormat fmt;
        faraday::BoardIR board =
            faraday::import_board_set(files, std::move(user), &fmt, stated_layers);
        std::cout << "format: " << faraday::format_name(fmt) << "\n";

        // Values the export did not carry, before anything reads them.
        if (!values_in.empty()) {
            faraday::values::ValueTable vt =
                faraday::values::parse_value_table(slurp(values_in));
            const size_t n = faraday::apply_values(board, vt);
            std::cout << "values: filled " << n << " component(s) from "
                      << values_in;
            if (vt.ignored)
                std::cout << " (" << vt.ignored
                          << " skipped — the board already carried a value, "
                             "which always wins)";
            std::cout << "\n";
        }
        if (!parts_out.empty()) {
            const auto parts = faraday::parts_without_values(board);
            std::ofstream po(parts_out);
            po << "refdes,part\n";
            for (const auto& [ref, part] : parts) po << ref << "," << part << "\n";
            std::cout << "parts: " << parts.size()
                      << " component(s) with a part number and no value -> "
                      << parts_out << "\n";
        }

        faraday::ScreenerParams sp;
        sp.user_switch_nets = user_switch_nets;
        nlohmann::json report = faraday::analyze_board(board, sp);

        if (!out_path.empty()) {
            std::ofstream out(out_path);
            out << report.dump(1);
            std::cout << "report: " << out_path << "\n";
        }

        const auto& meta = report["meta"];
        std::cout << "stackup: " << meta["stackupSource"].get<std::string>() << "\n";
        for (const auto& n : report["board"]["plausibilityNotes"])
            std::cout << "note: " << n.get<std::string>() << "\n";
        for (const auto& p : meta["planes"])
            std::cout << "layer " << p["layer"].get<std::string>()
                      << (p["isPlane"].get<bool>() ? "  [plane]" : "  [signal]")
                      << "  pour coverage "
                      << (int)(p["zoneCoverage"].get<double>() * 100) << "%\n";
        std::cout << report["findings"].size() << " findings";
        size_t floor_drop = meta["droppedBelowFloorDb"].get<size_t>();
        size_t cap_drop = meta["droppedByFindingCap"].get<size_t>();
        if (floor_drop) std::cout << " (+" << floor_drop << " below floor)";
        if (cap_drop) std::cout << " (+" << cap_drop << " over cap)";
        std::cout << "\n\n";
        for (const auto& f : report["findings"]) {
            std::cout << f["id"].get<std::string>() << "  ["
                      << f["severityLabel"].get<std::string>() << "]  "
                      << f["title"].get<std::string>() << "\n    "
                      << f["detail"].get<std::string>() << "\n";
        }
        int arcs = report["board"]["approximatedArcs"].get<int>();
        if (arcs)
            std::cout << "\nnote: " << arcs
                      << " arc(s) approximated by chords in this analysis\n";
        if (!fix_out.empty()) {
            if (files.size() != 1)
                throw std::runtime_error(
                    "--fix-stitching needs the single original .kicad_pcb");
            faraday::Screener fsc(board);
            auto plan = faraday::fixes::propose_stitching(board, fsc);
            std::cout << "\nstitching: " << plan.unstitched_seen
                      << " unstitched layer change(s), " << plan.vias.size()
                      << " via(s) proposed\n";
            for (const auto& n : plan.notes) std::cout << "  note: " << n << "\n";
            if (!plan.vias.empty()) {
                std::ofstream fo(fix_out);
                fo << faraday::fixes::apply_stitching(files[0].text, board,
                                                      plan.vias);
                std::cout << "  wrote " << fix_out << " (review it in KiCad; "
                          << "the original is untouched)\n";
            }
        }
        if (!spice_out.empty() || !manifest_out.empty()) {
            faraday::Screener ssc(board, sp);
            const faraday::spice::Deck deck =
                faraday::spice::build(board, ssc, spice_opt);
            if (!spice_out.empty()) {
                std::ofstream fo(spice_out);
                fo << deck.cir;
                std::cout << "\nspice: wrote " << spice_out << " ("
                          << deck.manifest["entries"].size()
                          << " extracted values, ports "
                          << deck.manifest["ports"][0].get<std::string>() << "/"
                          << deck.manifest["ports"][1].get<std::string>() << "/"
                          << deck.manifest["ports"][2].get<std::string>()
                          << "/CHASSIS)\n";
            }
            if (!manifest_out.empty()) {
                std::ofstream fo(manifest_out);
                fo << deck.manifest.dump(2);
                std::cout << "spice: wrote " << manifest_out
                          << " (provenance per value)\n";
            }
            for (const auto& w : deck.manifest["warnings"])
                std::cout << "  warning: " << w.get<std::string>() << "\n";
        }
        if (!baseline_path.empty()) {
            nlohmann::json base = nlohmann::json::parse(slurp(baseline_path));
            nlohmann::json d = faraday::diff::diff_reports(base, report);
            std::cout << "\nvs baseline: " << d["added"].size() << " new, "
                      << d["worsened"].size() << " worsened, "
                      << d["improved"].size() << " improved, "
                      << d["resolved"].size() << " resolved  ["
                      << d["verdict"].get<std::string>() << "]\n";
            for (const auto& e : d["added"])
                std::cout << "  NEW      [" << e["severityLabel"].get<std::string>()
                          << "]  " << e["title"].get<std::string>() << "\n";
            for (const auto& e : d["worsened"])
                std::cout << "  WORSE    [" << e["severityLabel"].get<std::string>()
                          << "]  " << e["title"].get<std::string>() << " ("
                          << e["why"].get<std::string>() << ")\n";
            for (const auto& e : d["resolved"])
                std::cout << "  resolved [" << e["severityLabel"].get<std::string>()
                          << "]  " << e["title"].get<std::string>() << "\n";
            if (!fail_on_regression.empty()) {
                if (fail_on_regression != "high" && fail_on_regression != "medium")
                    throw std::runtime_error(
                        "--fail-on-regression must be 'high' or 'medium', got '" +
                        fail_on_regression + "'");
                if (faraday::diff::has_regression_at(d, fail_on_regression)) {
                    std::cerr << "FAIL: new or worsened finding(s) at or above '"
                              << fail_on_regression << "' severity\n";
                    return 3;
                }
            }
        } else if (!fail_on_regression.empty()) {
            throw std::runtime_error("--fail-on-regression needs --baseline");
        }
        if (!fail_on.empty()) {
            if (fail_on != "high" && fail_on != "medium")
                throw std::runtime_error("--fail-on must be 'high' or 'medium', got '" +
                                         fail_on + "'");
            int hits = 0;
            for (const auto& f : report["findings"]) {
                const std::string sev = f.value("severityLabel", "");
                if (sev == "high" || (fail_on == "medium" && sev == "medium")) ++hits;
            }
            if (hits > 0) {
                std::cerr << "FAIL: " << hits << " finding(s) at or above '"
                          << fail_on << "' severity\n";
                return 3;
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "faraday: " << e.what() << "\n";
        return 1;
    }
}
