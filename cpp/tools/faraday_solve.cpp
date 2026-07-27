// faraday_solve — the field-solver tier.
//
//   faraday_solve --w 0.3 --sep 0.5 --h 0.2 --t 0.035 --er 4.4 --len 40
//                 [--deck deck.sp] [--field field.vtk]
//
// Takes a coupled-pair cross-section, meshes it, runs OMFEM's
// ElectrostaticCartesian twice (dielectric and vacuum), and reports the RLGC
// matrices, Z0, and the backward-coupling coefficient — the number the
// screening tier estimates with a closed form. Optionally writes the ngspice
// ladder deck for a time-domain NEXT/FEXT run.
//
// Built only when OMFEM is available (-DFARADAY_OMFEM_ROOT=...); the browser
// engine and the default build stay free of MFEM.

#include <faraday/CrossSection.hpp>
#include <faraday/Rlgc.hpp>
#include <faraday/Tline.hpp>

#include <omfem/ElectrostaticCartesian.hpp>
#include <omfem/HarmonicEddy.hpp>
#include <omfem/Materials.hpp>
#include <omfem/Problem.hpp>


#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

double arg_num(int argc, char** argv, const std::string& flag, double dflt,
               bool* seen = nullptr) {
    for (int i = 1; i + 1 < argc; ++i)
        if (flag == argv[i]) {
            if (seen) *seen = true;
            return std::atof(argv[i + 1]);
        }
    return dflt;
}
std::string arg_str(int argc, char** argv, const std::string& flag,
                    const std::string& dflt) {
    for (int i = 1; i + 1 < argc; ++i)
        if (flag == argv[i]) return argv[i + 1];
    return dflt;
}

