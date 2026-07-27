#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <faraday/KicadImporter.hpp>

#include <fstream>
#include <sstream>

using Catch::Approx;
using namespace faraday;

static std::string read_fixture(const char* name) {
    std::ifstream in(std::string(FARADAY_FIXTURE_DIR) + "/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

TEST_CASE("importer: fixture 2-layer board round-trips into the IR", "[importer]") {
    BoardIR b = import_kicad(read_fixture("fixture_2layer.kicad_pcb"));

    REQUIRE(b.copper_names.size() == 2);
    CHECK(b.copper_names[0] == "F.Cu");
    CHECK(b.copper_names[1] == "B.Cu");

    // stackup from the board file
    CHECK(b.stackup.source == "board-file");
    REQUIRE(b.stackup.layers.size() == 3);  // Cu / core / Cu
    CHECK(b.stackup.layers[1].kind == LayerKind::Dielectric);
    CHECK(b.stackup.layers[1].thickness_mm == Approx(1.51));
    REQUIRE(b.stackup.layers[1].epsilon_r.has_value());
    CHECK(*b.stackup.layers[1].epsilon_r == Approx(4.5));
    double h, eps;
    b.stackup.dielectric_between(0, 1, h, eps);
    CHECK(h == Approx(1.51));
    CHECK(eps == Approx(4.5));

    REQUIRE(b.nets.size() == 4);
    CHECK(b.net_name(2) == "CLK");

    REQUIRE(b.segments.size() == 3);
    CHECK(b.segments[0].net == 2);
    CHECK(b.segments[0].cu == 0);
    CHECK(b.segments[0].width == Approx(0.3));

    REQUIRE(b.vias.size() == 1);
    CHECK(b.vias[0].cu_from == 0);
    CHECK(b.vias[0].cu_to == 1);

    REQUIRE(b.zones.size() == 2);
    CHECK(b.zones[0].net == 1);
    CHECK(b.zones[0].cu == 1);
    CHECK(std::abs(b.zones[0].signed_area()) == Approx(24.0 * 30.0));
    CHECK(b.zones[0].contains(10, 15));
    CHECK_FALSE(b.zones[0].contains(25, 15));  // the split

    // footprint: reference + rotated pads (90 deg -> offset along +y, w/h swap)
    REQUIRE(b.components.size() == 1);
    CHECK(b.components[0].reference == "R1");
    CHECK(b.components[0].value == "100n");
    REQUIRE(b.pads.size() == 2);
    CHECK(b.pads[0].net == 2);
    CHECK(b.pads[0].x == Approx(10.0).margin(1e-9));
    CHECK(b.pads[0].y == Approx(20.7875));
    CHECK(b.pads[0].w == Approx(0.95));  // swapped at 90 deg
    CHECK(b.pads[0].h == Approx(0.9));

    // outline bbox from Edge.Cuts
    CHECK(b.bbox_from_outline);
    CHECK(b.bbox_x2 == Approx(50.0));
    CHECK(b.bbox_y2 == Approx(30.0));
    CHECK(b.approximated_arcs == 0);
}

TEST_CASE("importer: no stackup and no user stackup -> loud refusal", "[importer]") {
    std::string txt = R"((kicad_pcb (version 20221018)
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "A") (net 2 "B")
      (segment (start 0 0) (end 10 0) (width 0.3) (layer "F.Cu") (net 1))
      (segment (start 0 1) (end 10 1) (width 0.3) (layer "F.Cu") (net 2))
    ))";
    CHECK_THROWS_WITH(import_kicad(txt),
                      Catch::Matchers::ContainsSubstring("no stackup"));
    // explicit user stackup unblocks it
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    CHECK(b.stackup.source == "user:default-2layer");
    CHECK_FALSE(b.bbox_from_outline);  // no Edge.Cuts -> geometry bbox, reported
}

TEST_CASE("importer: stackup/board copper-count mismatch throws", "[importer]") {
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "")
      (segment (start 0 0) (end 10 0) (width 0.3) (layer "F.Cu") (net 0))
      (segment (start 0 1) (end 10 1) (width 0.3) (layer "F.Cu") (net 0))
    ))";
    CHECK_THROWS_WITH(import_kicad(txt, builtin_stackup("default-4layer")),
                      Catch::Matchers::ContainsSubstring("copper layers"));
}

TEST_CASE("importer: copper order is canonical even with KiCad-9-style ids", "[importer]") {
    // KiCad 9 renumbered layer ids (B.Cu no longer 31) — order must come from
    // the names, not the ids or file order.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (2 "B.Cu" signal) (4 "In1.Cu" signal) (6 "In2.Cu" power))
      (net 0 "")
      (segment (start 0 0) (end 10 0) (width 0.3) (layer "In1.Cu") (net 0))
      (segment (start 0 1) (end 10 1) (width 0.3) (layer "F.Cu") (net 0))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-4layer"));
    REQUIRE(b.copper_names.size() == 4);
    CHECK(b.copper_names[0] == "F.Cu");
    CHECK(b.copper_names[1] == "In1.Cu");
    CHECK(b.copper_names[2] == "In2.Cu");
    CHECK(b.copper_names[3] == "B.Cu");
    CHECK(b.segments[0].cu == 1);  // In1.Cu
    // the "power" type hint survives onto the stackup copper entry
    auto cu = b.stackup.copper_indices();
    CHECK(b.stackup.layers[cu[2]].copper_type == "power");
}

TEST_CASE("importer: arcs are chord-approximated and counted", "[importer]") {
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "A")
      (arc (start 0 0) (mid 5 5) (end 10 0) (width 0.3) (layer "F.Cu") (net 1))
      (segment (start 0 1) (end 10 1) (width 0.3) (layer "F.Cu") (net 1))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    CHECK(b.approximated_arcs == 1);
    CHECK(b.segments.size() == 2);
}

TEST_CASE("importer: unknown built-in stackup name throws", "[importer]") {
    CHECK_THROWS_WITH(builtin_stackup("nonsense"),
                      Catch::Matchers::ContainsSubstring("unknown built-in stackup"));
}
