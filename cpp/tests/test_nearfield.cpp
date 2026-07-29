// The quasi-static near-field core. This regime supplies unusually sharp
// invariants — an exact duality identity that holds at EVERY distance, and
// closed-form values at k*r = 1 — so almost nothing here is a captured number.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/NearField.hpp>
#include <faraday/Shielding.hpp>

using namespace faraday::nf;
namespace shield = faraday::shield;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

TEST_CASE("eta0 is derived, not pasted", "[nf]") {
    // Deriving it as mu0*c is what makes the duality assert below exact by
    // construction rather than exact to however many digits someone typed.
    CHECK_THAT(ETA0, WithinRel(376.730313461771, 1e-12));
    CHECK_THAT(ETA0 * ETA0, WithinRel(141925.729081, 1e-9));
}

TEST_CASE("the wave impedances are reciprocal about eta0 at EVERY distance",
          "[nf]") {
    // Z_w,E(r) * Z_w,M(r) = eta0^2, exactly, for all r. This cannot hold by
    // accident: it fails loudly if either dipole expression is wrong, which is
    // exactly what makes it the right first test.
    for (double f : {1e6, 30e6, 100e6, 1e9})
        for (double r : {1e-3, 5e-3, 0.05, 0.5, 3.0, 30.0}) {
            INFO("f = " << f * 1e-6 << " MHz, r = " << r << " m");
            CHECK_THAT(wave_impedance_electric(f, r) * wave_impedance_magnetic(f, r),
                       WithinRel(ETA0 * ETA0, 1e-12));
        }
}

TEST_CASE("wave impedance has its exact closed-form values at k*r = 1", "[nf]") {
    // At k*r = 1 the algebra collapses to eta0/sqrt(2) and eta0*sqrt(2).
    const double f = 100e6;
    const double r = near_far_boundary(f);           // k*r = 1 by definition
    CHECK_THAT(kr(f, r), WithinRel(1.0, 1e-12));
    CHECK_THAT(wave_impedance_electric(f, r), WithinRel(ETA0 / std::sqrt(2.0), 1e-12));
    CHECK_THAT(wave_impedance_magnetic(f, r), WithinRel(ETA0 * std::sqrt(2.0), 1e-12));
    CHECK_THAT(wave_impedance_electric(f, r), WithinRel(266.39, 1e-4));
}

TEST_CASE("both source types cross 377 ohm at k*r = 1/sqrt(2)", "[nf]") {
    // A consequence of the duality: if the product is always eta0^2, the two
    // curves can only meet where each equals eta0, and they must do so at the
    // same distance.
    const double f = 300e6;
    const double r = wavelength(f) / (2 * PI_N * std::sqrt(2.0));
    CHECK_THAT(kr(f, r), WithinRel(1.0 / std::sqrt(2.0), 1e-12));
    CHECK_THAT(wave_impedance_electric(f, r), WithinRel(ETA0, 1e-9));
    CHECK_THAT(wave_impedance_magnetic(f, r), WithinRel(ETA0, 1e-9));
}

TEST_CASE("the near field is where a PCB lives, and the map must know it",
          "[nf]") {
    // lambda/2pi against frequency — the numbers that justify treating the
    // whole board as quasi-static across the entire EMC band.
    CHECK_THAT(near_far_boundary(1e6), WithinRel(47.7, 0.01));
    CHECK_THAT(near_far_boundary(30e6), WithinRel(1.59, 0.01));
    CHECK_THAT(near_far_boundary(100e6), WithinRel(0.477, 0.01));
    CHECK_THAT(near_far_boundary(1e9), WithinRel(0.0477, 0.01));

    // 5 mm at 30 MHz is DEEPLY quasi-static; 3 m at 30 MHz is not the same
    // regime at all, which is the whole reason this header exists.
    CHECK_THAT(kr(30e6, 5e-3), WithinAbs(0.0031, 1e-4));
    CHECK(kr(30e6, 3.0) > 1.0);

    // an electrically LARGE source pushes the boundary out, and the true onset
    // is the larger of the two criteria
    CHECK_THAT(far_field_onset(1e9, 0.0), WithinRel(near_far_boundary(1e9), 1e-12));
    CHECK(far_field_onset(1e9, 0.5) > near_far_boundary(1e9));
}

