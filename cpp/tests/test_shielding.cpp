// Shielding effectiveness. The numbers on the right are either published
// constants, hand-computable, or the two regime limits that make the whole
// point of the model.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/Shielding.hpp>

using namespace faraday::shield;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

TEST_CASE("skin depth reproduces the published copper ladder", "[shield]") {
    // 66.1 / 12.1 / 6.61 / 2.09 um at 1 / 30 / 100 / 1000 MHz. Getting all
    // four right pins the constant and the square-root law together.
    const Material& cu = material_by_id("copper");
    CHECK_THAT(skin_depth_um(1e6, cu), WithinRel(66.1, 0.01));
    CHECK_THAT(skin_depth_um(3e7, cu), WithinRel(12.07, 0.01));
    CHECK_THAT(skin_depth_um(1e8, cu), WithinRel(6.61, 0.01));
    CHECK_THAT(skin_depth_um(1e9, cu), WithinRel(2.09, 0.01));
    // and it falls as 1/sqrt(f): four times the frequency, half the depth
    CHECK_THAT(skin_depth_um(4e6, cu), WithinRel(skin_depth_um(1e6, cu) / 2, 1e-9));
}

TEST_CASE("permeability is worth ~49 dB at 500 kHz and nothing at 130 MHz",
          "[shield]") {
    // THE result that makes a can's material spec meaningful or meaningless
    // depending on the frequency you care about.
    const Material& steel = material_by_id("tinsteel");
    const Material& brass = material_by_id("brass");

    const double s_lf = absorption_db(0.2, 5e5, steel);
    const double b_lf = absorption_db(0.2, 5e5, brass);
    CHECK_THAT(s_lf, WithinRel(59.0, 0.03));
    CHECK_THAT(b_lf, WithinRel(9.8, 0.05));
    CHECK(s_lf - b_lf > 45.0);          // material choice is decisive here

    // at 130 MHz both are absurdly opaque, so the difference cannot matter
    CHECK(absorption_db(0.2, 1.3e8, steel) > 500.0);
    CHECK(absorption_db(0.2, 1.3e8, brass) > 100.0);
}

TEST_CASE("absorption is linear in thickness and rises as sqrt(f)", "[shield]") {
    const Material& m = material_by_id("tinsteel");
    CHECK_THAT(absorption_db(0.4, 1e6, m),
               WithinRel(2 * absorption_db(0.2, 1e6, m), 1e-9));
    CHECK_THAT(absorption_db(0.2, 4e6, m),
               WithinRel(2 * absorption_db(0.2, 1e6, m), 1e-9));
    // one skin depth is 8.686 dB, by definition
    const double t_um = skin_depth_um(1e6, m);
    CHECK_THAT(absorption_db(t_um / 1000.0, 1e6, m), WithinAbs(8.686, 1e-6));
}

TEST_CASE("the seam sets the answer at HF, and only the seam", "[shield]") {
    // SE = 20 log10(lambda / 2L). At 130 MHz lambda is 2308 mm.
    CHECK_THAT(aperture_se_db(1.3e8, 5.0), WithinRel(47.3, 0.02));
    CHECK_THAT(aperture_se_db(1.3e8, 10.0), WithinRel(41.3, 0.02));
    CHECK_THAT(aperture_se_db(1.3e8, 25.0), WithinRel(33.3, 0.02));
    // halving the contact pitch buys exactly 6 dB
    CHECK_THAT(aperture_se_db(1.3e8, 5.0) - aperture_se_db(1.3e8, 10.0),
               WithinAbs(6.0206, 1e-6));
    // and at 500 kHz the seam is irrelevant — 96 dB for a 5 mm pitch
    CHECK(aperture_se_db(5e5, 5.0) > 90.0);
}

TEST_CASE("a seam at half a wavelength is refused, not extrapolated",
          "[shield]") {
    // Past lambda/2 the aperture resonates and the shield can be worse than
    // nothing. Returning a positive dB there would be actively misleading.
    const double f = seam_resonance_hz(25.0);
    CHECK_THAT(f * 1e-9, WithinRel(6.0, 0.01));     // 25 mm -> 6 GHz
    CHECK_THROWS_WITH(aperture_se_db(f, 25.0),
                      Catch::Matchers::ContainsSubstring("antenna, not an aperture"));
    CHECK_NOTHROW(aperture_se_db(f * 0.4, 25.0));
}