void print_matrix(const char* label, const std::vector<double>& m, size_t n,
                  const char* unit, double scale) {
    std::printf("%s [%s]\n", label, unit);
    for (size_t i = 0; i < n; ++i) {
        std::printf("   ");
        for (size_t j = 0; j < n; ++j) std::printf("%12.4f", m[i * n + j] * scale);
        std::printf("\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool have_w = false;
    const double w = arg_num(argc, argv, "--w", 0.0, &have_w);
    const double w_b = arg_num(argc, argv, "--wb", w);
    const double sep = arg_num(argc, argv, "--sep", 0.0);
    const double h = arg_num(argc, argv, "--h", 0.0);
    const double t = arg_num(argc, argv, "--t", 0.035);
    const double er = arg_num(argc, argv, "--er", 0.0);
    const double len_mm = arg_num(argc, argv, "--len", 0.0);
    const std::string deck_path = arg_str(argc, argv, "--deck", "");
    const std::string field_path = arg_str(argc, argv, "--field", "");

    if (!have_w || sep <= 0 || h <= 0 || er <= 0 || len_mm <= 0) {
        std::cerr <<
            "usage: faraday_solve --w <trace mm> --sep <centre spacing mm>\n"
            "                     --h <height above plane mm> --er <eps_r>\n"
            "                     --len <coupled length mm> [--wb <mm>]\n"
            "                     [--t <copper mm>] [--deck out.sp] [--field out.vtk]\n"
            "\nEvery parameter is required because none of them can be assumed.\n";
        return 2;
    }

    try {
        faraday::CrossSection cs =
            faraday::make_coupled_section(w, w_b, sep, h, t, er);
        const std::string mesh = "/tmp/faraday_section.msh";
        cs.write_gmsh(mesh);
        std::printf("cross-section: %.3f x %.3f mm, %d x %d cells\n",
                    cs.width * 1e3, cs.height * 1e3, cs.nx, cs.ny);

        omfem::Problem p;
        p.coordinate_system = omfem::CoordinateSystem::Cartesian;
        for (auto& [name, eps] : cs.region_permittivity())
            p.region_permittivity[name] = eps;

        omfem::ElectrostaticCartesian es(p);
        es.load_mesh(mesh);
        es.assemble();
        const auto& names = es.conductor_names();
        std::printf("conductors:");
        for (const auto& n : names) std::printf(" %s", n.c_str());
        std::printf("\n");

        // the reference is the conductor named *_gnd
        size_t ref = names.size();
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i].find("gnd") != std::string::npos) ref = i;
        if (ref == names.size())
            throw std::runtime_error("solve: no reference conductor in the section");

        std::vector<double> C = es.capacitance_matrix(false);
        std::vector<double> C0 = es.capacitance_matrix(true);
        if (!field_path.empty()) { es.solve(); es.write_field(field_path); }

        faraday::Rlgc p_ul =
            faraday::rlgc_from_maxwell(C, C0, names.size(), ref);
        // R is filled in below only if --rf asks for the eddy solve; otherwise
        // it stays ZERO and the deck is lossless, which is stated, not hidden.

        print_matrix("L", p_ul.L, p_ul.n, "nH/m", 1e9);
        print_matrix("C", p_ul.C, p_ul.n, "pF/m", 1e12);
        for (size_t i = 0; i < p_ul.n; ++i)
            std::printf("line %zu: Z0 = %.1f ohm, v = %.3f c0, eps_eff = %.2f\n", i,
                        p_ul.z0(i), p_ul.velocity(i) / 299792458.0,
                        std::pow(299792458.0 / p_ul.velocity(i), 2.0));
        if (p_ul.n >= 2) {
            const double kb = p_ul.kb(0, 1);
            std::printf("\nbackward coupling k_b = %.4f  ->  NEXT %.1f dB "
                        "(field solve)\n", kb, 20.0 * std::log10(kb));
            const double kb_screen = faraday::tline::next_sat_edge(sep, h);
            std::printf("screening estimate            "
                        "     NEXT %.1f dB  (delta %.1f dB)\n",
                        20.0 * std::log10(kb_screen),
                        20.0 * std::log10(kb) - 20.0 * std::log10(kb_screen));
        }

        // ---- R(f): harmonic eddy-current solve on the same geometry --------
        // Drive the aggressor with 1 A and return it through the plane; the
        // victim carries no NET current (polarity 0) but still develops the
        // proximity currents that make R rise with frequency. Total dissipation
        // gives R via P = 0.5 R I_peak^2.
        std::vector<double> r_dc_per_m;
        const double f_hz = arg_num(argc, argv, "--rf", 0.0);
        if (f_hz > 0.0) {
            const double sigma = arg_num(argc, argv, "--sigma", 5.8e7);
            const double delta =
                std::sqrt(2.0 / (2.0 * M_PI * f_hz * (4e-7 * M_PI) * sigma));
            // The eddy solve is only as good as the mesh's ability to resolve
            // the skin depth. This mesher is UNIFORM, so once delta drops below
            // a cell the current cannot crowd where physics puts it and R comes
            // out too low — silently. Refuse rather than report a number that
            // looks fine and is not. (Caught by a sqrt(f) scaling check: 0.25 ->
            // 1 GHz scaled correctly at 2.13x, 1 -> 4 GHz only reached 1.43x.)
            // Refine the eddy mesh until a cell fits inside the skin depth,
            // up to what a uniform grid can afford. (A graded mesh refining
            // only at conductor surfaces is the real answer and is not built.)
            faraday::CrossSection ecs = cs;
            const int ny_needed = (int)std::ceil(ecs.height / (delta / 2.0));
            const int ny_cap = (int)arg_num(argc, argv, "--eddy-ny-max", 1600);
            ecs.ny = std::min(std::max(ecs.ny, ny_needed), ny_cap);
            ecs.nx = std::min(std::max(ecs.nx, (int)(ecs.nx * 1.0)), 900);
            const double cell_y = ecs.height / ecs.ny;
            const double per_delta = delta / cell_y;
            if (per_delta < 2.0) {
                std::fprintf(stderr,
                    "faraday_solve: at %.3f GHz the skin depth is %.2f um but the "
                    "mesh cell is %.2f um (%.1f cells per skin depth). A uniform "
                    "mesh cannot resolve the current crowding and R would come "
                    "out too LOW. Raise the mesh density or solve at a lower "
                    "frequency; a graded mesh is the real fix and is not built "
                    "yet.\n", f_hz / 1e9, delta * 1e6, cell_y * 1e6, per_delta);
                return 1;
            }
            std::printf("\neddy mesh %d x %d: %.2f cells per skin depth "
                        "(delta %.2f um, cell %.2f um)\n", ecs.nx, ecs.ny,
                        per_delta, delta * 1e6, cell_y * 1e6);
            const std::string emesh = "/tmp/faraday_section_eddy.msh";
            ecs.write_gmsh(emesh, /*eddy=*/true);
            auto loss_at = [&](double f) {
                omfem::Problem q;
                q.coordinate_system = omfem::CoordinateSystem::Cartesian;
                q.conductor_model = omfem::ConductorModel::Massive;  // let current redistribute
                q.frequency = f;
                q.I_peak = 1.0;
                q.component_depth = 1.0;                             // per metre of run
                q.wire.sigma = sigma;   // annealed copper
                q.turn_polarity_by_role["sig"] = 1.0;
                q.turn_polarity_by_role["ret"] = 1.0;
                q.turn_polarity_by_role["vic"] = 0.0;   // victim: induced current only
                omfem::HarmonicEddy he(q);
                he.load_mesh(emesh);
                he.assemble();
                he.solve();
                omfem::SolveResults sr = he.compute_results();
                double total = 0.0;
                for (const auto& [name, w] : sr.P_cu_per_body) total += w;
                return total;
            };
            // DC reference: the same solve at a frequency low enough that the
            // current is uniform. Taking the ratio cancels every geometric
            // factor, so R_ac/R_dc is trustworthy even where the absolute
            // value depends on how much of the plane the section captures.
            const double p_dc = loss_at(1.0);
            const double p_ac = loss_at(f_hz);
            const double r_dc = 2.0 * p_dc;      // P = 0.5 R I^2, I_peak = 1 A
            const double r_ac = 2.0 * p_ac;
            std::printf("\nR(f) from the eddy-current solve (sigma %.2e S/m):\n",
                        sigma);
            std::printf("   R_dc = %8.2f mohm/m\n", r_dc * 1e3);
            std::printf("   R_ac = %8.2f mohm/m at %.3f GHz   (R_ac/R_dc = %.2f)\n",
                        r_ac * 1e3, f_hz / 1e9, r_dc > 0 ? r_ac / r_dc : 0.0);
            std::printf("   skin depth %.2f um vs %.0f um copper — %s\n",
                        delta * 1e6, t * 1e3,
                        delta * 1e3 < t ? "current is confined to the surface"
                                        : "copper is thin compared with the skin depth");
            r_dc_per_m.assign(p_ul.n, r_ac);
            std::remove(emesh.c_str());
        }

        faraday::DeckOptions o;
        o.length_m = len_mm * 1e-3;
        o.rise_s = arg_num(argc, argv, "--tr", o.rise_s);
        o.amplitude_v = arg_num(argc, argv, "--vdd", o.amplitude_v);
        for (size_t i = 0; i < p_ul.n && i < r_dc_per_m.size(); ++i)
            p_ul.R[i * p_ul.n + i] = r_dc_per_m[i];
        const std::string deck = faraday::spice_ladder_deck(p_ul, o);
        if (!deck_path.empty()) {
            std::ofstream os(deck_path);
            os << deck;
            std::printf("\ndeck: %s (%d sections over %.1f mm)\n",
                        deck_path.c_str(), o.sections, len_mm);
        }

// Time domain is a separate step: faraday_spice runs this deck through
        // Kirchhoff's in-process libngspice. Linking MFEM and ngspice into one
        // process segfaults, so the deck goes via a file.
        if (!deck_path.empty() && p_ul.n >= 2)
            std::printf("   run it: faraday_spice %s --victim 1 --sections %d "
                        "--vdd %.1f\n", deck_path.c_str(), o.sections, o.amplitude_v);

        std::remove(mesh.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "faraday_solve: " << e.what() << "\n";
        return 1;
    }
}
