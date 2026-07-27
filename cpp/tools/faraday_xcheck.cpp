// faraday_xcheck — the browser's transient solver against real ngspice.
//
// Mna.hpp is a purpose-built MNA stepper: linear circuit, fixed timestep,
// factor once. That is what makes it fast enough to sit behind a slider, and
// it is exactly the kind of specialisation that can be quietly wrong. This
// tool builds one cross-section, extracts RLGC with the boundary-element
// solver, then runs the SAME circuit two ways:
//
//   * through faraday::mna::simulate(), the solver the browser ships;
//   * through spice_ladder_deck() into Kirchhoff's IN-PROCESS libngspice —
//     a different codebase, a different matrix, its own integration and its
//     own timestep control.
//
// Agreement means the fast path is not cutting a corner. It is native-only:
// libngspice does not go in the browser, and this is a check on the physics,
// not a runtime dependency.
//
//   faraday_xcheck [--sections N] [--tol-db 0.75]
//
// The two ladders are not bit-identical by construction: spice_ladder_deck()
// puts a full C at nodes 1..N and none at node 0, while the MNA stepper splits
// it half-and-half across the two ends. Both converge to the same distributed
// line; at finite N they differ by O(1/N), which is why the comparison is made
// at a stated tolerance and at a section count where that term is small.

#include <faraday/Bem2d.hpp>
#include <faraday/Mna.hpp>
#include <faraday/Rlgc.hpp>

#include <NgspiceRunner.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

double arg_num(int argc, char** argv, const std::string& f, double d) {
    for (int i = 1; i + 1 < argc; ++i)
        if (f == argv[i]) return std::atof(argv[i + 1]);
    return d;
}

faraday::Rlgc extract(const faraday::bem::PairSection& s) {
    faraday::bem::Geometry g = faraday::bem::geometry_for(s);
    faraday::bem::Solution sd = faraday::bem::solve(g, false);
    faraday::bem::Solution sv = faraday::bem::solve(g, true);
    const size_t nt = 3;
    std::vector<double> M(nt * nt, 0.0), M0(nt * nt, 0.0);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            M[i * nt + j] = sd.at(i, j);
            M0[i * nt + j] = sv.at(i, j);
        }
    return faraday::rlgc_from_maxwell(M, M0, nt, 2);
}

double peak(const std::vector<double>& v) {
    double best = 0;
    for (double x : v) if (std::abs(x) > std::abs(best)) best = x;
    return best;
}

struct Case {
    const char* name;
    faraday::bem::PairSection sec;
    double len_mm, rise_ns, z_src, z_term;
};

}  // namespace

int main(int argc, char** argv) {
    const int sections = (int)arg_num(argc, argv, "--sections", 32);
    const double tol_db = arg_num(argc, argv, "--tol-db", 0.75);

    if (!Kirchhoff::ngspice_in_process_available()) {
        std::fprintf(stderr,
                     "Kirchhoff was built without libngspice — rebuild it with "
                     "-DENABLE_NGSPICE=ON (the external binary is never used)\n");
        return 2;
    }

    std::vector<Case> cases;
    {
        faraday::bem::PairSection ms;
        ms.w1 = ms.w2 = 0.25e-3; ms.h = 0.2e-3; ms.t = 35e-6; ms.eps_r = 4.3;
        ms.gap = 0.25e-3;
        cases.push_back({"microstrip, matched", ms, 60, 0.5, 60, 60});
        cases.push_back({"microstrip, fast edge", ms, 100, 0.15, 50, 50});

        faraday::bem::PairSection tight = ms;
        tight.gap = 0.1e-3;
        cases.push_back({"microstrip, tight gap", tight, 40, 0.3, 50, 50});

        faraday::bem::PairSection sl;
        sl.stripline = true;
        sl.w1 = sl.w2 = 0.2e-3; sl.gap = 0.2e-3; sl.b = 0.8e-3; sl.t = 1e-6;
        sl.eps_r = 4.3;
        cases.push_back({"stripline, homogeneous", sl, 80, 0.3, 50, 50});
    }

    std::printf("%-26s %10s %10s %8s   %10s %10s %8s\n", "", "NEXT mna",
                "NEXT spice", "d(dB)", "FEXT mna", "FEXT spice", "d(dB)");
    int failures = 0;

    for (const Case& c : cases) {
        const faraday::Rlgc p = extract(c.sec);

        faraday::mna::DriveOptions o;
        o.length_m = c.len_mm * 1e-3;
        o.rise_s = c.rise_ns * 1e-9;
        o.amplitude_v = 3.3;
        o.z_src = c.z_src;
        o.z_term = c.z_term;
        o.z_victim_near = c.z_src;   // spice_ladder_deck ties the victim's near
                                     // end to z_src, so match it here
        o.sections = sections;
        o.max_steps = 6000;
        const faraday::mna::Waveforms w = faraday::mna::simulate(p, o);

        faraday::DeckOptions d;
        d.sections = sections;
        d.length_m = o.length_m;
        d.z_src = o.z_src;
        d.z_term = o.z_term;
        d.rise_s = o.rise_s;
        d.amplitude_v = o.amplitude_v;
        d.tstop_s = w.t.back();      // same window, so the same reflections land
        d.aggressor = 0;
        const std::string deck = faraday::spice_ladder_deck(p, d);

        Kirchhoff::NgspiceRunResult r = Kirchhoff::run_ngspice_in_process(deck);
        if (!r.success) {
            std::printf("%-26s  ngspice failed: %s\n", c.name, r.error.c_str());
            ++failures;
            continue;
        }
        auto grab = [&](const std::string& node) -> std::vector<double> {
            for (const auto& [name, samples] : r.vectors) {
                std::string n = name;
                for (auto& ch : n) ch = (char)std::tolower((unsigned char)ch);
                if (n == node || n == "v(" + node + ")") return samples;
            }
            throw std::runtime_error("node '" + node + "' missing from ngspice output");
        };
        const double sp_next = peak(grab("n1_0"));
        const double sp_fext = peak(grab("n1_" + std::to_string(sections)));

        auto db = [](double a, double b) {
            if (std::abs(a) < 1e-9 || std::abs(b) < 1e-9) return 0.0;
            return 20.0 * std::log10(std::abs(a) / std::abs(b));
        };
        const double dn = db(w.next_peak_v, sp_next);
        const double df = db(w.fext_peak_v, sp_fext);
        const bool ok = std::abs(dn) <= tol_db &&
                        (std::abs(w.fext_peak_v) < 1e-3 || std::abs(df) <= tol_db);
        if (!ok) ++failures;

        std::printf("%-26s %9.2fm %9.2fm %+7.2f   %9.2fm %9.2fm %+7.2f  %s\n",
                    c.name, w.next_peak_v * 1e3, sp_next * 1e3, dn,
                    w.fext_peak_v * 1e3, sp_fext * 1e3, df, ok ? "ok" : "FAIL");
    }

    std::printf("\n%d case(s), %d outside %.2f dB\n", (int)cases.size(), failures,
                tol_db);
    return failures ? 1 : 0;
}