TEST_CASE("the near field decays as 1/r^3 — 18.06 dB per doubling", "[nf]") {
    // The single most consequential difference from far-field intuition, and
    // good news for a designer: doubling a keep-out buys 18 dB, not 6.
    const double m = magnetic_moment(1, 1.0, 1e-4);
    const double a = h_axial(m, 0.05), b = h_axial(m, 0.10);
    CHECK_THAT(20 * std::log10(a / b), WithinAbs(18.0618, 1e-3));
    CHECK_THAT(20 * std::log10(h_axial(m, 0.05) / h_axial(m, 0.5)),
               WithinAbs(60.0, 1e-3));                        // a decade

    // a WIRE is 1/r, not 1/r^3 — using the dipole law near a trace would
    // under-report badly, so the two laws are kept separate
    CHECK_THAT(20 * std::log10(h_wire(1.0, 0.05) / h_wire(1.0, 0.10)),
               WithinAbs(6.0206, 1e-3));
}

TEST_CASE("the dipole angular factor has its textbook axis and equator values",
          "[nf]") {
    const double m = magnetic_moment(1, 2.0, 5e-5);
    // on axis  H = m/(2 pi r^3);  equatorial  H = m/(4 pi r^3) — a factor of 2
    CHECK_THAT(h_axial(m, 0.02), WithinRel(m / (2 * PI_N * 8e-6), 1e-12));
    CHECK_THAT(h_equatorial(m, 0.02), WithinRel(m / (4 * PI_N * 8e-6), 1e-12));
    CHECK_THAT(h_axial(m, 0.02) / h_equatorial(m, 0.02), WithinRel(2.0, 1e-12));
}

TEST_CASE("the moment is linear in turns, current and area", "[nf]") {
    // m = N I A means the ranking metric is the current-area product. Publish
    // the area (certain) and let the current be a stated input; the whole map
    // then scales linearly and the uncertainty stays visible.
    const double base = magnetic_moment(1, 1.0, 1e-4);
    CHECK_THAT(magnetic_moment(2, 1.0, 1e-4), WithinRel(2 * base, 1e-12));
    CHECK_THAT(magnetic_moment(1, 2.0, 1e-4), WithinRel(2 * base, 1e-12));
    CHECK_THAT(magnetic_moment(1, 1.0, 2e-4), WithinRel(2 * base, 1e-12));
}

TEST_CASE("the dipole model refuses to answer where it is invalid", "[nf]") {
    // At 5 mm from a 100 mm^2 loop the point-dipole model is WRONG, not
    // marginal: the exact equivalent-circular-loop field is 37.2 A/m and the
    // first valid dipole point is about 28 mm. Per the no-fallbacks rule the
    // model must refuse rather than extrapolate.
    const double area = 100e-6;
    CHECK_THAT(effective_radius(area), WithinRel(std::sqrt(area / PI_N), 1e-12));
    CHECK_THAT(dipole_valid_from_m(area), WithinRel(0.0282, 0.01));   // ~28 mm

    CHECK_FALSE(dipole_valid(5e-3, area));
    CHECK_FALSE(dipole_valid(0.02, area));
    CHECK(dipole_valid(0.03, area));
    CHECK(dipole_valid(0.10, area));

    // a small loop is valid much closer in — the gate is a RATIO, not a
    // hardcoded radius
    CHECK(dipole_valid(5e-3, 1e-6));
}

