// RLGC extraction and the SPICE ladder. The physics here is checked against
// closed-form results, not against recorded numbers.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <faraday/CrossSection.hpp>
#include <faraday/Rlgc.hpp>
#include <faraday/Tline.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using Catch::Approx;
using namespace faraday;

TEST_CASE("rlgc: matrix inverse round-trips", "[rlgc]") {
    std::vector<double> a{4, 1, 0, 1, 3, 1, 0, 1, 2};
    std::vector<double> inv = invert_matrix(a, 3);
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j) {
            double s = 0;
            for (size_t k = 0; k < 3; ++k) s += a[i * 3 + k] * inv[k * 3 + j];
            CHECK(s == Approx(i == j ? 1.0 : 0.0).margin(1e-12));
        }
    CHECK_THROWS_WITH(invert_matrix({1, 2, 2, 4}, 2),
                      Catch::Matchers::ContainsSubstring("singular"));
}

TEST_CASE("rlgc: a vacuum line propagates at c and has the textbook Z0",
          "[rlgc]") {
    // Two conductors, C = C0 (vacuum). Whatever the geometry, the extracted
    // L and C must give v = c0 exactly — that is the check that the
    // L = mu0 eps0 C0^-1 route is wired up correctly.
    const double c_self = 1.0e-10;                  // 100 pF/m to the reference
    std::vector<double> mx{c_self, -c_self, -c_self, c_self};
    Rlgc p = rlgc_from_maxwell(mx, mx, 2, /*ref=*/1);
    REQUIRE(p.n == 1);
    const double c_light = 1.0 / std::sqrt(MU0_ * EPS0_);
    CHECK(p.velocity(0) == Approx(c_light).epsilon(1e-12));
    // Z0 = 1/(v C) is the standard identity
    CHECK(p.z0(0) == Approx(1.0 / (c_light * c_self)).epsilon(1e-12));
}

TEST_CASE("rlgc: dielectric slows the line and lowers Z0 by sqrt(eps_eff)",
          "[rlgc]") {
    const double c0 = 1.0e-10, er = 4.0;
    std::vector<double> vac{c0, -c0, -c0, c0};
    std::vector<double> mx{c0 * er, -c0 * er, -c0 * er, c0 * er};
    Rlgc p = rlgc_from_maxwell(mx, vac, 2, 1);
    const double c_light = 1.0 / std::sqrt(MU0_ * EPS0_);
    CHECK(p.velocity(0) == Approx(c_light / std::sqrt(er)).epsilon(1e-12));
    Rlgc p0 = rlgc_from_maxwell(vac, vac, 2, 1);
    CHECK(p.z0(0) == Approx(p0.z0(0) / std::sqrt(er)).epsilon(1e-12));
    // the vacuum solve fixes L, so L must NOT change with the dielectric
    CHECK(p.at(p.L, 0, 0) == Approx(p0.at(p0.L, 0, 0)).epsilon(1e-12));
}

TEST_CASE("rlgc: three-conductor extraction gives signed, coupled matrices",
          "[rlgc]") {
    // conductors 0,1 signal; 2 = reference. Maxwell matrix of a symmetric pair.
    const double cs = 1.0e-10, cm = 8.0e-12;
    std::vector<double> mx{
        cs + cm, -cm,     -cs,
        -cm,     cs + cm, -cs,
        -cs,     -cs,     2 * cs + 0.0};
    Rlgc p = rlgc_from_maxwell(mx, mx, 3, /*ref=*/2);
    REQUIRE(p.n == 2);
    // self capacitance to ground, mutual between the lines
    CHECK(p.c_mutual(0, 1) == Approx(cm));
    CHECK(p.c_to_ref(0) == Approx(cs));
    CHECK(p.at(p.C, 0, 0) > 0.0);
    // L symmetric and positive definite in the 2x2 sense
    CHECK(p.at(p.L, 0, 1) == Approx(p.at(p.L, 1, 0)));
    CHECK(p.at(p.L, 0, 0) > 0.0);
    CHECK(p.at(p.L, 0, 0) * p.at(p.L, 1, 1) > p.at(p.L, 0, 1) * p.at(p.L, 0, 1));
    CHECK(p.kb(0, 1) > 0.0);
    CHECK(p.kb(0, 1) < 0.25);   // below the saturated bound the screener uses
}

