// The return-path map. Everything asserted here is a geometric fact the layout
// proves — there is no current, no field and no dB in this layer, which is the
// entire point of the rework: ranked WITH an assumed current the old map's
// answer on a real converter was 97% a restatement of the switch-node rule,
// and ranked by geometry it was a different answer entirely.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/Import.hpp>
#include <faraday/ReturnPath.hpp>

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
        // "signal", not "power": the plane classifier honours KiCad's
        // layer-type hint, and this fixture must vary ONLY the pour.
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

rp::MapResult run(const BoardIR& b, rp::MapParams p = {}) {
    Screener s(b);
    return rp::compute(b, s, p);
}

}  // namespace

TEST_CASE("a segment's loop is its length times the real height, exactly",
          "[rp]") {
    const rp::MapResult r = run(toy());
    REQUIRE(r.counted == 2);
    const auto& c = r.segments[0];
    // 10 mm of trace over a 0.2 mm dielectric: 2 mm^2, eff height 0.2 mm
    CHECK_THAT(c.height_mm, WithinRel(0.2, 1e-9));
    CHECK_THAT(c.area_mm2, WithinRel(2.0, 1e-9));
    CHECK_THAT(c.eff_height_mm, WithinRel(0.2, 1e-9));
    CHECK_FALSE(c.no_reference);
    CHECK_FALSE(c.over_void);
}

TEST_CASE("effective height is independent of how the router chopped the trace",
          "[rp]") {
    // The legibility failure of the first radiation map, now impossible by
    // construction: a 1 mm segment and a 40 mm segment of the same trace get
    // the same colour, because the metric is area PER UNIT LENGTH.
    BoardIR b = toy();
    b.segments.clear();
    b.segments.push_back({1, 0, 0.0, 5.0, 1.0, 5.0, 0.25});    // 1 mm
    b.segments.push_back({1, 0, 0.0, 8.0, 40.0, 8.0, 0.25});   // 40 mm
    b.bbox_x2 = 50;
    b.zones.clear();
    b.zones.push_back({2, 1, {{0, 0}, {50, 0}, {50, 20}, {0, 20}}});
    const rp::MapResult r = run(b);
    CHECK_THAT(r.segments[0].eff_height_mm,
               WithinRel(r.segments[1].eff_height_mm, 1e-9));
    // while the AREAS differ by exactly the length ratio
    CHECK_THAT(r.segments[1].area_mm2, WithinRel(40.0 * r.segments[0].area_mm2, 1e-9));
}

TEST_CASE("losing the reference plane is what dominates the map", "[rp]") {
    // The design rule itself: the same trace over a 0.2 mm plane and with no
    // plane at all differ by the ratio of the loop heights — a factor of 8 on
    // a 1.6 mm board, and a pure geometric fact.
    const rp::MapResult with = run(toy(true));
    const rp::MapResult without = run(toy(false));

    CHECK_FALSE(with.segments[0].no_reference);
    CHECK(without.segments[0].no_reference);
    CHECK(without.no_reference_count == 2);
    CHECK_THAT(without.segments[0].eff_height_mm /
                   with.segments[0].eff_height_mm,
               WithinRel(1.6 / 0.2, 1e-9));
}

TEST_CASE("a void under the trace raises its effective height", "[rp]") {
    BoardIR b = toy(true);
    b.segments.clear();
    b.segments.push_back({1, 0, 1.0, 2.0, 9.0, 2.0, 0.25});    // over solid pour
    b.segments.push_back({1, 0, 1.0, 15.0, 9.0, 15.0, 0.25});  // over the void
    b.zones.clear();
    b.zones.push_back({2, 1, {{0, 0}, {20, 0}, {20, 12}, {0, 12}}});

    const rp::MapResult r = run(b);
    CHECK_FALSE(r.segments[0].over_void);
    CHECK(r.segments[1].over_void);
    CHECK_THAT(r.segments[1].unreferenced_fraction, WithinAbs(1.0, 1e-9));
    CHECK(r.over_void_count == 1);
    CHECK(r.segments[1].eff_height_mm > 2.0 * r.segments[0].eff_height_mm);
}

TEST_CASE("the worst list ranks nets by enclosed area, a geometric fact",
          "[rp]") {
    BoardIR b = toy();
    b.nets.push_back({3, "LONG"});
    b.bbox_x2 = 50;
    b.zones.clear();
    b.zones.push_back({2, 1, {{0, 0}, {50, 0}, {50, 20}, {0, 20}}});
    b.segments.push_back({3, 0, 0.0, 12.0, 40.0, 12.0, 0.25});   // 4x the copper
    const rp::MapResult r = run(b);
    REQUIRE_FALSE(r.worst.empty());
    CHECK(r.worst[0].net == 3);
    for (size_t k = 1; k < r.worst.size(); ++k)
        CHECK(r.worst[k - 1].area_mm2 >= r.worst[k].area_mm2);
}

