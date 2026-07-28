// faraday_cli: board.kicad_pcb [--stackup default-2layer|default-4layer]
//              [-o report.json]
// Screens the board and prints the ranked findings; writes the full report
// JSON (board geometry + findings + meta) for the web viewer.

#include <faraday/Import.hpp>
#include <faraday/Screener.hpp>

#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    std::string board_path, out_path, stackup_name, fail_on;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--stackup" && i + 1 < argc) stackup_name = argv[++i];
        else if (a == "-o" && i + 1 < argc) out_path = argv[++i];
        // CI gate: exit 3 when any finding reaches this severity. "high" or
        // "medium"; anything else is refused rather than silently ignored.
        else if (a == "--fail-on" && i + 1 < argc) fail_on = argv[++i];
        else if (board_path.empty()) board_path = a;
        else {
            std::cerr << "unexpected argument: " << a << "\n";
            return 2;
        }
    }
    if (board_path.empty()) {
        std::cerr << "usage: faraday_cli <board.kicad_pcb|board.hyp|board.xml> "
                     "[--stackup default-<N>layer] [-o report.json] "
                     "[--fail-on high|medium]\n"
                     "       format is detected from the file's contents\n";
        return 2;
    }

    try {
        std::ifstream in(board_path);
        if (!in) throw std::runtime_error("cannot open " + board_path);
        std::stringstream ss;
        ss << in.rdbuf();

        std::optional<faraday::Stackup> user;
        if (!stackup_name.empty()) user = faraday::builtin_stackup(stackup_name);
        faraday::BoardFormat fmt;
        faraday::BoardIR board =
            faraday::import_board(ss.str(), std::move(user), &fmt);
        std::cout << "format: " << faraday::format_name(fmt) << "\n";

        nlohmann::json report = faraday::analyze_board(board);

        if (!out_path.empty()) {
            std::ofstream out(out_path);
            out << report.dump(1);
            std::cout << "report: " << out_path << "\n";
        }

        const auto& meta = report["meta"];
        std::cout << "stackup: " << meta["stackupSource"].get<std::string>() << "\n";
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
