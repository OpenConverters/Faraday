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

        if (!deck_path.empty()) {
            faraday::DeckOptions o;
            o.length_m = len_mm * 1e-3;
            std::ofstream os(deck_path);
            os << faraday::spice_ladder_deck(p_ul, o);
            std::printf("\ndeck: %s (%d sections over %.1f mm)\n",
                        deck_path.c_str(), o.sections, len_mm);
        }
        std::remove(mesh.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "faraday_solve: " << e.what() << "\n";
        return 1;
    }
}
