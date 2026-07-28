// The component near-field map on a board. What is pinned here is that the
// map DECOMPOSES into the NearField.hpp physics already validated against
// exact identities, and that it refuses where the model is invalid.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/Import.hpp>
#include <faraday/NearFieldMap.hpp>

#include <fstream>
#include <sstream>

using namespace faraday;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

namespace {

// A converter fragment: a switch net with the footprints the switch-node rule
// needs, plus two victims at different distances.
BoardIR converter() {
    BoardIR b;
    b.copper_names = {"F.Cu", "B.Cu"};
    b.stackup.layers = {
        {LayerKind::Copper, "F.Cu", 0.035, std::nullopt, "signal"},
        {LayerKind::Dielectric, "core", 0.2, 4.3, ""},
        {LayerKind::Copper, "B.Cu", 0.035, std::nullopt, "signal"},
    };
    b.stackup.source = "test";
    b.nets = {{0, ""}, {1, "/DCDC/SW_NODE"}, {2, "GND"}, {3, "VIN"},
              {4, "/ADC/ISENSE"}, {5, "/MCU/COMP"}};
    b.bbox_x1 = 0; b.bbox_y1 = 0; b.bbox_x2 = 60; b.bbox_y2 = 40;
    b.bbox_from_outline = true;

    b.components.push_back({"Q1", "lib:dfn", "N-FET", 10.0, 10.0, 0.0});
    b.components.push_back({"Q2", "lib:dfn", "N-FET", 14.0, 10.0, 0.0});
    b.components.push_back({"L1", "lib:ind", "10u", 18.0, 10.0, 0.0});
    b.components.push_back({"C1", "lib:0805", "10u", 10.0, 16.0, 0.0});
    b.components.push_back({"U2", "lib:soic", "INA190", 40.0, 10.0, 0.0});
    b.components.push_back({"U3", "lib:soic", "MCU", 12.0, 13.0, 0.0});

    // the commutation loop members
    b.pads.push_back({"Q1", 1, 10.0, 10.0, 1.0, 1.0, false, 0});
    b.pads.push_back({"Q1", 3, 10.0, 12.0, 1.0, 1.0, false, 0});
    b.pads.push_back({"Q2", 1, 14.0, 10.0, 1.0, 1.0, false, 0});
    b.pads.push_back({"Q2", 2, 14.0, 12.0, 1.0, 1.0, false, 0});
    b.pads.push_back({"L1", 1, 18.0, 10.0, 1.0, 1.0, false, 0});
    b.pads.push_back({"C1", 3, 10.0, 16.0, 1.0, 1.0, false, 0});
    b.pads.push_back({"C1", 2, 12.0, 16.0, 1.0, 1.0, false, 0});
    // victims: one far away, one right next to the switch
    b.pads.push_back({"U2", 4, 40.0, 10.0, 0.5, 0.5, false, 0});
    b.pads.push_back({"U3", 5, 12.0, 13.0, 0.5, 0.5, false, 0});

    b.segments.push_back({1, 0, 10.0, 10.0, 18.0, 10.0, 0.8});
    b.segments.push_back({3, 0, 10.0, 12.0, 10.0, 16.0, 0.8});
    b.zones.push_back({2, 1, {{0, 0}, {60, 0}, {60, 40}, {0, 40}}});
    return b;
}

nfmap::MapResult run(const BoardIR& b, nfmap::MapParams p = {}) {
    Screener s(b);
    return nfmap::compute(b, s, p);
}

}  // namespace

TEST_CASE("net names are classified into victim classes conservatively",
          "[nfmap]") {
    CHECK(nfmap::victim_class_for("/ADC/ISENSE") == "csa");
    CHECK(nfmap::victim_class_for("XTAL1") == "xtal");
    CHECK(nfmap::victim_class_for("/MCU/COMP") == "comp");
    CHECK(nfmap::victim_class_for("VREF") == "adc12");
    // an unrecognised net is NOT a victim: inventing a threshold for a net we
    // do not understand would produce confident nonsense
    CHECK(nfmap::victim_class_for("GND").empty());
    CHECK(nfmap::victim_class_for("Net-(R12-Pad1)").empty());
    CHECK(nfmap::victim_class_for("").empty());
}

TEST_CASE("the map reduces the commutation loop to the dipole it is",
          "[nfmap]") {
    const BoardIR b = converter();
    nfmap::MapParams p;
    p.ring_current_a = 10.0;
    const nfmap::MapResult r = run(b, p);

    REQUIRE_FALSE(r.aggressors.empty());
    const auto& a = r.aggressors[0];
    CHECK(a.area_mm2 > 0);
    // m = N I A with N = 1, exactly as NearField.hpp defines it
    CHECK_THAT(a.moment_am2,
               WithinRel(nf::magnetic_moment(1.0, 10.0, a.area_mm2 * 1e-6), 1e-12));
    // and the validity radius is 5 * sqrt(A/pi)
    CHECK_THAT(a.valid_from_mm,
               WithinRel(5.0 * std::sqrt(a.area_mm2 / nf::PI_N), 1e-9));
    CHECK(a.valid_from_mm > a.a_eff_mm);
}