TEST_CASE("rlgc: ladder deck is well formed and physical", "[rlgc][deck]") {
    const double cs = 1.0e-10, cm = 8.0e-12;
    std::vector<double> mx{cs + cm, -cm, -cs, -cm, cs + cm, -cs, -cs, -cs, 2 * cs};
    Rlgc p = rlgc_from_maxwell(mx, mx, 3, 2);
    DeckOptions o;
    o.sections = 4;
    o.length_m = 0.04;
    std::string deck = spice_ladder_deck(p, o);

    CHECK(deck.find("Vagg src 0 PWL") != std::string::npos);
    CHECK(deck.find(".tran") != std::string::npos);
    CHECK(deck.find(".end") != std::string::npos);
    // one L and one C per line per section, plus the mutual pair
    auto count = [&](const std::string& tok) {
        size_t n = 0, pos = 0;
        while ((pos = deck.find(tok, pos)) != std::string::npos) { ++n; ++pos; }
        return n;
    };
    CHECK(count("\nL0_") == 4);
    CHECK(count("\nL1_") == 4);
    CHECK(count("\nCm01_") == 4);
    CHECK(count("\nK01_") == 4);
    // the victim is terminated at both ends so NEXT and FEXT are observable
    CHECK(deck.find("Rnear1 n1_0 0") != std::string::npos);
    CHECK(deck.find("Rfar1 n1_4 0") != std::string::npos);
}

TEST_CASE("rlgc: an unphysical inductance matrix is refused", "[rlgc][deck]") {
    Rlgc p;
    p.n = 2;
    p.C = {1e-10, 0, 0, 1e-10};
    p.L = {1e-7, 1e-7, 1e-7, 1e-7};   // k = 1 exactly: not physical for a deck
    p.R = {0, 0, 0, 0};
    CHECK_THROWS_WITH(spice_ladder_deck(p, DeckOptions{}),
                      Catch::Matchers::ContainsSubstring("not physical"));
}

TEST_CASE("cross-section: coupled microstrip geometry is built correctly",
          "[crosssection]") {
    // 0.3 mm traces, 0.5 mm centre spacing, 0.2 mm above the plane, 35 um copper
    CrossSection cs = make_coupled_section(0.3, 0.3, 0.5, 0.2, 0.035, 4.4);
    REQUIRE(cs.conductors.size() == 3);
    CHECK(cs.conductors[0].name == "conductor_gnd");
    CHECK(cs.conductors[1].name == "conductor_a");
    CHECK(cs.conductors[2].name == "conductor_b");
    // the reference plane must be in the section, spanning it
    CHECK(cs.conductors[0].x0 == Approx(0.0));
    CHECK(cs.conductors[0].x1 == Approx(cs.width));
    // traces sit one dielectric height above the plane
    CHECK(cs.conductors[1].y0 == Approx(0.035e-3 + 0.2e-3));
    // and are 0.5 mm apart, centre to centre
    const double xa = 0.5 * (cs.conductors[1].x0 + cs.conductors[1].x1);
    const double xb = 0.5 * (cs.conductors[2].x0 + cs.conductors[2].x1);
    CHECK(xb - xa == Approx(0.5e-3));
    // permittivity is the board's between plane and trace, air above
    CHECK(cs.eps_at(0.035e-3 + 0.1e-3) == Approx(4.4));
    CHECK(cs.eps_at(cs.height - 1e-6) == Approx(1.0));
    // region lookup: inside a trace is that conductor, outside is dielectric
    CHECK(cs.region_at(xa, 0.035e-3 + 0.2e-3 + 0.01e-3) == "conductor_a");
    CHECK(cs.region_at(xa, 0.035e-3 + 0.1e-3).rfind("dielectric", 0) == 0);
    CHECK(cs.nx > 100);
    CHECK(cs.ny > 50);
}

