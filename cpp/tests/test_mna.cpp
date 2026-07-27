// The transient solver, pinned against transmission-line theory rather than
// against itself. Three of these have exact right answers:
//
//   * near-end crosstalk saturates at kb * V_launched once the round trip
//     exceeds the edge, and is proportionally smaller below that;
//   * forward crosstalk in a HOMOGENEOUS medium is identically zero, so the
//     residual is a direct readout of the ladder's discretisation error;
//   * with the aggressor settled, DC is a resistor divider.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <faraday/Bem2d.hpp>
#include <faraday/Mna.hpp>
#include <faraday/Rlgc.hpp>

using namespace faraday;
using Catch::Matchers::WithinRel;

namespace {

Rlgc rlgc_of(const bem::PairSection& s) {
    bem::Geometry g = bem::geometry_for(s);
    bem::Solution sd = bem::solve(g, false);
    bem::Solution sv = bem::solve(g, true);
    const size_t nt = 3;
    std::vector<double> M(nt * nt, 0.0), M0(nt * nt, 0.0);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            M[i * nt + j] = sd.at(i, j);
            M0[i * nt + j] = sv.at(i, j);
        }
    return rlgc_from_maxwell(M, M0, nt, 2);
}

Rlgc microstrip_pair() {
    bem::PairSection s;
    s.w1 = s.w2 = 0.25e-3;
    s.h = 0.2e-3;
    s.t = 35e-6;
    s.eps_r = 4.3;
    s.gap = 0.25e-3;
    return rlgc_of(s);
}

}  // namespace

TEST_CASE("near-end crosstalk saturates at kb times the launched wave", "[mna]") {
    const Rlgc p = microstrip_pair();
    const double z0 = p.z0(0);
    mna::DriveOptions o;
    o.z_src = z0;                  // matched everywhere, so the only thing the
    o.z_term = z0;                 // victim sees is coupling, not reflections
    o.z_victim_near = z0;
    o.rise_s = 0.5e-9;
    o.amplitude_v = 3.3;

    SECTION("long line: saturated") {
        for (double len_mm : {50.0, 120.0}) {
            o.length_m = len_mm * 1e-3;
            const mna::Waveforms w = mna::simulate(p, o);
            REQUIRE(2 * w.delay_s > o.rise_s);        // genuinely saturated
            INFO("length " << len_mm << " mm");
            CHECK_THAT(w.next_peak_v, WithinRel(p.kb(0, 1) * w.launched_v, 0.05));
        }
    }

    SECTION("short line: proportionally less") {
        o.length_m = 20e-3;
        const mna::Waveforms w = mna::simulate(p, o);
        REQUIRE(2 * w.delay_s < o.rise_s);
        const double saturated = p.kb(0, 1) * w.launched_v;
        // below saturation NEXT scales as 2*Td/tr
        CHECK(w.next_peak_v < saturated * 0.8);
        CHECK_THAT(w.next_peak_v,
                   WithinRel(saturated * 2 * w.delay_s / o.rise_s, 0.15));
    }
}

TEST_CASE("forward crosstalk vanishes in a homogeneous medium", "[mna]") {
    // FEXT is proportional to (Lm/L - Cm/C), which is exactly zero when every
    // mode travels at the same velocity. Any residual is the lumped ladder
    // failing to be a distributed line, so it must shrink with more sections.
    bem::PairSection sl;
    sl.stripline = true;
    sl.w1 = sl.w2 = 0.2e-3;
    sl.gap = 0.2e-3;
    sl.b = 0.8e-3;
    sl.t = 1e-6;
    sl.eps_r = 4.3;
    const Rlgc p = rlgc_of(sl);

    mna::DriveOptions o;
    o.length_m = 0.08;
    o.rise_s = 0.3e-9;
    o.z_src = o.z_term = o.z_victim_near = p.z0(0);
    o.max_steps = 4000;

    double prev = 1e9;
    for (int n : {8, 16, 32}) {
        o.sections = n;
        const mna::Waveforms w = mna::simulate(p, o);
        const double rel = std::abs(w.fext_peak_v / w.next_peak_v);
        INFO(n << " sections: FEXT is " << rel * 100 << "% of NEXT");
        CHECK(rel < 0.02);
        CHECK(rel < prev);
        prev = rel;
    }
}

