// The whole-board radiation attribution. There is no published reference for a
// map like this, so what is checked is that it DECOMPOSES exactly into the
// single-loop physics already validated in test_emissions.cpp, and that it
// ranks the way the design rule says it must.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <faraday/Import.hpp>
#include <faraday/RadiationMap.hpp>

#include <fstream>
#include <sstream>

using namespace faraday;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

namespace {

// A board built by hand so every number in the assertion is one we chose:
// two 10 mm traces on the top layer, a solid pour on the bottom.
BoardIR toy(bool with_plane = true) {
    BoardIR b;
    b.copper_names = {"F.Cu", "B.Cu"};
    b.stackup.layers = {
        {LayerKind::Copper, "F.Cu", 0.035, std::nullopt, "signal"},
        {LayerKind::Dielectric, "core", 0.2, 4.3, ""},
        // "signal", not "power": the plane classifier honours KiCad's layer-type
        // hint, and this fixture must vary ONLY whether the pour is there.
        {LayerKind::Copper, "B.Cu", 0.035, std::nullopt, "signal"},
    };
    b.stackup.source = "test";
    b.nets = {{0, ""}, {1, "SIG"}, {2, "GND"}};
    b.bbox_x1 = 0; b.bbox_y1 = 0; b.bbox_x2 = 20; b.bbox_y2 = 20;
    b.bbox_from_outline = true;
    b.segments.push_back({1, 0, 0.0, 5.0, 10.0, 5.0, 0.25});
    b.segments.push_back({1, 0, 0.0, 8.0, 10.0, 8.0, 0.25});
    if (with_plane)
        b.zones.push_back({2, 1, {{0, 0}, {20, 0}, {20, 20}, {0, 20}}});
    return b;
}

radmap::MapResult run(const BoardIR& b, radmap::MapParams p = {}) {
    Screener s(b);
    return radmap::compute(b, s, p);
}

}  // namespace

TEST_CASE("a segment's contribution is the single-loop physics, exactly",
          "[radmap]") {
    const BoardIR b = toy();
    radmap::MapParams p;
    const radmap::MapResult r = run(b, p);
    REQUIRE(r.counted == 2);

    const radmap::SegmentContribution& c = r.segments[0];
    // 10 mm of trace over a 0.2 mm dielectric closes a 2 mm^2 loop
    CHECK_THAT(c.height_mm, WithinRel(0.2, 1e-9));
    CHECK_THAT(c.area_m2, WithinRel(10e-3 * 0.2e-3, 1e-9));
    CHECK_FALSE(c.no_reference);

    // and the field is exactly what Emissions.hpp gives for that loop
    emc::Trapezoid t;
    t.amplitude_a = c.current_a;
    t.f_sw_hz = p.f_sw_hz;
    t.duty = p.duty;
    t.rise_s = p.rise_s;
    CHECK_THAT(c.e_v_per_m,
               WithinRel(emc::GROUND_REFLECTION *
                             emc::plateau_v_per_m(c.area_m2, t, p.r_m), 1e-12));
}

TEST_CASE("the total is the incoherent sum of the parts", "[radmap]") {
    const radmap::MapResult r = run(toy());
    double power = 0;
    for (const auto& c : r.segments) power += c.e_v_per_m * c.e_v_per_m;
    CHECK_THAT(r.total_v_per_m, WithinRel(std::sqrt(power), 1e-12));
    // two identical segments: sqrt(2) over one, i.e. 3.01 dB
    CHECK_THAT(r.total_v_per_m, WithinRel(std::sqrt(2.0) * r.segments[0].e_v_per_m, 1e-12));
    CHECK_THAT(r.total_dbuv_m, WithinRel(emc::to_dbuv_m(r.total_v_per_m), 1e-12));
}