TEST_CASE("cross-section: impossible geometry is refused", "[crosssection]") {
    // separation smaller than the two half-widths means overlapping copper
    CHECK_THROWS_WITH(make_coupled_section(0.3, 0.3, 0.2, 0.2, 0.035, 4.4),
                      Catch::Matchers::ContainsSubstring("smaller than the two half-widths"));
    CHECK_THROWS(make_coupled_section(0.0, 0.3, 0.5, 0.2, 0.035, 4.4));
    CHECK_THROWS(make_coupled_section(0.3, 0.3, 0.5, 0.0, 0.035, 4.4));
}

TEST_CASE("cross-section: gmsh mesh round-trips through the region map",
          "[crosssection]") {
    CrossSection cs = make_coupled_section(0.3, 0.3, 0.5, 0.2, 0.035, 4.4);
    const std::string path = std::string(FARADAY_FIXTURE_DIR) + "/../.tmp_section.msh";

    // A mesh too coarse to resolve the 35 um copper would drop an electrode
    // entirely and yield a capacitance matrix missing a conductor. Refused.
    {
        CrossSection coarse = cs;
        coarse.nx = 40;
        coarse.ny = 20;
        CHECK_THROWS_WITH(coarse.write_gmsh(path),
                          Catch::Matchers::ContainsSubstring("vanish from the mesh"));
    }

    cs.write_gmsh(path);
    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string m = ss.str();
    CHECK(m.rfind("$MeshFormat", 0) == 0);
    CHECK(m.find("$PhysicalNames") != std::string::npos);
    CHECK(m.find("\"conductor_a\"") != std::string::npos);
    CHECK(m.find("\"conductor_b\"") != std::string::npos);
    CHECK(m.find("\"conductor_gnd\"") != std::string::npos);
    CHECK(m.find("$Elements\n" + std::to_string(cs.nx * cs.ny) + "\n")
          != std::string::npos);
    std::remove(path.c_str());
}

TEST_CASE("rlgc: crosstalk peaks are read as excursions from quiescent",
          "[rlgc][deck]") {
    // near-end swings positive, far-end negative — the classic signature
    std::vector<double> near{0, 0.01, 0.064, 0.03, 0};
    std::vector<double> far{0, -0.005, -0.028, -0.01, 0};
    faraday::CrosstalkPeaks p = crosstalk_from_waveforms(near, far, 1.69);
    CHECK(p.next_v == Approx(0.064));
    CHECK(p.fext_v == Approx(-0.028));    // sign preserved: direction matters
    CHECK(p.next_db == Approx(20.0 * std::log10(0.064 / 1.69)).margin(0.01));
    CHECK(p.fext_db < p.next_db);
    // a node that never moved is a floor, not a nan
    CHECK(crosstalk_from_waveforms({0, 0, 0}, {0, 0, 0}, 1.0).next_db == -300.0);
    CHECK_THROWS_WITH(crosstalk_from_waveforms({}, {0.0}, 1.0),
                      Catch::Matchers::ContainsSubstring("empty waveform"));
    CHECK_THROWS(crosstalk_from_waveforms({0.1}, {0.1}, 0.0));
}

TEST_CASE("cross-section: graded axis puts fine cells at every feature",
          "[crosssection][graded]") {
    const double fine = 2e-6;
    auto ax = CrossSection::graded_axis({0.001, 0.002}, 0.0, 0.003, fine, 1.25, 4000);
    REQUIRE(ax.size() > 10);
    CHECK(ax.front() == Approx(0.0));
    CHECK(ax.back() == Approx(0.003));
    // strictly increasing
    for (size_t i = 1; i < ax.size(); ++i) CHECK(ax[i] > ax[i - 1]);
    // the cell straddling each feature is ~fine, not a partly-grown one
    auto cell_at = [&](double f) {
        for (size_t i = 0; i + 1 < ax.size(); ++i)
            if (f >= ax[i] && f <= ax[i + 1]) return ax[i + 1] - ax[i];
        return 1e30;
    };
    CHECK(cell_at(0.001) == Approx(fine).epsilon(0.3));
    CHECK(cell_at(0.002) == Approx(fine).epsilon(0.3));
    // and cells grow away from the features, so the count stays affordable:
    // a uniform mesh at this resolution would need 1500 cells
    CHECK(ax.size() < 400);
    double mid = cell_at(0.0005);          // far from both features
    CHECK(mid > 5 * fine);
}

