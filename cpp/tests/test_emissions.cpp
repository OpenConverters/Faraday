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

// ---------------------------------------------------------------------------
// Common-mode radiation from an attached cable
// ---------------------------------------------------------------------------

TEST_CASE("the common-mode constant is Ott's, composed not pasted", "[emc][cm]") {
    // Ott quotes E = 1.257e-6 f I L / r for a cable over a ground plane. That
    // is the free-space short-dipole value eta0/(2c) DOUBLED by the ground
    // image, so with the reflection factor applied it must reproduce exactly.
    CHECK_THAT(ETA0 / (2 * C_LIGHT), WithinRel(6.283e-7, 1e-3));
    CHECK_THAT(GROUND_REFLECTION * ETA0 / (2 * C_LIGHT), WithinRel(1.2566e-6, 1e-3));

    // one hand-checkable evaluation: 1 mA, 50 MHz, 1 m cable, 3 m away
    CHECK_THAT(cm_e_field(50e6, 1e-3, 1.0, 3.0),
               WithinRel(6.283e-7 * 50e6 * 1e-3 * 1.0 / 3.0, 1e-3));
}

TEST_CASE("a cable stops lengthening once it passes a quarter wave", "[emc][cm]") {
    // Below lambda/4 the whole cable counts; above it the short-antenna formula
    // would keep growing without bound, which is simply wrong.
    CHECK_THAT(effective_length_m(1.0, 30e6), WithinRel(1.0, 1e-12));
    CHECK_THAT(effective_length_m(1.0, 300e6),
               WithinRel(C_LIGHT / (4 * 300e6), 1e-12));   // 0.2498 m, not 0.25
    CHECK_THAT(effective_length_m(5.0, 30e6),
               WithinRel(C_LIGHT / (4 * 30e6), 1e-12));
    // so the field saturates rather than climbing with length
    const double a = cm_e_field(300e6, 1e-3, 1.0, 3.0);
    const double b = cm_e_field(300e6, 1e-3, 9.0, 3.0);
    CHECK_THAT(b, WithinRel(a, 1e-12));
}

TEST_CASE("the current budget reproduces the classic rule of thumb", "[emc][cm]") {
    // The number every EMC text quotes: to meet Class B at 3 m with a 1 m
    // cable, common-mode current has to stay in the SINGLE MICROAMPS. That it
    // is microamps and not milliamps is the whole point — an ordinary current
    // probe cannot see it.
    // With the quarter-wave cap in force the tightest point on a 1 m cable
    // lands near 3 uA: above 75 MHz the cable stops counting as longer, so the
    // budget falls more slowly than 1/f and never reaches the ~1 uA an uncapped
    // formula would predict at 230 MHz.
    const CmBudget b = cm_budget(1.0, "cispr32b");
    CHECK(b.tightest_a > 1e-6);
    CHECK(b.tightest_a < 5e-6);

    // at the bottom of the band it is looser, around 8 uA
    const double at30 = cm_current_budget_a(40.0, 30e6, 1.0, 3.0, GROUND_REFLECTION);
    CHECK_THAT(at30, WithinRel(8e-6, 0.05));

    // the budget falls as 1/f inside a band
    const double at60 = cm_current_budget_a(40.0, 60e6, 1.0, 3.0, GROUND_REFLECTION);
    CHECK_THAT(at60, WithinRel(at30 / 2, 1e-9));
}

TEST_CASE("the budget scales the way the antenna does", "[emc][cm]") {
    // 30 MHz, where lambda/4 is 2.5 m, so both cables below are still short and
    // the length term is the honest one rather than the capped one.
    const double base = cm_current_budget_a(40.0, 30e6, 1.0, 3.0, GROUND_REFLECTION);
    CHECK_THAT(cm_current_budget_a(40.0, 30e6, 2.0, 3.0, GROUND_REFLECTION),
               WithinRel(base / 2, 1e-9));
    CHECK_THAT(cm_current_budget_a(40.0, 30e6, 1.0, 6.0, GROUND_REFLECTION),
               WithinRel(base * 2, 1e-9));
    CHECK_THAT(cm_current_budget_a(46.0206, 30e6, 1.0, 3.0, GROUND_REFLECTION),
               WithinRel(base * 2, 1e-3));
    CHECK_THAT(cm_current_budget_a(40.0, 30e6, 1.0, 3.0, 1.0),
               WithinRel(base * 2, 1e-9));

    // and PAST the quarter wave the length term stops helping, which is the
    // cap doing its job: a 2 m cable at 50 MHz counts as 1.5 m, not 2 m.
    CHECK_THAT(cm_current_budget_a(40.0, 50e6, 2.0, 3.0, GROUND_REFLECTION),
               WithinRel(cm_current_budget_a(40.0, 50e6, 1.5, 3.0, GROUND_REFLECTION),
                         1e-9));
}