TEST_CASE("a layer change with no stitching via nearby costs loop area",
          "[rp]") {
    // When a signal changes layers its return must hop between planes, and it
    // can only do that through a stitching via. With none nearby the return
    // travels a long way round — a classic failure — and the old map added NO
    // penalty at all, colouring it identically to a trace that never changed
    // layers. Silent exactly where it should be loudest.
    auto board_with = [](double stitch_dx) {
        BoardIR b;
        b.copper_names = {"F.Cu", "In1.Cu", "In2.Cu", "B.Cu"};
        b.stackup.layers = {
            {LayerKind::Copper, "F.Cu", 0.035, std::nullopt, "signal"},
            {LayerKind::Dielectric, "d1", 0.2, 4.3, ""},
            {LayerKind::Copper, "In1.Cu", 0.035, std::nullopt, "signal"},
            {LayerKind::Dielectric, "d2", 1.0, 4.3, ""},
            {LayerKind::Copper, "In2.Cu", 0.035, std::nullopt, "signal"},
            {LayerKind::Dielectric, "d3", 0.2, 4.3, ""},
            {LayerKind::Copper, "B.Cu", 0.035, std::nullopt, "signal"},
        };
        b.stackup.source = "test";
        b.nets = {{0, ""}, {1, "SIG"}, {2, "GND"}};
        b.bbox_x1 = 0; b.bbox_y1 = 0; b.bbox_x2 = 60; b.bbox_y2 = 40;
        b.bbox_from_outline = true;
        b.segments.push_back({1, 0, 5.0, 10.0, 20.0, 10.0, 0.25});
        b.vias.push_back({1, 20.0, 10.0, 0.6, 0.3, 0, 3});
        b.vias.push_back({2, 20.0 + stitch_dx, 10.0, 0.6, 0.3, 0, 3});
        b.zones.push_back({2, 1, {{0, 0}, {60, 0}, {60, 40}, {0, 40}}});
        b.zones.push_back({2, 2, {{0, 0}, {60, 0}, {60, 40}, {0, 40}}});
        return b;
    };

    const rp::MapResult tight = run(board_with(0.6));
    const rp::MapResult loose = run(board_with(18.0));

    REQUIRE(tight.segments[0].layer_change);
    REQUIRE(loose.segments[0].layer_change);
    CHECK(tight.segments[0].stitch_distance_mm < loose.segments[0].stitch_distance_mm);
    CHECK(tight.segments[0].return_area_mm2 < loose.segments[0].return_area_mm2);
    CHECK(loose.segments[0].eff_height_mm > tight.segments[0].eff_height_mm);
    CHECK(tight.layer_change_count == 1);
}

TEST_CASE("no stitch at all within reach is flagged, not silently capped",
          "[rp]") {
    BoardIR b;
    b.copper_names = {"F.Cu", "In1.Cu", "B.Cu"};
    b.stackup.layers = {
        {LayerKind::Copper, "F.Cu", 0.035, std::nullopt, "signal"},
        {LayerKind::Dielectric, "d1", 0.2, 4.3, ""},
        {LayerKind::Copper, "In1.Cu", 0.035, std::nullopt, "signal"},
        {LayerKind::Dielectric, "d2", 1.0, 4.3, ""},
        {LayerKind::Copper, "B.Cu", 0.035, std::nullopt, "signal"},
    };
    b.stackup.source = "test";
    b.nets = {{0, ""}, {1, "SIG"}, {2, "GND"}};
    b.bbox_x1 = 0; b.bbox_y1 = 0; b.bbox_x2 = 60; b.bbox_y2 = 40;
    b.bbox_from_outline = true;
    b.segments.push_back({1, 0, 5.0, 10.0, 20.0, 10.0, 0.25});
    b.vias.push_back({1, 20.0, 10.0, 0.6, 0.3, 0, 2});   // no ground via at all
    b.zones.push_back({2, 1, {{0, 0}, {60, 0}, {60, 40}, {0, 40}}});
    b.zones.push_back({2, 2, {{0, 0}, {60, 0}, {60, 40}, {0, 40}}});

    const rp::MapResult r = run(b);
    CHECK(r.segments[0].layer_change);
    CHECK(r.segments[0].unstitched);
    CHECK(r.unstitched_count == 1);
    CHECK(r.segments[0].return_area_mm2 > 0);
}

