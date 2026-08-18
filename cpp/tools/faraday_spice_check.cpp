// Does the exported deck actually parse in a real SPICE engine?
//
// ABT #805. A .cir that only a human has read is a text file with opinions in
// it. This wraps the exported subcircuit in a minimal harness — resistors
// where the switching cell would be — and solves an operating point through
// KIRCHHOFF'S IN-PROCESS libngspice (never the installed binary; house rule,
// and prod must run without it). It proves syntax and connectivity, nothing
// about the physics: the deck deliberately carries no device models, so there
// is no switching to simulate here, and this tool must never pretend otherwise.
//
//   faraday_spice_check <board> [--stackup NAME] [--chassis-gap-mm X]

#include <faraday/Import.hpp>
#include <faraday/SpiceExport.hpp>

#include "NgspiceRunner.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    std::string path, stackup;
    faraday::spice::ExportOptions opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--stackup" && i + 1 < argc) stackup = argv[++i];
        else if (a == "--chassis-gap-mm" && i + 1 < argc)
            opt.chassis_gap_mm = std::stod(argv[++i]);
        else path = a;
    }
    if (path.empty()) {
        std::cerr << "usage: faraday_spice_check <board> [--stackup NAME] "
                     "[--chassis-gap-mm X]\n";
        return 2;
    }
    try {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    faraday::BoardIR board =
        faraday::import_board(ss.str(), faraday::resolve_stackup(stackup));
    faraday::Screener sc(board);
    const faraday::spice::Deck deck = faraday::spice::build(board, sc, opt);

    const auto& ports = deck.manifest["ports"];
    const std::string rail = ports[0], cell = ports[1], sw = ports[2],
                      rtn = ports[3];

    std::ostringstream h;
    h << "* harness: the exported subcircuit, with resistors where the\n"
      << "* switching cell would be. Operating point only.\n"
      << deck.cir << "\n"
      << "V1 " << rail << " 0 DC 48\n"
      << "X1 " << rail << " " << cell << " " << sw << " 0 CH "
      << deck.manifest["subckt"].get<std::string>() << "\n"
      << "RCELL " << cell << " " << sw << " 10\n"
      << "RLOW " << sw << " 0 10\n"
      << "RCH CH 0 1meg\n"
      << ".op\n.end\n";

    if (!Kirchhoff::ngspice_in_process_available()) {
        std::cerr << "libngspice is not available in this build\n";
        return 2;
    }
    const auto r = Kirchhoff::run_ngspice_in_process(h.str(), 60.0);
    if (!r.success) {
        std::cerr << "the exported deck did NOT parse/solve: " << r.error << "\n";
        return 1;
    }
    std::cout << "OK: the exported deck parses and solves in libngspice — "
              << deck.manifest["entries"].size() << " extracted values, ports "
              << rail << "/" << cell << "/" << sw << "/" << rtn << "/CHASSIS, "
              << r.vectors.size() << " vectors back\n";
    for (const auto& w : deck.manifest["warnings"])
        std::cout << "  warning: " << w.get<std::string>() << "\n";
    return 0;
    // A refusal is an ANSWER here — "this board has no commutation mesh", "a
    // 4-layer stackup does not fit a 2-layer board" — and it must arrive as a
    // sentence and an exit code, not as an uncaught exception and a core dump.
    } catch (const std::exception& e) {
        std::cerr << "refused: " << e.what() << "\n";
        return 1;
    }
}