TEST_CASE("victim pickup is the physics a layout tool actually owns", "[nf]") {
    // V = 2 pi f B A cos(theta). A and cos(theta) come exactly from geometry;
    // only B carries the current assumption.
    // Anchor: 14.1 uT at 10 mm from an unshielded drum, 4 mm^2 victim loop.
    const double v = induced_voltage(500e3, 14.1e-6, 4e-6, 1.0);
    CHECK_THAT(v * 1e6, WithinRel(177.0, 0.02));            // ~177 uV

    // linear in every term
    CHECK_THAT(induced_voltage(1e6, 14.1e-6, 4e-6, 1.0), WithinRel(2 * v, 1e-12));
    CHECK_THAT(induced_voltage(500e3, 28.2e-6, 4e-6, 1.0), WithinRel(2 * v, 1e-12));
    CHECK_THAT(induced_voltage(500e3, 14.1e-6, 8e-6, 1.0), WithinRel(2 * v, 1e-12));

    // and the cheapest countermeasure in the whole subject: orientation.
    // Edge-on nulls the coupling and no distance-based rule can see it.
    CHECK_THAT(induced_voltage(500e3, 14.1e-6, 4e-6, 0.0), WithinAbs(0.0, 1e-18));
    CHECK_THAT(induced_voltage(500e3, 14.1e-6, 4e-6, 0.5), WithinRel(v / 2, 1e-12));
}

TEST_CASE("the ringing frequency dominates, not the switching frequency",
          "[nf]") {
    // V is proportional to f, so a 130 MHz hot-loop ring beats the 500 kHz
    // fundamental by more than two orders of magnitude at equal flux density.
    const double at_fsw = induced_voltage(500e3, 1e-6, 4e-6, 1.0);
    const double at_ring = induced_voltage(130e6, 1e-6, 4e-6, 1.0);
    CHECK_THAT(at_ring / at_fsw, WithinRel(260.0, 1e-9));
    CHECK(20 * std::log10(at_ring / at_fsw) > 48.0);
}

TEST_CASE("broadside overlap capacitance matches the published pF per mm2",
          "[nf]") {
    // 0.19 pF per mm^2 at 0.2 mm on FR-4 (eps_r 4.3) — the dominant PCB
    // coupling mechanism and one a layout tool computes exactly.
    const double c = overlap_capacitance(1e-6, 0.2e-3, 4.3);
    CHECK_THAT(c * 1e12, WithinRel(0.19, 0.02));
    // halve the dielectric, double the capacitance
    CHECK_THAT(overlap_capacitance(1e-6, 0.1e-3, 4.3), WithinRel(2 * c, 1e-12));
}

TEST_CASE("capacitive coupling has a frequency-independent ceiling", "[nf]") {
    // The divider bound is the worst case and is always valid.
    // 0.05 pF onto a 5 pF node from a 12 V switch node -> ~119 mV.
    CHECK_THAT(capacitive_step(12.0, 0.05e-12, 5e-12) * 1e3, WithinRel(119.0, 0.02));
    // a heavier victim node dilutes it
    CHECK(capacitive_step(12.0, 0.05e-12, 50e-12) <
          capacitive_step(12.0, 0.05e-12, 5e-12));
    // and it can never exceed the switching swing itself
    CHECK(capacitive_step(12.0, 1e-12, 1e-15) <= 12.0);
}

TEST_CASE("displacement current is the switch-node mechanism", "[nf]") {
    // 12 V in 5 ns (2.4 V/ns) across 100 fF of stray capacitance -> 240 uA.
    CHECK_THAT(displacement_current(100e-15, 2.4e9) * 1e6, WithinRel(240.0, 1e-9));
}

TEST_CASE("the two victim ladders are kept apart", "[nf]") {
    // Peak-volts thresholds and DC-accuracy thresholds respond to different
    // things — a spike versus a rectified average — and cannot be ranked
    // against one another.
    const auto& csa = victim_by_id("csa");
    const auto& logic = victim_by_id("logic33");
    CHECK(csa.kind == VictimKind::DcAccuracy);
    CHECK(logic.kind == VictimKind::PeakVolts);
    CHECK(csa.threshold_v < logic.threshold_v);

    // every class states WHY its threshold is what it is
    for (const auto& v : victim_classes()) {
        INFO(v.id);
        CHECK(v.threshold_v > 0);
        CHECK(std::string(v.why).size() > 10);
    }
    CHECK_THROWS_WITH(victim_by_id("ttl"),
                      Catch::Matchers::ContainsSubstring("cannot be guessed"));
}