TEST_CASE("forward crosstalk is real and grows with length in mixed media", "[mna]") {
    // Microstrip is NOT homogeneous — the even and odd modes see different
    // effective permittivity — so FEXT exists and accumulates linearly.
    const Rlgc p = microstrip_pair();
    mna::DriveOptions o;
    o.z_src = o.z_term = o.z_victim_near = p.z0(0);
    o.rise_s = 0.5e-9;

    o.length_m = 0.05;
    const double f1 = std::abs(mna::simulate(p, o).fext_peak_v);
    o.length_m = 0.10;
    const double f2 = std::abs(mna::simulate(p, o).fext_peak_v);
    CHECK(f1 > 1e-4);
    CHECK_THAT(f2 / f1, WithinRel(2.0, 0.2));
}

TEST_CASE("DC settles to the resistor divider", "[mna]") {
    const Rlgc p = microstrip_pair();
    mna::DriveOptions o;
    o.length_m = 0.05;
    o.rise_s = 0.5e-9;
    o.amplitude_v = 3.3;
    o.z_src = 30;
    o.z_term = 1e4;
    o.z_victim_near = 30;
    const mna::Waveforms w = mna::simulate(p, o);

    CHECK_THAT(w.agg_far.back(), WithinRel(3.3 * 1e4 / (1e4 + 30), 0.01));
    CHECK(std::abs(w.vic_near.back()) < 0.01);   // the victim ends up quiet
}

TEST_CASE("the far end responds one propagation delay later", "[mna]") {
    const Rlgc p = microstrip_pair();
    mna::DriveOptions o;
    o.length_m = 0.1;
    o.rise_s = 0.3e-9;
    o.z_src = o.z_term = o.z_victim_near = p.z0(0);
    const mna::Waveforms w = mna::simulate(p, o);

    double t50 = 0;
    const double half = 0.5 * w.launched_v;
    for (size_t i = 0; i < w.t.size(); ++i)
        if (w.agg_far[i] > half) { t50 = w.t[i]; break; }
    REQUIRE(t50 > 0);
    CHECK_THAT(t50, WithinRel(w.delay_s + o.rise_s / 2, 0.15));
}

TEST_CASE("an unterminated victim sees more noise than a matched one", "[mna]") {
    // This is the whole argument for termination, and the reason the transient
    // window runs for several round trips: stopping at the first backward wave
    // would under-report the unterminated case.
    const Rlgc p = microstrip_pair();
    mna::DriveOptions o;
    o.length_m = 0.08;
    o.rise_s = 0.5e-9;
    o.z_src = 30;

    o.z_term = o.z_victim_near = p.z0(0);
    const double matched = std::abs(mna::simulate(p, o).next_peak_v);
    o.z_term = o.z_victim_near = 1e4;
    const double open = std::abs(mna::simulate(p, o).next_peak_v);
    INFO("matched " << matched * 1e3 << " mV, open " << open * 1e3 << " mV");
    CHECK(open > matched);
}

TEST_CASE("bad drive parameters are refused", "[mna]") {
    const Rlgc p = microstrip_pair();
    mna::DriveOptions o;
    o.length_m = 0.05;
    o.rise_s = 0;
    CHECK_THROWS_AS(mna::simulate(p, o), std::invalid_argument);
    o.rise_s = 1e-9;
    o.z_src = 0;
    CHECK_THROWS_AS(mna::simulate(p, o), std::invalid_argument);
    o.z_src = 50;
    o.aggressor = 7;
    CHECK_THROWS_AS(mna::simulate(p, o), std::invalid_argument);
}