TEST_CASE("current scales the whole map linearly, and is an assumption",
          "[nfmap]") {
    nfmap::MapParams p1, p2;
    p1.ring_current_a = 1.0;
    p2.ring_current_a = 2.0;
    const auto r1 = run(converter(), p1), r2 = run(converter(), p2);
    CHECK_THAT(r2.aggressors[0].moment_am2,
               WithinRel(2 * r1.aggressors[0].moment_am2, 1e-12));
    // every reported victim field doubles with it
    REQUIRE(r1.victims.size() == r2.victims.size());
    for (size_t i = 0; i < r1.victims.size(); ++i)
        CHECK_THAT(r2.victims[i].h_a_per_m,
                   WithinRel(2 * r1.victims[i].h_a_per_m, 1e-9));
}

TEST_CASE("distance is the dominant lever, at 18 dB per doubling", "[nfmap]") {
    const nfmap::MapResult r = run(converter());
    // U2 is far from the switch, U3 is right beside it
    const nfmap::VictimHit* near = nullptr;
    const nfmap::VictimHit* far = nullptr;
    for (const auto& v : r.victims) {
        if (v.component == "U3") near = &v;
        if (v.component == "U2") far = &v;
    }
    REQUIRE(far);
    if (near && far) {
        CHECK(near->distance_mm < far->distance_mm);
        CHECK(near->h_a_per_m > far->h_a_per_m);
        // The exact integral is steeper than 1/r but shallower than 1/r^3 at
        // these distances, which is the whole reason it replaces the dipole:
        // close to a real loop the field is NOT a point dipole.
        const double slope = std::log(near->h_a_per_m / far->h_a_per_m) /
                             std::log(far->distance_mm / near->distance_mm);
        INFO("effective exponent " << slope);
        CHECK(slope > 0.8);
        CHECK(slope < 3.2);
    }
}

TEST_CASE("close-in victims get a real number, from the exact integral",
          "[nfmap]") {
    // The point-dipole law would have to refuse here — a 267 mm^2 loop is
    // invalid within 46 mm, which on a 100 mm board excludes every part beside
    // the switcher, i.e. exactly the ones worth asking about. Biot-Savart over
    // the loop polygon has no such restriction, so every victim is answered and
    // the dipole flag becomes context rather than a gate.
    const nfmap::MapResult r = run(converter());
    REQUIRE_FALSE(r.victims.empty());
    for (const auto& v : r.victims) {
        INFO(v.component << " at " << v.distance_mm << " mm");
        CHECK(v.h_a_per_m > 0.0);
        CHECK(std::isfinite(v.h_a_per_m));
        CHECK(v.induced_v > 0.0);
        // the flag still reports honestly whether the point model would hold
        CHECK(v.dipole_valid == (v.distance_mm >= r.aggressors[0].valid_from_mm));
    }
}

TEST_CASE("probe height is mandatory and materially changes the answer",
          "[nfmap]") {
    // A map with no stated height is meaningless — 60 dB per decade.
    nfmap::MapParams bad;
    bad.probe_height_mm = 0;
    Screener s(converter());
    const BoardIR b = converter();
    CHECK_THROWS_WITH(nfmap::compute(b, s, bad),
                      Catch::Matchers::ContainsSubstring("meaningless"));

    nfmap::MapParams lo, hi;
    lo.probe_height_mm = 1.0;
    hi.probe_height_mm = 20.0;
    const auto rl = run(converter(), lo), rh = run(converter(), hi);
    CHECK(rl.probe_height_mm == 1.0);
    CHECK(rh.probe_height_mm == 20.0);
    for (const auto& a : rl.victims)
        for (const auto& b2 : rh.victims)
            if (a.component == b2.component && a.net == b2.net)
                CHECK(a.h_a_per_m > b2.h_a_per_m);
}

TEST_CASE("the ringing frequency is what drives the coupling", "[nfmap]") {
    nfmap::MapParams slow, fast;
    slow.ring_hz = 65e6;
    fast.ring_hz = 130e6;
    const auto rs = run(converter(), slow), rf = run(converter(), fast);
    for (size_t i = 0; i < rs.victims.size(); ++i)
    {
            // V = 2 pi f B A: linear in frequency, field unchanged
            CHECK_THAT(rf.victims[i].h_a_per_m,
                       WithinRel(rs.victims[i].h_a_per_m, 1e-12));
            CHECK_THAT(rf.victims[i].induced_v,
                       WithinRel(2 * rs.victims[i].induced_v, 1e-9));
        }
}

TEST_CASE("the map reports the regime context it must be read against",
          "[nfmap]") {
    nfmap::MapParams p;
    p.ring_hz = 130e6;
    const nfmap::MapResult r = run(converter(), p);
    // lambda/2pi at 130 MHz is ~367 mm, so the whole board is deep in the near
    // field — which is exactly why this map is not a radiation map
    CHECK_THAT(r.lambda_over_2pi_mm, WithinRel(367.0, 0.02));
    CHECK(r.lambda_over_2pi_mm > 60.0);   // larger than the board itself
    CHECK(r.ring_hz == 130e6);
}