TEST_CASE("the budget curve is self-consistent and marks resonance", "[emc][cm]") {
    const CmBudget b = cm_budget(1.0, "cispr32b");
    REQUIRE(b.points.size() > 50);
    CHECK_THAT(b.quarter_wave_hz, WithinRel(C_LIGHT / 4.0, 1e-12));

    bool saw_tightest = false;
    for (const auto& p : b.points) {
        CHECK(p.budget_a >= b.tightest_a - 1e-18);
        CHECK(p.resonant == (p.f_hz > b.quarter_wave_hz));
        CHECK(p.eff_len_m <= 1.0 + 1e-12);
        // every point must round-trip: put the budget current in, get the limit
        CHECK_THAT(to_dbuv_m(GROUND_REFLECTION *
                             cm_e_field(p.f_hz, p.budget_a, b.cable_m, b.distance_m)),
                   WithinAbs(p.limit_dbuv_m, 1e-6));
        if (std::abs(p.f_hz - b.tightest_f_hz) < 1e-6) saw_tightest = true;
    }
    CHECK(saw_tightest);
}

TEST_CASE("Class A at 10 m allows more common-mode current than Class B at 3 m",
          "[emc][cm]") {
    CHECK(cm_budget(1.0, "cispr32a").tightest_a >
          cm_budget(1.0, "cispr32b").tightest_a);
}

