// faraday_cli: board.kicad_pcb | gerber-dir/ | file1.gbr file2.gbr ...
//              [--stackup default-<N>layer] [-o report.json]
// Screens the board and prints the ranked findings; writes the full report
// JSON (board geometry + findings + meta) for the web viewer.

#include <faraday/Diff.hpp>
#include <faraday/Fixes.hpp>
#include <faraday/Import.hpp>
#include <faraday/Screener.hpp>

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
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--stackup" && i + 1 < argc) stackup_name = argv[++i];
        else if (a == "-o" && i + 1 < argc) out_path = argv[++i];
        // CI gate: exit 3 when any finding reaches this severity. "high" or
        // "medium"; anything else is refused rather than silently ignored.
        else if (a == "--fail-on" && i + 1 < argc) fail_on = argv[++i];
        else if (a == "--baseline" && i + 1 < argc) baseline_path = argv[++i];
        else if (a == "--fail-on-regression" && i + 1 < argc)
            fail_on_regression = argv[++i];
        else if (a == "--fix-stitching" && i + 1 < argc) fix_out = argv[++i];
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
                     "[--fail-on-regression high|medium]]\n"
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
            faraday::import_board_set(files, std::move(user), &fmt);
        std::cout << "format: " << faraday::format_name(fmt) << "\n";

        nlohmann::json report = faraday::analyze_board(board);

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