TEST_CASE("bad inputs are refused", "[nf]") {
    CHECK_THROWS_AS(wavelength(0), std::invalid_argument);
    CHECK_THROWS_AS(kr(1e6, 0), std::invalid_argument);
    CHECK_THROWS_AS(magnetic_moment(1, -1, 1e-4), std::invalid_argument);
    CHECK_THROWS_AS(magnetic_moment(0, 1, 1e-4), std::invalid_argument);
    CHECK_THROWS_AS(h_axial(1e-4, 0), std::invalid_argument);
    CHECK_THROWS_AS(effective_radius(0), std::invalid_argument);
    CHECK_THROWS_AS(overlap_capacitance(1e-6, 0, 4.3), std::invalid_argument);
    CHECK_THROWS_AS(induced_voltage(0, 1e-6, 1e-6, 1), std::invalid_argument);
}

TEST_CASE("dBuA/m conversion round-trips", "[nf]") {
    CHECK_THAT(to_dbua_m(1.0), WithinAbs(120.0, 1e-9));      // 1 A/m = 120 dBuA/m
    CHECK_THAT(to_dbua_m(1e-6), WithinAbs(0.0, 1e-9));
    CHECK_THAT(to_dbua_m(2.0) - to_dbua_m(1.0), WithinAbs(6.0206, 1e-3));
    CHECK(to_dbua_m(0.0) < -300.0);                          // floored, not inf
}

// ---------------------------------------------------------------------------
// Biot-Savart over the real loop
// ---------------------------------------------------------------------------

TEST_CASE("Biot-Savart converges to the point dipole far from the loop",
          "[nf]") {
    // THE test that matters: if the exact filamentary integral and the
    // point-dipole law are the same physics, they must agree once the field
    // point is far enough out. If they disagree, one of them is wrong, and
    // this catches it without any external reference.
    const double a = 0.01;                       // 10 mm square, 100 mm^2
    const std::vector<Vec3> sq = {
        {-a / 2, -a / 2, 0}, {a / 2, -a / 2, 0}, {a / 2, a / 2, 0}, {-a / 2, a / 2, 0}};
    const double I = 1.0;
    const double m = magnetic_moment(1.0, I, a * a);

    // on the loop axis, at increasing distance
    for (double r : {0.1, 0.2, 0.5, 1.0}) {
        const double exact = h_loop(sq, {0, 0, r}, I);
        const double dip = h_axial(m, r);
        INFO("axis, r = " << r * 1e3 << " mm: exact " << exact << " dipole " << dip);
        CHECK_THAT(exact, WithinRel(dip, 0.02));
    }
    // and in the equatorial plane
    for (double r : {0.2, 0.5, 1.0}) {
        const double exact = h_loop(sq, {r, 0, 0}, I);
        CHECK_THAT(exact, WithinRel(h_equatorial(m, r), 0.03));
    }
}

TEST_CASE("Biot-Savart is finite and 1/r-like close to a conductor", "[nf]") {
    // This is why it replaces the refusal: right beside the loop the field is
    // dominated by the nearest conductor and behaves as I/(2 pi d), not 1/r^3.
    // A dipole model would badly under-report exactly where a designer looks.
    const double a = 0.02;                       // 20 mm square
    const std::vector<Vec3> sq = {
        {0, 0, 0}, {a, 0, 0}, {a, a, 0}, {0, a, 0}};
    const double I = 10.0;

    // 1 mm outside the midpoint of one side: close to the infinite-wire value
    const double h = h_loop(sq, {a / 2, -0.001, 0}, I);
    CHECK(std::isfinite(h));
    CHECK_THAT(h, WithinRel(h_wire(I, 0.001), 0.25));

    // and the close-in slope is far shallower than 18 dB/doubling
    const double h1 = h_loop(sq, {a / 2, -0.001, 0}, I);
    const double h2 = h_loop(sq, {a / 2, -0.002, 0}, I);
    const double slope = 20 * std::log10(h1 / h2);
    INFO("close-in slope " << slope << " dB per doubling");
    CHECK(slope > 4.0);
    CHECK(slope < 12.0);      // between 1/r (6 dB) and 1/r^2 (12 dB)
}