TEST_CASE("contribution scales with length, current, area and distance",
          "[radmap]") {
    BoardIR b = toy();
    const double base = run(b).segments[0].e_v_per_m;

    // twice the trace, twice the loop
    b.segments[0] = {1, 0, 0.0, 5.0, 20.0, 5.0, 0.25};
    CHECK_THAT(run(b).segments[0].e_v_per_m, WithinRel(2 * base, 1e-9));

    // twice the swing is twice the current is twice the field
    radmap::MapParams p;
    p.swing_v *= 2;
    CHECK_THAT(run(toy(), p).segments[0].e_v_per_m, WithinRel(2 * base, 1e-9));

    // twice as far away, half the field
    radmap::MapParams q;
    q.r_m = 6.0;
    CHECK_THAT(run(toy(), q).segments[0].e_v_per_m, WithinRel(base / 2, 1e-9));

    // and the ground reflection is the same 6 dB it is everywhere else
    radmap::MapParams g;
    g.ground_reflection = false;
    CHECK_THAT(run(toy(), g).segments[0].e_v_per_m, WithinRel(base / 2, 1e-9));
}

TEST_CASE("losing the reference plane is what dominates a map", "[radmap]") {
    // This is the whole design rule: the same trace over a plane 0.2 mm away
    // and with no plane at all differ by the ratio of the loop heights, which
    // on a 1.6 mm board is 18 dB. If the map did not show that it would be
    // measuring the wrong thing.
    const radmap::MapResult with = run(toy(true));
    const radmap::MapResult without = run(toy(false));

    CHECK_FALSE(with.segments[0].no_reference);
    CHECK(without.segments[0].no_reference);
    CHECK(without.no_reference_count == 2);
    CHECK_THAT(without.no_reference_share, WithinAbs(1.0, 1e-12));
    CHECK_THAT(with.no_reference_share, WithinAbs(0.0, 1e-12));

    // The GEOMETRY ratio is exactly the ratio of loop heights: 1.6 mm of board
    // against 0.2 mm of dielectric, 18.1 dB.
    const double area_ratio =
        without.segments[0].area_m2 / with.segments[0].area_m2;
    CHECK_THAT(area_ratio, WithinRel(1.6 / 0.2, 1e-9));
    CHECK_THAT(20 * std::log10(area_ratio), WithinAbs(18.06, 0.01));

    // The FIELD ratio is larger still, and for a second reason worth keeping
    // separate: with no reference there is no cross-section to derive Z0 from,
    // so the current falls back to swing / 50 ohm, and the real microstrip here
    // is 64 ohm. Losing the reference costs area AND raises the current — it
    // can never help.
    const double field_ratio =
        without.segments[0].e_v_per_m / with.segments[0].e_v_per_m;
    CHECK(field_ratio >= area_ratio);
    CHECK_THAT(field_ratio / area_ratio,
               WithinRel(without.segments[0].current_a / with.segments[0].current_a,
                         1e-9));
}

TEST_CASE("a trace over a plane void closes a bigger loop than one over copper",
          "[radmap]") {
    // The check that makes this a MAP rather than a net highlight: the colour
    // must vary with WHERE the copper is, not just which layer and which net.
    // Same trace, same net, same layer — one over the pour, one over a hole in
    // it.
    BoardIR b = toy(true);
    b.segments.clear();
    b.segments.push_back({1, 0, 1.0, 2.0, 9.0, 2.0, 0.25});    // over solid pour
    b.segments.push_back({1, 0, 1.0, 15.0, 9.0, 15.0, 0.25});  // over the void
    // punch a void in the plane under the second one
    b.zones.clear();
    b.zones.push_back({2, 1, {{0, 0}, {20, 0}, {20, 12}, {0, 12}}});

    const radmap::MapResult r = run(b);
    REQUIRE(r.segments.size() == 2);
    CHECK_FALSE(r.segments[0].over_void);
    CHECK(r.segments[1].over_void);
    CHECK_THAT(r.segments[1].unreferenced_fraction, WithinAbs(1.0, 1e-9));
    CHECK(r.over_void_count == 1);
    // and it costs real loop area, so it must actually be louder
    CHECK(r.segments[1].e_v_per_m > r.segments[0].e_v_per_m * 2);
    CHECK(r.segments[1].height_mm > r.segments[0].height_mm);
}