TEST_CASE("a cable with no length is refused", "[emc][cm]") {
    CHECK_THROWS_AS(cm_budget(0.0, "cispr32b"), std::invalid_argument);
    CHECK_THROWS_AS(effective_length_m(-1.0, 50e6), std::invalid_argument);
    CHECK_THROWS_AS(cm_budget(1.0, "mil-std-461"), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Conducted: the limit lines, the verdict, and the common-mode source term
// ---------------------------------------------------------------------------

TEST_CASE("CISPR 32 mains conducted limits match the published table",
          "[emc][conducted]") {
    const ConductedLimit& b_qp = conducted_limit_by_id("cispr32b-qp");
    CHECK(b_qp.detector == "quasi-peak");
    // Class B QP: 66 dBuV at 150 kHz falling to 56 at 500 kHz, flat 56 to
    // 5 MHz, 60 from 5 to 30 MHz. Same numbers Hertz carries.
    CHECK_THAT(*conducted_limit_at(b_qp, 150e3), WithinAbs(66.0, 1e-9));
    CHECK_THAT(*conducted_limit_at(b_qp, 500e3), WithinAbs(56.0, 1e-9));
    CHECK_THAT(*conducted_limit_at(b_qp, 1e6), WithinAbs(56.0, 1e-9));
    CHECK_THAT(*conducted_limit_at(b_qp, 30e6), WithinAbs(60.0, 1e-9));
    // the sloping segment is LOG-linear: the geometric midpoint of
    // 150-500 kHz sits exactly halfway down the 10 dB drop
    CHECK_THAT(*conducted_limit_at(b_qp, std::sqrt(150e3 * 500e3)),
               WithinAbs(61.0, 1e-9));
    // at a shared boundary the TIGHTER limit applies: Class A is 73 at
    // 500 kHz, not the 79 of the segment that ends there
    CHECK_THAT(*conducted_limit_at(conducted_limit_by_id("cispr32a-qp"), 500e3),
               WithinAbs(73.0, 1e-9));
    // average lines sit below their quasi-peak siblings, everywhere
    const ConductedLimit& b_av = conducted_limit_by_id("cispr32b-av");
    for (double f : {150e3, 300e3, 1e6, 10e6, 29e6})
        CHECK(*conducted_limit_at(b_av, f) < *conducted_limit_at(b_qp, f));
    CHECK_FALSE(conducted_limit_at(b_qp, 100e3).has_value());   // below the band
    CHECK_FALSE(conducted_limit_at(b_qp, 50e6).has_value());    // above it
    CHECK_THROWS_AS(conducted_limit_by_id("cispr11"), std::invalid_argument);
}

TEST_CASE("the design frequency is f_sw, or its first harmonic in the band",
          "[emc][conducted]") {
    CHECK(conducted_design_frequency(500e3) == 500e3);
    CHECK(conducted_design_frequency(150e3) == 150e3);
    // 100 kHz: nothing is measured below 150 kHz, so the filter is designed
    // at the second harmonic
    CHECK(conducted_design_frequency(100e3) == 200e3);
    CHECK_THAT(conducted_design_frequency(60e3), WithinAbs(180e3, 1e-6));
    CHECK_THROWS_AS(conducted_design_frequency(0.0), std::invalid_argument);
}

TEST_CASE("the conducted verdict names the mode, the frequency and the dB",
          "[emc][conducted]") {
    const Trapezoid t{10.0, 500e3, 0.4, 20e-9};
    const ConductedEstimate est = conducted_estimate(t, 10e-6, 10e-9, 0.01, 48.0, 50e-12);
    const ConductedVerdict v = conducted_verdict(est, t.f_sw_hz, "cispr32b-qp", 10.0);

    REQUIRE(!v.points.empty());
    CHECK(v.limit_label.find("Class B") != std::string::npos);
    CHECK(v.design_f_hz == 500e3);
    // hand values at 500 kHz (pinned in the estimate's own test): DM 95.65,
    // CM 101.15 dBuV against a 56 dBuV limit
    const ConductedPoint& p0 = v.points.front();
    CHECK(p0.f_hz == 500e3);
    CHECK_THAT(p0.limit_dbuv, WithinAbs(56.0, 1e-9));
    CHECK_THAT(p0.dm_margin_db, WithinAbs(56.0 - 95.65, 0.3));
    CHECK_THAT(p0.cm_margin_db, WithinAbs(56.0 - 101.15, 0.3));
    // A_req = level - limit + margin, at the design frequency (ANP015 §1)
    CHECK_THAT(v.required_cm_db, WithinAbs(101.15 - 56.0 + 10.0, 0.3));
    CHECK_THAT(v.required_dm_db, WithinAbs(95.65 - 56.0 + 10.0, 0.3));
    // the headline is the worse of the two modes, and it names which
    CHECK(v.worst_margin_db == std::min(v.dm_worst_margin_db, v.cm_worst_margin_db));
    CHECK((v.worst_mode == "CM" || v.worst_mode == "DM"));
    CHECK(v.worst_margin_db < 0);        // 10 A at 500 kHz with no filter fails
    CHECK(v.required_cm_band_db >= v.required_cm_db);   // the band is never kinder
    CHECK(v.cm_dominant_fraction >= 0.0);
    CHECK(v.cm_dominant_fraction <= 1.0);
    // This input capacitor self-resonates at ~503 kHz, which is why DM dips at
    // the fundamental and CM leads there; above resonance |Z_cin| = wL grows
    // faster than the CM path's w*C_stray*25, so DM takes over and there is no
    // frequency above which CM stays on top.
    CHECK(v.cm_crossover_hz == 0.0);
    CHECK(v.cm_dominant_fraction < 0.5);
}

TEST_CASE("a hundredfold C_stray makes it a common-mode problem, and says so",
          "[emc][conducted]") {
    const Trapezoid t{10.0, 500e3, 0.4, 20e-9};
    const ConductedVerdict v = conducted_verdict(
        conducted_estimate(t, 10e-6, 10e-9, 0.01, 48.0, 5e-9), t.f_sw_hz);
    CHECK(v.worst_mode == "CM");
    CHECK(v.cm_dominant_fraction == 1.0);
    CHECK(v.cm_crossover_hz == v.points.front().f_hz);   // CM on top from the start
    CHECK(v.required_cm_db > v.required_dm_db);
}

TEST_CASE("a verdict without a standard, or against an empty estimate, is refused",
          "[emc][conducted]") {
    const Trapezoid t{10.0, 500e3, 0.4, 20e-9};
    const ConductedEstimate est = conducted_estimate(t, 10e-6, 10e-9, 0.01, 48.0, 50e-12);
    CHECK_THROWS_AS(conducted_verdict(est, t.f_sw_hz, "en55011"), std::invalid_argument);
    CHECK_THROWS_AS(conducted_verdict(ConductedEstimate{}, t.f_sw_hz),
                    std::invalid_argument);
    CHECK_THROWS_AS(conducted_verdict(est, t.f_sw_hz, "cispr32b-qp", -1.0),
                    std::invalid_argument);
}

TEST_CASE("C_stray comes off the copper, and refuses to be invented",
          "[emc][conducted]") {
    // 400 mm^2 of switch-node copper 5 mm above a chassis:
    //   C = eps0 * 4e-4 / 5e-3 = 0.708 pF
    CHECK_THAT(chassis_stray_c_f(400.0, 5.0), WithinRel(0.7083e-12, 1e-3));
    // an FR4-mounted heatsink against the laminate is 4.5x that, and half the
    // distance doubles it — the two levers a designer actually has
    CHECK_THAT(chassis_stray_c_f(400.0, 5.0, 4.5),
               WithinRel(4.5 * 0.7083e-12, 1e-3));
    CHECK_THAT(chassis_stray_c_f(400.0, 2.5), WithinRel(2.0 * 0.7083e-12, 1e-3));
    CHECK_THROWS_AS(chassis_stray_c_f(0.0, 5.0), std::invalid_argument);
    CHECK_THROWS_AS(chassis_stray_c_f(400.0, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(chassis_stray_c_f(400.0, 5.0, 0.5), std::invalid_argument);
}

TEST_CASE("CISPR 25 Class 5 limits reproduce an accredited lab's own receiver trace",
          "[emc][conducted][cispr25][measured]") {
    // Everything else about this table is checked against the STANDARD, which
    // means against my reading of it. This case checks it against a receiver.
    //
    // TI's TIDA-01492 design guide (TIDUDA4, Figure 47) is a Rohde & Schwarz
    // screenshot from a CISPR 25 conducted-emissions run, with the Class 5
    // limit lines drawn on it by the instrument. Reading the red pixels back
    // out (scripts/digitize_ce_screenshot.py) recovers the levels the lab's
    // own limit table produced — an independent implementation, on real
    // equipment, by people who do this for a living.
    //
    // The digitisation is trustworthy because the same screenshot carries the
    // instrument's marker readout as a control: 13.42 dBuV at 30 MHz, against
    // 13.27 recovered. 0.15 dB on the axis mapping, so a decibel of
    // disagreement HERE would be the limit table, not the picture.
    //
    // Recovered, per band, peak and average (see docs/validation-conducted.md):
    //     MW 0.53-1.8 MHz   54.04 / 34.09 dBuV
    //     LW 0.15-0.30 MHz     -- / 50.05          (peak sits on the graticule)
    //     SW 5.9-6.2 MHz    53.1  / 33.1           (short band, fewer pixels)
    //     CB 26-28 MHz      44.1  / 24.1
    const ConductedLimit pk = cispr25_conducted(5, "peak");
    const ConductedLimit av = cispr25_conducted(5, "average");
    const double tol = 0.25;   // digitisation, not physics: ~1 px is 0.125 dB
    CHECK_THAT(*conducted_limit_at(pk, 1e6), WithinAbs(54.04, tol));
    CHECK_THAT(*conducted_limit_at(av, 1e6), WithinAbs(34.09, tol));
    CHECK_THAT(*conducted_limit_at(av, 200e3), WithinAbs(50.05, tol));
    CHECK_THAT(*conducted_limit_at(pk, 6e6), WithinAbs(53.1, tol));
    CHECK_THAT(*conducted_limit_at(av, 6e6), WithinAbs(33.1, tol));
    CHECK_THAT(*conducted_limit_at(pk, 27e6), WithinAbs(44.1, tol));
    CHECK_THAT(*conducted_limit_at(av, 27e6), WithinAbs(24.1, tol));

    // The band EDGES have to agree too, or the levels agreeing is luck. The
    // lab's MW line runs 0.5263-1.7830 MHz against a nominal 0.53-1.8: the
    // instrument drew it to the pixel it could, so the test is that our band
    // contains the drawn extent's interior and excludes what lies outside.
    CHECK(conducted_limit_at(pk, 0.54e6).has_value());
    CHECK(conducted_limit_at(pk, 1.75e6).has_value());
    CHECK_FALSE(conducted_limit_at(pk, 0.45e6).has_value());
    CHECK_FALSE(conducted_limit_at(pk, 2.0e6).has_value());
}

TEST_CASE("CISPR 25 conducted: per-band class steps, and gaps that stay gaps",
          "[emc][conducted][cispr25]") {
    const ConductedLimit c5 = cispr25_conducted(5, "quasi-peak");
    // Class 5 QP baseline, Table 4: LW 57, MW 41, SW 40, CB 31 dBuV
    CHECK_THAT(*conducted_limit_at(c5, 200e3), WithinAbs(57.0, 1e-9));
    CHECK_THAT(*conducted_limit_at(c5, 1e6), WithinAbs(41.0, 1e-9));
    CHECK_THAT(*conducted_limit_at(c5, 6e6), WithinAbs(40.0, 1e-9));
    CHECK_THAT(*conducted_limit_at(c5, 27e6), WithinAbs(31.0, 1e-9));
    // Between the protected bands there is NO limit — information, not zero
    CHECK_FALSE(conducted_limit_at(c5, 400e3).has_value());   // LW..MW gap
    CHECK_FALSE(conducted_limit_at(c5, 3e6).has_value());     // MW..SW gap
    CHECK_FALSE(conducted_limit_at(c5, 15e6).has_value());    // SW..CB gap

    // The class step is per band: 10 dB in LW, 8 in MW, 6 from SW up. Class 3
    // is NOT class 5 + 20 dB everywhere — that would be 8 dB too permissive in
    // SW/CB, and permissive is the wrong direction to be wrong in.
    const ConductedLimit c3 = cispr25_conducted(3, "quasi-peak");
    CHECK_THAT(*conducted_limit_at(c3, 200e3), WithinAbs(57.0 + 20.0, 1e-9));
    CHECK_THAT(*conducted_limit_at(c3, 1e6), WithinAbs(41.0 + 16.0, 1e-9));
    CHECK_THAT(*conducted_limit_at(c3, 6e6), WithinAbs(40.0 + 12.0, 1e-9));

    // detector offsets: peak = QP + 13, average = QP - 7
    CHECK_THAT(*conducted_limit_at(cispr25_conducted(5, "peak"), 1e6),
               WithinAbs(41.0 + 13.0, 1e-9));
    CHECK_THAT(*conducted_limit_at(cispr25_conducted(5, "average"), 1e6),
               WithinAbs(41.0 - 7.0, 1e-9));

    CHECK_THROWS_AS(cispr25_conducted(0, "quasi-peak"), std::invalid_argument);
    CHECK_THROWS_AS(cispr25_conducted(3, "rms"), std::invalid_argument);
    // and every generated line is reachable by id from the registry
    CHECK(conducted_limit_by_id("cispr25c3-qp").detector == "quasi-peak");
    CHECK(conducted_limit_by_id("cispr25c5-av").detector == "average");
    CHECK(limit_expects_automotive_lisn("cispr25c3-qp"));
    CHECK_FALSE(limit_expects_automotive_lisn("cispr32b-qp"));
}

TEST_CASE("a design frequency in a CISPR 25 gap gets no requirement, and says so",
          "[emc][conducted][cispr25]") {
    // 400 kHz switching: the fundamental lands between LW (ends 300 kHz) and
    // MW (starts 530 kHz), where no limit exists. Quoting an attenuation there
    // would size a stage against a limit that is not measured.
    const Trapezoid t{10.0, 400e3, 0.4, 20e-9};
    const ConductedVerdict gap = conducted_verdict(
        conducted_estimate(t, 10e-6, 10e-9, 0.01, 14.0, 100e-12),
        t.f_sw_hz, "cispr25c3-qp");
    CHECK_FALSE(gap.design_f_covered);
    CHECK(gap.required_cm_db == 0.0);
    CHECK(gap.required_dm_db == 0.0);
    // the band-limited verdict still ranks the points that ARE measured
    CHECK(!gap.points.empty());
    for (const auto& p : gap.points)
        CHECK(conducted_limit_at(conducted_limit_by_id("cispr25c3-qp"), p.f_hz));

    // 600 kHz lands inside MW, and there the requirement is quoted
    const Trapezoid t2{10.0, 600e3, 0.4, 20e-9};
    const ConductedVerdict in = conducted_verdict(
        conducted_estimate(t2, 10e-6, 10e-9, 0.01, 14.0, 100e-12),
        t2.f_sw_hz, "cispr25c3-qp");
    CHECK(in.design_f_covered);
    CHECK(in.design_f_used_hz == 600e3);
    CHECK(in.required_cm_db != 0.0);
}

TEST_CASE("the input branch is a NETWORK: bulk sets C, ceramics set L",
          "[emc][conducted][branch]") {
    // A real converter input: 2 x 390 uF electrolytic (2 nH each, 60 mOhm) and
    // 2 x 1 uF ceramic (1.2 nH each, 5 mOhm). Collapsing that to one lumped
    // capacitor cannot reproduce the crossover between them.
    const std::vector<InputCapBranch> real = {
        {390e-6, 2e-9, 0.06}, {390e-6, 2e-9, 0.06},
        {1e-6, 1.2e-9, 0.005}, {1e-6, 1.2e-9, 0.005},
    };
    // At 10 kHz the bulk owns it, but NOT as pure capacitance: 1/(wC) is
    // 40.8 mOhm per can against a 60 mOhm ESR, so the ESR is already the
    // larger term. Two cans in parallel with the (still capacitive, 15.9 ohm)
    // ceramics give |Z| = 36.2 mOhm — not the 20.4 mOhm a 782 uF ideal
    // capacitor would suggest. This is exactly the error a lumped C makes.
    CHECK_THAT(input_branch_z(real, 10e3), WithinRel(0.036153, 1e-4));
    // At 10 MHz the cans are inductive (X = +125.7 mOhm) while each ceramic is
    // only partly so (75.4 - 15.9 = 59.5 mOhm), and the parallel network lands
    // at 21.2 mOhm.
    CHECK_THAT(input_branch_z(real, 10e6), WithinRel(0.021179, 1e-4));
    // the parallel network is never above the best single branch
    for (double f : {1e5, 1e6, 5e6, 2e7}) {
        double best = 1e30;
        for (const auto& b : real)
            best = std::min(best, input_branch_z({b}, f));
        CHECK(input_branch_z(real, f) <= best * 1.000001);
    }
    // and a branch list reproduces the scalar form exactly when it is one cap
    const Trapezoid t{10.0, 500e3, 0.4, 20e-9};
    const ConductedEstimate one = conducted_estimate(t, 10e-6, 10e-9, 0.01, 48.0, 50e-12);
    const ConductedEstimate lst = conducted_estimate(
        t, {{10e-6, 10e-9, 0.01}}, 48.0, 50e-12);
    REQUIRE(one.f_hz.size() == lst.f_hz.size());
    for (size_t i = 0; i < one.f_hz.size(); ++i)
        CHECK_THAT(lst.dm_dbuv[i], WithinAbs(one.dm_dbuv[i], 1e-9));

    // the DM curve MOVES when the real branch replaces the lumped guess —
    // which is the whole argument for reading the board's parts
    const ConductedEstimate derived = conducted_estimate(t, real, 48.0, 50e-12);
    bool differs = false;
    for (size_t i = 0; i < one.dm_dbuv.size(); ++i)
        if (std::abs(derived.dm_dbuv[i] - one.dm_dbuv[i]) > 3.0) differs = true;
    CHECK(differs);

    CHECK_THROWS_AS(input_branch_z({}, 1e6), std::invalid_argument);
    CHECK_THROWS_AS(input_branch_z({{0.0, 1e-9, 0.01}}, 1e6), std::invalid_argument);
    CHECK_THROWS_AS(conducted_estimate(t, std::vector<InputCapBranch>{}, 48.0, 50e-12),
                    std::invalid_argument);
}
