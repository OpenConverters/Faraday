// Radiated-emission estimation. Everything on the right-hand side here is
// either a published constant, a published regulatory table, or an exact
// scaling law — no recorded outputs.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/Emissions.hpp>

using namespace faraday::emc;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

namespace {
Trapezoid demo() {
    Trapezoid t;
    t.amplitude_a = 10;
    t.f_sw_hz = 500e3;
    t.duty = 0.4;
    t.rise_s = 20e-9;
    return t;
}
}  // namespace

TEST_CASE("the loop coefficient is the one the textbooks publish", "[emc]") {
    // Paul and Ott both quote E = 1.3168e-14 f^2 I A / r for a small loop.
    // Composing it from eta0 and c rather than pasting the number means a typo
    // in either constant shows up here.
    CHECK_THAT(ETA0 * PI_E / (C_LIGHT * C_LIGHT), WithinRel(1.3168e-14, 1e-4));

    // and one fully hand-checkable evaluation
    CHECK_THAT(e_field_loop(100e6, 100e-6, 1.0, 3.0),
               WithinRel(1.3168e-14 * 1e16 * 1e-4 / 3.0, 1e-4));
}

TEST_CASE("field scales exactly as the physics says", "[emc]") {
    const double base = e_field_loop(100e6, 100e-6, 1.0, 3.0);
    CHECK_THAT(e_field_loop(100e6, 200e-6, 1.0, 3.0), WithinRel(2 * base, 1e-12));
    CHECK_THAT(e_field_loop(100e6, 100e-6, 2.0, 3.0), WithinRel(2 * base, 1e-12));
    CHECK_THAT(e_field_loop(100e6, 100e-6, 1.0, 6.0), WithinRel(base / 2, 1e-12));
    CHECK_THAT(e_field_loop(200e6, 100e-6, 1.0, 3.0), WithinRel(4 * base, 1e-12));
}

TEST_CASE("the trapezoid series is bounded by its envelope everywhere", "[emc]") {
    const Trapezoid t = demo();
    for (int n = 1; n <= 2000; ++n) {
        INFO("harmonic " << n);
        REQUIRE(harmonic_a(t, n) <= envelope_a(t, n) * (1.0 + 1e-9));
    }
    // and the envelope is not loose: it must be touched near the sinc peaks
    double best = 0;
    for (int n = 1; n <= 2000; ++n)
        best = std::max(best, harmonic_a(t, n) / envelope_a(t, n));
    CHECK(best > 0.9);
}

TEST_CASE("the series has the nulls a trapezoid must have", "[emc]") {
    const Trapezoid t = demo();          // duty 2/5, so every 5th line is a null
    for (int n : {5, 10, 80, 160}) {
        INFO("harmonic " << n << " should be a duty null");
        CHECK(harmonic_a(t, n) < 1e-12);
    }
    CHECK(harmonic_a(t, 7) > 1e-3);      // and neighbours are not
    // the edge null: n = T/tr = 100
    CHECK(harmonic_a(t, 100) < 1e-12);
}

TEST_CASE("the plateau closed form agrees with the series envelope", "[emc]") {
    // Above both breaks the radiated level is flat. plateau_v_per_m() derives
    // that in closed form; it must equal what the envelope actually does.
    const Trapezoid t = demo();
    const double area = 267e-6, r = 3.0;
    const double closed = plateau_v_per_m(area, t, r);
    for (int n : {200, 500, 1200, 1900}) {
        const double f = n * t.f_sw_hz;
        INFO("harmonic " << n << " at " << f * 1e-6 << " MHz");
        CHECK_THAT(e_field_loop(f, area, envelope_a(t, n), r),
                   WithinRel(closed, 1e-9));
    }
}

TEST_CASE("the plateau moves only with the four things it depends on", "[emc]") {
    // area, current, switching period and edge rate — each exactly 6 dB per
    // doubling. This is the check that caught the plateau being sampled off a
    // spectral null, where halving the edge appeared to change it by 295 dB.
    const Trapezoid t = demo();
    PredictOptions o;
    const double base = predict_loop(100.0, t, o).plateau_dbuv_m;

    CHECK_THAT(predict_loop(200.0, t, o).plateau_dbuv_m - base, WithinAbs(6.0206, 1e-3));
    Trapezoid i2 = t; i2.amplitude_a *= 2;
    CHECK_THAT(predict_loop(100.0, i2, o).plateau_dbuv_m - base, WithinAbs(6.0206, 1e-3));
    Trapezoid e2 = t; e2.rise_s /= 2;
    CHECK_THAT(predict_loop(100.0, e2, o).plateau_dbuv_m - base, WithinAbs(6.0206, 1e-3));
    Trapezoid f2 = t; f2.f_sw_hz *= 2;
    CHECK_THAT(predict_loop(100.0, f2, o).plateau_dbuv_m - base, WithinAbs(6.0206, 1e-3));
    // and NOT with duty, which only moves the first break
    Trapezoid d2 = t; d2.duty = 0.2;
    CHECK_THAT(predict_loop(100.0, d2, o).plateau_dbuv_m, WithinAbs(base, 1e-6));
}