TEST_CASE("each victim is judged against its own class threshold", "[nfmap]") {
    const nfmap::MapResult r = run(converter());
    for (const auto& v : r.victims) {
        const auto& vc = nf::victim_by_id(v.victim_class);
        CHECK_THAT(v.threshold_v, WithinRel(vc.threshold_v, 1e-12));
        CHECK_THAT(v.ratio, WithinRel(v.induced_v / v.threshold_v, 1e-9));
    }
    // a current-sense amp is judged far more harshly than a logic input
    CHECK(nf::victim_by_id("csa").threshold_v <
          nf::victim_by_id("logic33").threshold_v);
}

TEST_CASE("a board with no switching aggressor is refused, not faked",
          "[nfmap]") {
    BoardIR b = converter();
    b.nets = {{0, ""}, {1, "SIG"}, {2, "GND"}, {3, "VIN"}, {4, "/ADC/ISENSE"},
              {5, "/MCU/COMP"}};
    b.components.clear();
    b.pads.clear();
    Screener s(b);
    CHECK_THROWS_WITH(nfmap::compute(b, s, nfmap::MapParams{}),
                      Catch::Matchers::ContainsSubstring("no commutation loop"));
}

TEST_CASE("a real converter board maps without inventing anything",
          "[nfmap][real]") {
    std::ifstream in(std::string(FARADAY_FIXTURE_DIR) + "/real/mppt-2420-hc.kicad_pcb");
    if (!in) { WARN("mppt fixture missing"); return; }
    std::stringstream ss;
    ss << in.rdbuf();
    BoardIR b = import_board(ss.str(), builtin_stackup("default-4layer"));
    Screener s(b);
    const nfmap::MapResult r = nfmap::compute(b, s, nfmap::MapParams{});

    CHECK(r.aggressors.size() >= 1);
    INFO("aggressors " << r.aggressors.size() << ", victims " << r.victims.size()
                       << ", too close " << r.too_close_count);
    for (const auto& a : r.aggressors) {
        CHECK(a.area_mm2 > 0);
        CHECK(a.moment_am2 > 0);
        CHECK(a.valid_from_mm > 0);
    }
    for (const auto& v : r.victims) {
        CHECK(std::isfinite(v.h_a_per_m));
        CHECK(v.h_a_per_m >= 0);
        CHECK(v.distance_mm > 0);
    }
}

TEST_CASE("a can attenuates only when it separates aggressor from victim",
          "[nfmap][shield]") {
    // Both inside or both outside, it changes nothing — which is what makes
    // drawing a rectangle mean something rather than acting as a global knob.
    const BoardIR b = converter();
    Screener s(b);

    const nfmap::MapResult bare = nfmap::compute(b, s, nfmap::MapParams{});
    const nfmap::VictimHit* far0 = nullptr;
    for (const auto& v : bare.victims) if (v.component == "U2") far0 = &v;
    REQUIRE(far0);

    // can around the switching cluster (Q1/Q2/L1 near x 8..20, y 8..17):
    // U2 at (40,10) is outside -> attenuated; U3 at (12,13) is inside with the
    // aggressor -> untouched
    nfmap::MapParams p;
    shield::Rect can;
    can.x1 = 6; can.y1 = 6; can.x2 = 22; can.y2 = 18;
    can.se_db = 20.0;
    p.shields.push_back(can);
    const nfmap::MapResult sh = nfmap::compute(b, s, p);

    const nfmap::VictimHit* far1 = nullptr;
    const nfmap::VictimHit* near1 = nullptr;
    for (const auto& v : sh.victims) {
        if (v.component == "U2") far1 = &v;
        if (v.component == "U3") near1 = &v;
    }
    REQUIRE(far1);
    CHECK_THAT(far1->shield_db, WithinAbs(20.0, 1e-9));
    CHECK_THAT(20 * std::log10(far0->h_a_per_m / far1->h_a_per_m),
               WithinAbs(20.0, 1e-6));
    CHECK(sh.shielded_victims >= 1);
    if (near1) {
        CHECK(near1->shield_db == 0.0);   // same side as the aggressor
    }

    // a can somewhere irrelevant changes nothing at all
    nfmap::MapParams q;
    shield::Rect off;
    off.x1 = 50; off.y1 = 30; off.x2 = 58; off.y2 = 38;
    off.se_db = 40.0;
    q.shields.push_back(off);
    const nfmap::MapResult un = nfmap::compute(b, s, q);
    CHECK(un.shielded_victims == 0);
    for (size_t i = 0; i < un.victims.size(); ++i)
        CHECK_THAT(un.victims[i].h_a_per_m,
                   WithinRel(bare.victims[i].h_a_per_m, 1e-12));
}