TEST_CASE("a trace that never changes layers gets no return penalty", "[rp]") {
    const rp::MapResult r = run(toy(true));
    for (const auto& c : r.segments) {
        CHECK_FALSE(c.layer_change);
        CHECK(c.return_area_mm2 == 0.0);
    }
    CHECK(r.layer_change_count == 0);
}

TEST_CASE("bad parameters and empty boards are refused", "[rp]") {
    const BoardIR b = toy();
    Screener s(b);
    rp::MapParams p;
    p.no_reference_height_mm = 0;
    CHECK_THROWS_AS(rp::compute(b, s, p), std::invalid_argument);
    p = {}; p.max_return_detour_mm = 0;
    CHECK_THROWS_AS(rp::compute(b, s, p), std::invalid_argument);
    BoardIR empty = toy();
    empty.segments.clear();
    Screener es(empty);
    CHECK_THROWS_AS(rp::compute(empty, es, rp::MapParams{}), std::invalid_argument);
}

TEST_CASE("a real board maps its return quality without inventing anything",
          "[rp][real]") {
    std::ifstream in(std::string(FARADAY_FIXTURE_DIR) + "/real/mppt-2420-hc.kicad_pcb");
    if (!in) { WARN("mppt fixture missing"); return; }
    std::stringstream ss;
    ss << in.rdbuf();
    BoardIR b = import_board(ss.str(), builtin_stackup("default-4layer"));
    Screener s(b);
    const rp::MapResult r = rp::compute(b, s, rp::MapParams{});

    CHECK(r.counted > 100);
    CHECK(r.min_eff_mm > 0);
    CHECK(r.max_eff_mm > r.min_eff_mm);
    // the board has voids and layer changes — the two things this map exists
    // to show, and both are pure geometry
    CHECK(r.over_void_count > 0);
    CHECK(r.layer_change_count > 0);
    REQUIRE_FALSE(r.worst.empty());
    for (const auto& w : r.worst) CHECK(w.area_mm2 > 0);
    for (const auto& c : r.segments)
        if (c.len_mm > 0) {
            CHECK(std::isfinite(c.eff_height_mm));
            CHECK(c.eff_height_mm >= 0);
        }
}

TEST_CASE("at HF the return follows the slot edge, not the whole board",
          "[rp]") {
    // The high-frequency return does not teleport: at a void it diverts
    // laterally around the slot on the SAME plane. A trace grazing the edge of
    // a void therefore pays a small detour, and one deep inside a large void
    // pays a big one — a distance a "next plane down or the whole board"
    // model cannot see. On this 2-layer board there is no second plane, so the
    // lateral path is the only one, and the model must find it.
    auto board_at = [](double trace_y) {
        BoardIR b;
        b.copper_names = {"F.Cu", "B.Cu"};
        b.stackup.layers = {
            {LayerKind::Copper, "F.Cu", 0.035, std::nullopt, "signal"},
            {LayerKind::Dielectric, "core", 0.2, 4.3, ""},
            {LayerKind::Copper, "B.Cu", 0.035, std::nullopt, "signal"},
        };
        b.stackup.source = "test";
        b.nets = {{0, ""}, {1, "SIG"}, {2, "GND"}};
        b.bbox_x1 = 0; b.bbox_y1 = 0; b.bbox_x2 = 40; b.bbox_y2 = 40;
        b.bbox_from_outline = true;
        // pour covers y < 24 (60% — enough to classify as a plane); above it
        // is one large void
        b.zones.push_back({2, 1, {{0, 0}, {40, 0}, {40, 24}, {0, 24}}});
        b.segments.push_back({1, 0, 5.0, trace_y, 35.0, trace_y, 0.25});
        return b;
    };

    const rp::MapResult grazing = run(board_at(25.5));   // just past the edge
    const rp::MapResult deep = run(board_at(38.0));      // 14 mm past it

    REQUIRE(grazing.segments[0].over_void);
    REQUIRE(deep.segments[0].over_void);
    // the grazing trace's return only has to bulge ~1-2 mm sideways
    CHECK(grazing.segments[0].height_mm < 3.0);
    // the deep one pays an order of magnitude more
    CHECK(deep.segments[0].height_mm > 5.0 * grazing.segments[0].height_mm);
    CHECK(deep.segments[0].eff_height_mm > grazing.segments[0].eff_height_mm);
}