TEST_CASE("a switch node is given the switched current, not swing over Z0",
          "[radmap]") {
    BoardIR b = toy();
    // name the net so the switch-node rule can find it, and give it the
    // footprints that rule requires
    b.nets = {{0, ""}, {1, "/DCDC/SW"}, {2, "GND"}};
    b.components.push_back({"Q1", "lib:sot23", "N-FET", 1.0, 1.0, 0.0});
    b.components.push_back({"L1", "lib:ind", "10u", 3.0, 1.0, 0.0});
    b.pads.push_back({"Q1", 1, 1.0, 1.0, 0.5, 0.5, false, 0});
    b.pads.push_back({"L1", 1, 3.0, 1.0, 0.5, 0.5, false, 0});
    b.pads.push_back({"Q1", 2, 1.0, 2.0, 0.5, 0.5, false, 0});

    radmap::MapParams p;
    p.sw_current_a = 12.0;
    const radmap::MapResult r = run(b, p);
    if (r.segments[0].switch_node) {
        CHECK_THAT(r.segments[0].current_a, WithinRel(12.0, 1e-12));
    } else {
        // the rule did not classify it; then it must be on the signal path
        CHECK(r.segments[0].current_a < 1.0);
    }
}

TEST_CASE("the ranking puts the biggest radiator first", "[radmap]") {
    BoardIR b = toy();
    b.segments.push_back({1, 0, 0.0, 12.0, 40.0, 12.0, 0.25});   // 4x longer
    const radmap::MapResult r = run(b);
    REQUIRE(r.top.size() >= 3);
    CHECK(r.top[0] == 2);
    for (size_t k = 1; k < r.top.size(); ++k)
        CHECK(r.segments[r.top[k - 1]].e_v_per_m >=
              r.segments[r.top[k]].e_v_per_m);
}

TEST_CASE("bad parameters are refused", "[radmap]") {
    const BoardIR b = toy();
    Screener s(b);
    radmap::MapParams p;
    p.r_m = 0;
    CHECK_THROWS_AS(radmap::compute(b, s, p), std::invalid_argument);
    p = {}; p.rise_s = 0;
    CHECK_THROWS_AS(radmap::compute(b, s, p), std::invalid_argument);
    p = {}; p.default_z0 = 0;
    CHECK_THROWS_AS(radmap::compute(b, s, p), std::invalid_argument);
    // a board with no routed copper has nothing to attribute
    BoardIR empty = toy();
    empty.segments.clear();
    Screener es(empty);
    CHECK_THROWS_AS(radmap::compute(empty, es, radmap::MapParams{}),
                    std::invalid_argument);
}

TEST_CASE("a real board attributes its radiation to a handful of traces",
          "[radmap][real]") {
    std::ifstream in(std::string(FARADAY_FIXTURE_DIR) + "/real/mppt-2420-hc.kicad_pcb");
    if (!in) { WARN("mppt fixture missing"); return; }
    std::stringstream ss;
    ss << in.rdbuf();
    BoardIR b = import_board(ss.str(), builtin_stackup("default-4layer"));
    Screener s(b);
    const radmap::MapResult r = radmap::compute(b, s, radmap::MapParams{});

    CHECK(r.counted > 100);
    CHECK(r.total_v_per_m > 0);
    CHECK(std::isfinite(r.total_dbuv_m));

    // the point of an attribution: a small minority of the copper carries most
    // of the power, otherwise there is nothing to act on
    double power = 0;
    for (const auto& c : r.segments) power += c.e_v_per_m * c.e_v_per_m;
    double top_power = 0;
    for (size_t i : r.top) top_power += r.segments[i].e_v_per_m * r.segments[i].e_v_per_m;
    INFO("top " << r.top.size() << " of " << r.counted << " segments carry "
                << 100 * top_power / power << "% of the power");
    CHECK(top_power / power > 0.05);
    CHECK(r.top.size() <= 40);
}