TEST_CASE("the model names which regime is binding", "[shield]") {
    Can can;                       // 0.2 mm tin-plated steel, 5 mm pitch
    // At 130 MHz the wall is opaque, so the seam must be the limit.
    const Verdict hf = evaluate(can, 1.3e8, FieldKind::MagneticNear);
    CHECK(hf.limited_by == "seam");
    CHECK_THAT(hf.se_db, WithinRel(47.3, 0.02));
    CHECK(hf.walls_per_skin > 50.0);

    // At 500 kHz the wall is only a few skin depths, so it binds instead.
    const Verdict lf = evaluate(can, 5e5, FieldKind::MagneticNear);
    CHECK(lf.limited_by == "wall");
    CHECK_THAT(lf.se_db, WithinRel(59.0, 0.03));
}

TEST_CASE("a thin wall against a LF magnetic field is called out", "[shield]") {
    // The specific claim that a can 'does nothing' for low-frequency magnetic
    // fields, made precise: below one skin depth of wall the eddy currents
    // barely develop.
    Can thin;
    thin.material = "brass";
    thin.wall_mm = 0.15;
    const Verdict v = evaluate(thin, 1e5, FieldKind::MagneticNear);
    CHECK(v.walls_per_skin < 1.0);
    CHECK(v.se_db < 10.0);
    CHECK_THAT(v.caveat, Catch::Matchers::ContainsSubstring("skin depth"));
    CHECK_THAT(v.caveat, Catch::Matchers::ContainsSubstring("single-digit"));
}

TEST_CASE("an electric near field is a different problem entirely", "[shield]") {
    // Reflection is essentially total, so the metal never limits and the bond
    // does. A model that returned the same SE for both fields would be wrong
    // by ~100 dB in one direction.
    Can can;
    const Verdict e = evaluate(can, 1e6, FieldKind::ElectricNear);
    const Verdict h = evaluate(can, 1e6, FieldKind::MagneticNear);
    CHECK(e.limited_by == "seam");
    CHECK(e.se_db > h.se_db);
    CHECK_THAT(e.caveat, Catch::Matchers::ContainsSubstring("ungrounded"));
}

TEST_CASE("five-sided is stated as the upper bound it is", "[shield]") {
    Can five, six;
    six.five_sided = false;
    CHECK_THAT(evaluate(five, 1.3e8, FieldKind::MagneticNear).caveat,
               Catch::Matchers::ContainsSubstring("routes around"));
    CHECK(evaluate(six, 1.3e8, FieldKind::MagneticNear).caveat.find("routes around")
          == std::string::npos);
    // the number itself is unchanged — it is the claim that is qualified
    CHECK_THAT(evaluate(five, 1.3e8, FieldKind::MagneticNear).se_db,
               WithinRel(evaluate(six, 1.3e8, FieldKind::MagneticNear).se_db, 1e-12));
}

TEST_CASE("mu-metal wins at LF, and past its roll-off it is flagged not trusted",
          "[shield]") {
    // Mu-metal is the right answer for a low-frequency magnetic field: at
    // 10 kHz its permeability buys far more than copper's conductivity.
    const Material& mu = material_by_id("mumetal");
    const Material& cu = material_by_id("copper");
    CHECK(absorption_db(0.2, 1e4, mu) > absorption_db(0.2, 1e4, cu));

    // But mu_r = 20000 is a DC/audio figure. Held constant it would make
    // mu-metal look like the best HF shield in the list, which is backwards —
    // high-mu alloys lose permeability long before conductors lose
    // conductivity. The model does not silently extrapolate it.
    Can can;
    can.material = "mumetal";
    const Verdict lf = evaluate(can, 5e4, FieldKind::MagneticNear);
    const Verdict hf = evaluate(can, 1.3e8, FieldKind::MagneticNear);
    CHECK_FALSE(lf.permeability_extrapolated);
    CHECK(hf.permeability_extrapolated);
    CHECK_THAT(hf.caveat, Catch::Matchers::ContainsSubstring("no longer holds"));

    // and the ordinary can metals are not flagged in their own working range
    Can steel;
    CHECK_FALSE(evaluate(steel, 5e5, FieldKind::MagneticNear)
                    .permeability_extrapolated);
    Can brass;
    brass.material = "brass";
    CHECK_FALSE(evaluate(brass, 1.3e8, FieldKind::MagneticNear)
                    .permeability_extrapolated);
}

TEST_CASE("bad inputs are refused", "[shield]") {
    CHECK_THROWS_AS(material_by_id("unobtainium"), std::invalid_argument);
    CHECK_THROWS_AS(skin_depth_um(0, material_by_id("copper")), std::invalid_argument);
    CHECK_THROWS_AS(absorption_db(0, 1e6, material_by_id("copper")),
                    std::invalid_argument);
    CHECK_THROWS_AS(aperture_se_db(1e6, 0), std::invalid_argument);
}