TEST_CASE("cross-section: grading reports the cell size it ACTUALLY achieved",
          "[crosssection][graded]") {
    // The guard that protects a skin-effect solve must measure the mesh it got,
    // not the one it asked for — requesting `fine` does not by itself deliver
    // `fine` cells at a conductor.
    CrossSection cs = make_coupled_section(0.3, 0.3, 0.5, 0.2, 0.035, 4.4);
    const double fine = 3e-6;
    cs.grade_for(fine);
    CHECK(cs.max_cell_at_conductors() == Approx(fine).epsilon(0.35));
    CHECK(cs.nx > 100);
    CHECK(cs.ny > 60);
    // a graded mesh of a few 10k cells replaces a uniform one of millions
    CHECK((size_t)cs.nx * cs.ny < 200000);
    const std::string path = std::string(FARADAY_FIXTURE_DIR) + "/../.tmp_graded.msh";
    cs.write_gmsh(path);            // still a valid mesh with all electrodes
    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream ss; ss << in.rdbuf();
    CHECK(ss.str().find("\"conductor_gnd\"") != std::string::npos);
    std::remove(path.c_str());
}

TEST_CASE("tline: elliptic K matches known values", "[tline][cohn]") {
    CHECK(tline::elliptic_K(0.0) == Approx(M_PI / 2));
    CHECK(tline::elliptic_K(std::sin(M_PI / 6)) == Approx(1.6857503548).epsilon(1e-9));
    CHECK(tline::elliptic_K(std::sin(M_PI / 4)) == Approx(1.8540746773).epsilon(1e-9));
    CHECK(tline::elliptic_K(0.99) > tline::elliptic_K(0.5));   // diverges at k->1
    CHECK_THROWS(tline::elliptic_K(1.0));
}

TEST_CASE("tline: Cohn coupled stripline is exact and behaves", "[tline][cohn]") {
    // Coupled striplines sit in a homogeneous medium, so this conformal-mapping
    // result is exact — which is what makes it a yardstick for the numerical
    // extraction's MUTUAL terms rather than just its self terms.
    auto eo = tline::coupled_stripline_cohn(0.4, 0.5, 1.0, 4.4);
    CHECK(eo.z_even > eo.z_odd);                    // always, for coupled lines
    CHECK(eo.z_even == Approx(57.83).margin(0.5));
    CHECK(eo.z_odd == Approx(49.61).margin(0.5));
    // widening the gap drives both modes toward the isolated-line value
    auto wide = tline::coupled_stripline_cohn(0.4, 20.0, 1.0, 4.4);
    CHECK(wide.z_even == Approx(wide.z_odd).epsilon(0.01));
    CHECK(wide.z_even < eo.z_even);
    CHECK(wide.z_odd > eo.z_odd);
    // and both scale as 1/sqrt(eps_r)
    auto e1 = tline::coupled_stripline_cohn(0.4, 0.5, 1.0, 1.0);
    CHECK(e1.z_even == Approx(eo.z_even * std::sqrt(4.4)).epsilon(1e-9));
    CHECK_THROWS(tline::coupled_stripline_cohn(0.0, 0.5, 1.0, 4.4));
}

TEST_CASE("cross-section: coupled stripline has BOTH planes as one electrode",
          "[crosssection][cohn]") {
    CrossSection cs = make_coupled_stripline(0.4, 0.5, 1.0, 0.018, 4.4);
    // four rectangles, but only three distinct electrodes: the two planes are
    // the same net, or the extraction would see an extra conductor
    REQUIRE(cs.conductors.size() == 4);
    int gnd = 0;
    for (const auto& c : cs.conductors) if (c.name == "conductor_gnd") ++gnd;
    CHECK(gnd == 2);
    // traces centred between the planes, in a single homogeneous dielectric
    const double ymid = 0.5 * (cs.conductors[1].y0 + cs.conductors[1].y1);
    CHECK(ymid == Approx(0.5 * cs.height).epsilon(0.02));
    REQUIRE(cs.slabs.size() == 1);
    CHECK(cs.slabs[0].eps_r == Approx(4.4));
    CHECK(cs.eps_at(ymid) == Approx(4.4));
    CHECK_THROWS(make_coupled_stripline(0.4, 0.5, 0.01, 0.018, 4.4));  // t >= b
}
