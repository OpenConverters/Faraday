// The boundary-element extraction, pinned against results that do not come
// from Faraday: a closed-form conformal map, a classical equivalent radius,
// Hammerstad's curve fit, and an exact identity of quasi-TEM theory.
//
// These are not regression baselines captured from a previous run. Every
// number on the right-hand side is independently derivable, which is the only
// kind of check that can catch a solver that is wrong in a self-consistent way
// — and this solver WAS, until the atan2 branch bug documented in Bem2d.hpp
// turned up here as a converged-but-wrong capacitance.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <faraday/Bem2d.hpp>
#include <faraday/Rlgc.hpp>
#include <faraday/Tline.hpp>

using namespace faraday;
using Catch::Matchers::WithinRel;

namespace {

Rlgc rlgc_of(const bem::PairSection& s, size_t* panels = nullptr) {
    bem::Geometry g = bem::geometry_for(s);
    bem::Solution sd = bem::solve(g, false);
    bem::Solution sv = bem::solve(g, true);
    if (panels) *panels = sd.panels.size();
    const size_t nt = 3;
    std::vector<double> M(nt * nt, 0.0), M0(nt * nt, 0.0);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            M[i * nt + j] = sd.at(i, j);
            M0[i * nt + j] = sv.at(i, j);
        }
    return rlgc_from_maxwell(M, M0, nt, 2);
}

}  // namespace

TEST_CASE("square conductor matches its classical equivalent radius", "[bem]") {
    // A square of side a has the logarithmic capacity (equivalent radius)
    //     r_eq = a * Gamma(1/4)^2 / (4 pi^(3/2)) = 0.59017 a
    // so over a ground plane at distance d >> a its capacitance per unit
    // length is 2 pi eps0 / ln(2d / r_eq). This is the check that isolates the
    // closed-contour path from the coplanar-strip one.
    const double a = 100e-6, d = 600e-6;
    const double want = 2 * bem::PI_ * bem::EPS0 / std::log(2 * d / (0.59017 * a));

    bem::Geometry g;
    g.n_signal = 1;
    g.conductors.push_back({-a / 2, d - a / 2, a / 2, d + a / 2, 0});
    g.eps_top = 1.0;
    bem::Solution s = bem::solve(g, true);

    CHECK_THAT(s.at(0, 0), WithinRel(want, 0.01));

    // The charge density on a conductor held above a grounded plane is
    // positive EVERYWHERE. A sign change means the discretisation is
    // oscillating, which is how the branch-cut bug first showed itself.
    for (double sigma : s.sigma[0]) CHECK(sigma > 0.0);
}

TEST_CASE("coupled stripline reproduces Cohn's exact solution", "[bem]") {
    // Coupled striplines sit in a homogeneous medium, so the mode is purely
    // TEM and conformal mapping gives Z_even and Z_odd EXACTLY — no curve fit
    // and no validity window. It is the only benchmark available for the
    // MUTUAL terms, which are the entire point of a crosstalk tool.
    const double w = 0.2, b = 0.8, er = 4.3;
    for (double gap : {0.15, 0.3, 0.6}) {
        bem::PairSection ps;
        ps.stripline = true;
        ps.w1 = ps.w2 = w * 1e-3;
        ps.gap = gap * 1e-3;
        ps.b = b * 1e-3;
        ps.t = 1e-6;                     // Cohn assumes zero thickness
        ps.eps_r = er;
        const Rlgc p = rlgc_of(ps);

        const double Le = p.at(p.L, 0, 0) + p.at(p.L, 0, 1);
        const double Lo = p.at(p.L, 0, 0) - p.at(p.L, 0, 1);
        const double Ce = p.at(p.C, 0, 0) + p.at(p.C, 0, 1);
        const double Co = p.at(p.C, 0, 0) - p.at(p.C, 0, 1);
        const auto exact = tline::coupled_stripline_cohn(w, gap, b, er);

        INFO("gap = " << gap << " mm");
        CHECK_THAT(std::sqrt(Le / Ce), WithinRel(exact.z_even, 0.005));
        CHECK_THAT(std::sqrt(Lo / Co), WithinRel(exact.z_odd, 0.005));
    }
}

TEST_CASE("L*C is mu0 eps0 eps_r times identity in a homogeneous medium", "[bem]") {
    // Quasi-TEM theory makes this an EXACT matrix identity, off-diagonals
    // included. L and C come from two separate solves — one with the
    // dielectric, one in vacuum — so nothing but a correct formulation makes
    // their product collapse to a scaled identity. It is the single most
    // sensitive check in the suite: the branch-cut bug showed here as a 1.6%
    // error while every scalar figure still looked plausible.
    for (double er : {1.0, 4.3, 10.0}) {
        bem::PairSection s;
        s.stripline = true;
        s.w1 = s.w2 = 0.2e-3;
        s.gap = 0.2e-3;
        s.b = 0.8e-3;
        s.t = 1e-6;
        s.eps_r = er;
        const Rlgc p = rlgc_of(s);
        const double want = bem::MU0 * bem::EPS0 * er;

        for (size_t i = 0; i < 2; ++i)
            for (size_t j = 0; j < 2; ++j) {
                double m = 0;
                for (size_t k = 0; k < 2; ++k) m += p.at(p.L, i, k) * p.at(p.C, k, j);
                INFO("eps_r = " << er << "  element " << i << "," << j);
                if (i == j) CHECK_THAT(m, WithinRel(want, 1e-9));
                else CHECK(std::abs(m) < want * 1e-9);
            }
    }
}

