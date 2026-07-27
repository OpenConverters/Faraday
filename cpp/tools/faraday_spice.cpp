// faraday_spice — run a coupled-line ladder deck and report NEXT/FEXT.
//
//   faraday_spice deck.sp --victim 1 --sections 24 [--vdd 3.3]
//
// Deliberately a SEPARATE binary from faraday_solve: linking MFEM and
// libngspice into one process segfaults (ngspice's shared-library mode is not
// re-entrant and does not coexist with the solver's runtime). Splitting the two
// steps costs a file on disk and buys a stable pipeline.
//
// The simulation still goes through Kirchhoff's IN-PROCESS libngspice — the
// external ngspice binary is never invoked.

#include <faraday/Rlgc.hpp>

#include <NgspiceRunner.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
double arg_num(int argc, char** argv, const std::string& f, double d) {
    for (int i = 1; i + 1 < argc; ++i) if (f == argv[i]) return std::atof(argv[i + 1]);
    return d;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: faraday_spice <deck.sp> --victim <i> --sections <n> "
                     "[--vdd <V>]\n";
        return 2;
    }
    const std::string path = argv[1];
    const int victim = (int)arg_num(argc, argv, "--victim", 1);
    const int sections = (int)arg_num(argc, argv, "--sections", 24);
    const double vdd = arg_num(argc, argv, "--vdd", 3.3);

    try {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("cannot open " + path);
        std::stringstream ss;
        ss << in.rdbuf();

        if (!Kirchhoff::ngspice_in_process_available())
            throw std::runtime_error(
                "Kirchhoff was built without libngspice — rebuild it with "
                "-DENABLE_NGSPICE=ON (the external ngspice binary is not used)");

        Kirchhoff::NgspiceRunResult r = Kirchhoff::run_ngspice_in_process(ss.str());
        if (!r.success) throw std::runtime_error("ngspice: " + r.error);

        auto grab = [&](const std::string& node) {
            for (const auto& [name, samples] : r.vectors) {
                std::string n = name;
                for (auto& c : n) c = (char)std::tolower((unsigned char)c);
                if (n == node || n == "v(" + node + ")") return samples;
            }
            std::string have;
            for (const auto& [name, s] : r.vectors) have += " " + name;
            throw std::runtime_error("node '" + node +
                                     "' not in the ngspice output; have:" + have);
        };
        const int aggressor = victim == 0 ? 1 : 0;
        const std::string nn = "n" + std::to_string(victim) + "_0";
        const std::string nf = "n" + std::to_string(victim) + "_" +
                               std::to_string(sections);
        const std::string na = "n" + std::to_string(aggressor) + "_0";

        // Reference the crosstalk to the voltage actually LAUNCHED onto the
        // aggressor line, not the source's open-circuit swing: the driver's
        // output impedance and the line's Z0 form a divider, and normalising to
        // the wrong one shifts every dB figure (5.6 dB for a 50 ohm driver into
        // a 55 ohm line).
        const std::vector<double> agg = grab(na);
        double launched = 0.0;
        for (double x : agg) launched = std::max(launched, std::abs(x));
        if (launched <= 0.0)
            throw std::runtime_error("the aggressor node never moved — check the deck");

        faraday::CrosstalkPeaks p =
            faraday::crosstalk_from_waveforms(grab(nn), grab(nf), launched);

        std::printf("%zu samples over %.2f ns\n", r.time.size(),
                    r.time.empty() ? 0.0 : r.time.back() * 1e9);
        std::printf("aggressor launched %.2f V of the %.2f V source swing "
                    "(driver/line divider)\n", launched, vdd);
        std::printf("NEXT peak %+8.1f mV  (%.1f dB of launched)   at %s\n",
                    p.next_v * 1e3, p.next_db, nn.c_str());
        std::printf("FEXT peak %+8.1f mV  (%.1f dB of launched)   at %s\n",
                    p.fext_v * 1e3, p.fext_db, nf.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "faraday_spice: " << e.what() << "\n";
        return 1;
    }
}