TEST_CASE("limit lines match the published tables", "[emc]") {
    // FCC 47 CFR 15.109(a) Class B at 3 m is given in uV/m: 100, 150, 200, 500
    const LimitLine& fcc = limit_by_id("fcc15b");
    CHECK(fcc.distance_m == 3.0);
    CHECK_THAT(*limit_at(fcc, 50e6), WithinAbs(20 * std::log10(100.0), 1e-9));
    CHECK_THAT(*limit_at(fcc, 100e6), WithinAbs(20 * std::log10(150.0), 0.01));
    CHECK_THAT(*limit_at(fcc, 500e6), WithinAbs(20 * std::log10(200.0), 0.01));
    CHECK_THAT(*limit_at(fcc, 980e6), WithinAbs(20 * std::log10(500.0), 0.01));

    // CISPR 32 / EN 55032 Class B at 3 m: 40 and 47 dBuV/m
    const LimitLine& c = limit_by_id("cispr32b");
    CHECK(c.distance_m == 3.0);
    CHECK_THAT(*limit_at(c, 100e6), WithinAbs(40.0, 1e-9));
    CHECK_THAT(*limit_at(c, 500e6), WithinAbs(47.0, 1e-9));
    // Class A is specified at 10 m, and that distance must be carried through
    CHECK(limit_by_id("cispr32a").distance_m == 10.0);

    // at a band edge the TIGHTER limit applies
    CHECK_THAT(*limit_at(c, 230e6), WithinAbs(40.0, 1e-9));
    CHECK_THAT(*limit_at(fcc, 88e6), WithinAbs(40.0, 1e-9));
    // and nothing is regulated below 30 MHz on this axis
    CHECK_FALSE(limit_at(c, 10e6).has_value());
}

TEST_CASE("a loop that is no longer electrically small is flagged", "[emc]") {
    // The f^2 growth is only real while the current is uniform around the loop.
    // A 267 mm^2 loop has a ~58 mm perimeter, so quarter-wave lands near 1.3 GHz.
    const double area = 267e-6;
    CHECK_THAT(loop_perimeter_m(area), WithinRel(2 * std::sqrt(PI_E * area), 1e-12));
    CHECK_THAT(small_loop_max_hz(area),
               WithinRel(C_LIGHT / (4 * loop_perimeter_m(area)), 1e-12));

    // a big loop pushes that ceiling into the measured band, and the harmonics
    // above it must be marked rather than believed
    Trapezoid t = demo();
    PredictOptions o;
    Prediction p = predict_loop(40000.0, t, o);    // 40 cm^2
    CHECK(p.small_loop_max_hz < 1000e6);
    CHECK(p.beyond_model_count > 0);
    for (const auto& h : p.harmonics)
        CHECK(h.beyond_small_loop == (h.f_hz > p.small_loop_max_hz));
}

TEST_CASE("the headline margin is taken against the envelope, not the nulls",
          "[emc]") {
    const Trapezoid t = demo();
    PredictOptions o;
    Prediction p = predict_loop(267.0, t, o);
    // the line spectrum dips into its nulls, so its best-case margin is always
    // at least as good as the envelope's — never worse
    CHECK(p.worst_harmonic_margin_db >= p.worst_margin_db - 1e-9);
    for (const auto& h : p.harmonics) {
        CHECK(h.e_dbuv_m <= h.envelope_dbuv_m + 1e-9);
        if (!h.beyond_small_loop)
            CHECK(h.envelope_margin_db >= p.worst_margin_db - 1e-9);
    }
    // the worst envelope level IS the plateau for this source, because the
    // edge knee (16 MHz) sits below the regulated band
    CHECK(p.knee_hz < 30e6);
    CHECK_THAT(p.worst_level_dbuv_m, WithinAbs(p.plateau_dbuv_m, 0.01));
}

TEST_CASE("ground reflection is worth exactly 6 dB and can be turned off",
          "[emc]") {
    const Trapezoid t = demo();
    PredictOptions on, off;
    off.ground_reflection = false;
    CHECK_THAT(predict_loop(267.0, t, on).plateau_dbuv_m -
                   predict_loop(267.0, t, off).plateau_dbuv_m,
               WithinAbs(6.0206, 1e-6));
}

TEST_CASE("unresolved harmonics are flagged for slow switchers", "[emc]") {
    // Below the receiver's 120 kHz bandwidth several harmonics land in one bin
    // and the reading exceeds any single line drawn.
    Trapezoid slow = demo();
    slow.f_sw_hz = 60e3;
    CHECK(predict_loop(267.0, slow, PredictOptions{}).harmonics_unresolved);
    CHECK_FALSE(predict_loop(267.0, demo(), PredictOptions{}).harmonics_unresolved);
}

TEST_CASE("bad sources and unknown standards are refused", "[emc]") {
    Trapezoid t = demo();
    t.duty = 1.5;
    CHECK_THROWS_AS(harmonic_a(t, 1), std::invalid_argument);
    t = demo();
    t.rise_s = 1e-3;                    // edge longer than the whole pulse
    CHECK_THROWS_WITH(harmonic_a(t, 1),
                      Catch::Matchers::ContainsSubstring("triangular"));
    CHECK_THROWS_AS(predict_loop(0.0, demo(), PredictOptions{}), std::invalid_argument);
    PredictOptions bad;
    bad.limit_id = "en55011-b";
    CHECK_THROWS_WITH(predict_loop(267.0, demo(), bad),
                      Catch::Matchers::ContainsSubstring("unknown limit line"));
}

TEST_CASE("a realistic converter loop lands where an engineer expects", "[emc]") {
    // 267 mm^2 (what Faraday measured on the MPPT board), 10 A switched, 20 ns
    // edges, 500 kHz. That is a marginal design, and the estimate should say so
    // rather than passing it comfortably or condemning it absurdly.
    Prediction p = predict_loop(267.0, demo(), PredictOptions{});
    CHECK(p.worst_margin_db > -20.0);
    CHECK(p.worst_margin_db < 10.0);
    // halving the loop area must buy exactly 6 dB of margin
    Prediction half = predict_loop(133.5, demo(), PredictOptions{});
    CHECK_THAT(half.worst_margin_db - p.worst_margin_db, WithinAbs(6.0206, 0.01));
}