TEST_CASE("microstrip Z0 tracks Hammerstad once thickness is accounted for", "[bem]") {
    // Hammerstad's synthesis equations assume zero-thickness copper. Real
    // 35 um foil raises the capacitance and lowers Z0, and the IPC-2141
    // effective-width correction w + (t/pi)(1 + ln(2h/t)) is the standard way
    // to compare like with like. What must hold is that the RESIDUAL shrinks
    // as t/w shrinks — that is the signature of a real thickness effect
    // rather than a solver error that happens to scale.
    const double h = 0.2, t = 0.035;
    const double dw = (t / bem::PI_) * (1.0 + std::log(2 * h / t));
    double prev_err = 1e9;
    for (double w : {0.15, 0.25, 0.5, 1.0}) {
        bem::PairSection s;
        s.w1 = s.w2 = w * 1e-3;
        s.h = h * 1e-3;
        s.t = t * 1e-3;
        s.eps_r = 4.3;
        s.gap = 5e-3;                     // far enough apart to read self terms
        const double z = rlgc_of(s).z0(0);
        const double corrected = tline::microstrip_z0(w + dw, h, 4.3);
        const double err = std::abs(z - corrected) / corrected;
        INFO("w = " << w << " mm, BEM " << z << " vs corrected " << corrected);
        CHECK(err < 0.04);
        CHECK(err < prev_err);            // monotonically better as t/w falls
        prev_err = err;
    }
}

TEST_CASE("coupling falls monotonically with separation", "[bem]") {
    double prev = 1e9;
    for (double gap : {0.1, 0.2, 0.4, 0.8}) {
        bem::PairSection s;
        s.w1 = s.w2 = 0.25e-3;
        s.h = 0.2e-3;
        s.t = 35e-6;
        s.eps_r = 4.3;
        s.gap = gap * 1e-3;
        const double kb = rlgc_of(s).kb(0, 1);
        INFO("gap = " << gap);
        CHECK(kb > 0.0);
        CHECK(kb < prev);
        prev = kb;
    }
}

TEST_CASE("the screening closed form is optimistic, and by a stable amount", "[bem]") {
    // The findings list tells the user the closed form runs optimistic. That
    // claim is only worth making if it is measured, and it is only useful for
    // RANKING if the offset is roughly constant across geometries.
    double lo = 1e9, hi = -1e9;
    for (double gap : {0.1, 0.2, 0.4, 0.8}) {
        bem::PairSection s;
        s.w1 = s.w2 = 0.25e-3;
        s.h = 0.2e-3;
        s.t = 35e-6;
        s.eps_r = 4.3;
        s.gap = gap * 1e-3;
        const double solved = 20 * std::log10(rlgc_of(s).kb(0, 1));
        const double screen =
            20 * std::log10(tline::next_sat_edge(0.25 + gap, 0.2));
        const double offset = solved - screen;
        CHECK(offset > 0.0);              // the field solve always finds MORE
        lo = std::min(lo, offset);
        hi = std::max(hi, offset);
    }
    INFO("screening optimism spans " << lo << " to " << hi << " dB");
    CHECK(hi - lo < 3.0);
}

TEST_CASE("truncating the dielectric interface has converged by default", "[bem]") {
    auto z_at = [](double tf) {
        bem::PairSection s;
        s.w1 = s.w2 = 0.25e-3;
        s.h = 0.2e-3;
        s.t = 35e-6;
        s.eps_r = 4.3;
        s.gap = 0.25e-3;
        bem::Geometry g = bem::geometry_for(s);
        g.truncate_factor = tf;
        bem::Solution sd = bem::solve(g, false);
        bem::Solution sv = bem::solve(g, true);
        const size_t nt = 3;
        std::vector<double> M(nt * nt, 0.0), M0(nt * nt, 0.0);
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) {
                M[i * nt + j] = sd.at(i, j);
                M0[i * nt + j] = sv.at(i, j);
            }
        return rlgc_from_maxwell(M, M0, nt, 2).z0(0);
    };
    CHECK_THAT(z_at(12.0), WithinRel(z_at(60.0), 0.002));
}

TEST_CASE("a cross-section too thin to panel is refused, not fudged", "[bem]") {
    // A 10 mm pour 1 um thick cannot be discretised as a closed contour at any
    // sane panel count. The answer is to say so.
    bem::Geometry g;
    g.n_signal = 1;
    g.conductors.push_back({-5e-3, 1e-4, 5e-3, 1e-4 + 1e-9, 0});
    g.eps_top = 1.0;
    CHECK_THROWS_AS(bem::solve(g, true), std::invalid_argument);
}

TEST_CASE("field sampling reproduces the boundary conditions it was solved for",
          "[bem]") {
    bem::PairSection s;
    s.w1 = s.w2 = 0.25e-3;
    s.h = 0.2e-3;
    s.t = 35e-6;
    s.eps_r = 4.3;
    s.gap = 0.25e-3;
    bem::Solution sd = bem::solve(bem::geometry_for(s), false);
    bem::FieldMap f = bem::sample_field(sd, 0, -1e-3, 0.0, 1e-3, 0.8e-3, 60, 30);

    REQUIRE(f.v.size() == 60 * 30);
    // the imaged plane is at y = 0, so the whole bottom row must be at 0 V
    for (int ix = 0; ix < 60; ++ix) CHECK(std::abs(f.v[ix]) < 1e-6);
    // and nothing anywhere may exceed the 1 V the aggressor is driven to
    for (float v : f.v) CHECK(v < 1.0f + 1e-4f);
    CHECK(f.e_max > 0.0);
}