TEST_CASE("Biot-Savart scales linearly with current and refuses bad input",
          "[nf]") {
    const std::vector<Vec3> sq = {
        {0, 0, 0}, {0.01, 0, 0}, {0.01, 0.01, 0}, {0, 0.01, 0}};
    const double h = h_loop(sq, {0.005, 0.005, 0.01}, 1.0);
    CHECK_THAT(h_loop(sq, {0.005, 0.005, 0.01}, 2.0), WithinRel(2 * h, 1e-12));
    CHECK_THAT(h_loop(sq, {0.005, 0.005, 0.01}, 0.0), WithinAbs(0.0, 1e-18));

    CHECK_THROWS_AS(h_loop({{0, 0, 0}, {1, 0, 0}}, {0, 1, 0}, 1.0),
                    std::invalid_argument);
    CHECK_THROWS_AS(h_loop(sq, {0, 0, 0}, 1.0), std::invalid_argument);
    CHECK_THROWS_AS(h_loop(sq, {0.005, 0.005, 0.01}, -1.0),
                    std::invalid_argument);
}

TEST_CASE("the loop field is orientation-aware, as the physics requires",
          "[nf]") {
    // On axis is the maximum; in the plane of the loop, well outside it, the
    // field is weaker for the same distance. A model that ignored this would
    // miss the cheapest countermeasure there is.
    const double a = 0.01;
    const std::vector<Vec3> sq = {
        {-a / 2, -a / 2, 0}, {a / 2, -a / 2, 0}, {a / 2, a / 2, 0}, {-a / 2, a / 2, 0}};
    const double r = 0.05;
    CHECK(h_loop(sq, {0, 0, r}, 1.0) > h_loop(sq, {r, 0, 0}, 1.0));
    // the ratio approaches the dipole's factor of 2
    CHECK_THAT(h_loop(sq, {0, 0, r}, 1.0) / h_loop(sq, {r, 0, 0}, 1.0),
               WithinRel(2.0, 0.1));
}

TEST_CASE("shield: a permeability grade scales absorption as sqrt(mu) exactly",
          "[shield][mur]") {
    // 300 kHz, tin-plated steel: the WALL binds (seam SE ~100 dB there), so
    // the grade decides. delta ~ 1/sqrt(mu) -> absorption ~ sqrt(mu).
    shield::Can base;
    base.material = "tinsteel";
    base.wall_mm = 0.2;
    base.seam_pitch_mm = 5.0;
    const double f = 300e3;
    const auto v0 = shield::evaluate(base, f, shield::FieldKind::MagneticNear);
    REQUIRE(v0.limited_by == "wall");

    shield::Can graded = base;
    graded.mu_r = 400.0;             // 4x the material's 100
    const auto v1 = shield::evaluate(graded, f, shield::FieldKind::MagneticNear);
    CHECK_THAT(v1.absorption_db / v0.absorption_db,
               Catch::Matchers::WithinRel(2.0, 1e-12));   // sqrt(4)
    CHECK(v1.se_db > v0.se_db);
    // user-supplied at this frequency: no extrapolation flag
    CHECK(!v1.permeability_extrapolated);

    // 0 means "the material's own" and changes nothing
    shield::Can same = base;
    same.mu_r = 0;
    CHECK(shield::evaluate(same, f, shield::FieldKind::MagneticNear).se_db ==
          v0.se_db);

    // sub-unity relative permeability is refused, not clamped
    shield::Can bad = base;
    bad.mu_r = 0.5;
    CHECK_THROWS_WITH(shield::evaluate(bad, f, shield::FieldKind::MagneticNear),
                      Catch::Matchers::ContainsSubstring(">= 1"));
}
