// The import-time plausibility gate. These tests are the statement of what
// the gate is FOR: it must refuse a board that could not have been made, name
// the likely cause, and let every real board through untouched.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/Import.hpp>
#include <faraday/Plausible.hpp>

#include <fstream>
#include <sstream>

using namespace faraday;

namespace {

std::string read_real(const char* name) {
    std::ifstream in(std::string(FARADAY_FIXTURE_DIR) + "/real/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A board at a stated scale: 40 x 25 mm with 0.25 mm routing, times `k`.
std::string board_at_scale(double k) {
    auto n = [k](double v) { return std::to_string(v * k); };
    return "(kicad_pcb (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal))"
           " (net 0 \"\") (net 1 \"A\") (net 2 \"B\")"
           " (segment (start " + n(5) + " " + n(5) + ") (end " + n(35) + " " + n(5) +
           ") (width " + n(0.25) + ") (layer \"F.Cu\") (net 1))"
           " (segment (start " + n(5) + " " + n(9) + ") (end " + n(35) + " " + n(9) +
           ") (width " + n(0.25) + ") (layer \"F.Cu\") (net 2))"
           " (gr_line (start " + n(0) + " " + n(0) + ") (end " + n(40) + " " + n(0) +
           ") (layer \"Edge.Cuts\"))"
           " (gr_line (start " + n(40) + " " + n(0) + ") (end " + n(40) + " " + n(25) +
           ") (layer \"Edge.Cuts\"))"
           " (gr_line (start " + n(40) + " " + n(25) + ") (end " + n(0) + " " + n(25) +
           ") (layer \"Edge.Cuts\"))"
           " (gr_line (start " + n(0) + " " + n(25) + ") (end " + n(0) + " " + n(0) +
           ") (layer \"Edge.Cuts\")))";
}

}  // namespace

TEST_CASE("plausible: a real board passes untouched", "[plausible]") {
    BoardIR b = import_board(board_at_scale(1.0), builtin_stackup("default-2layer"));
    CHECK(b.plausibility_notes.empty());
    // and the two production fixtures, which is what "does not fire on real
    // boards" actually means
    BoardIR h = import_board(read_real("hackrf-one.kicad_pcb"));
    CHECK(h.plausibility_notes.empty());
    BoardIR m = import_board(read_real("mppt-2420-hc.kicad_pcb"),
                             builtin_stackup("default-4layer"));
    CHECK(m.plausibility_notes.empty());
}

// This is the ODB++ units bug, reproduced as pure geometry: an inch board read
// as mm is 1/25.4 scale. Nothing about it is unparseable — it screens fine and
// reports numbers that are all wrong. Only the gate can tell.
TEST_CASE("plausible: an inch board read as mm is refused, with the factor named",
          "[plausible]") {
    CHECK_THROWS_WITH(
        import_board(board_at_scale(1.0 / 25.4), builtin_stackup("default-2layer")),
        Catch::Matchers::ContainsSubstring("not imported at its true scale") &&
            Catch::Matchers::ContainsSubstring("exported in inches"));
}

TEST_CASE("plausible: an mm board read as inches is refused too", "[plausible]") {
    CHECK_THROWS_WITH(
        import_board(board_at_scale(25.4), builtin_stackup("default-2layer")),
        Catch::Matchers::ContainsSubstring("larger than any fabrication panel") &&
            Catch::Matchers::ContainsSubstring("exported in millimetres"));
}

// The bounds must sit OUTSIDE any real process, not at it — a fine-pitch HDI
// board with 0.075 mm routing is unusual, not impossible, and must pass.
TEST_CASE("plausible: fine-pitch HDI is unusual, not impossible", "[plausible]") {
    std::string hdi =
        "(kicad_pcb (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal))"
        " (net 0 \"\") (net 1 \"A\") (net 2 \"B\")"
        " (segment (start 1 1) (end 6 1) (width 0.075) (layer \"F.Cu\") (net 1))"
        " (segment (start 1 1.2) (end 6 1.2) (width 0.075) (layer \"F.Cu\") (net 2))"
        " (via (at 3 1) (size 0.2) (drill 0.1) (layers \"F.Cu\" \"B.Cu\") (net 1)))";
    BoardIR b = import_board(hdi, builtin_stackup("default-2layer"));
    CHECK(b.plausibility_notes.empty());
    CHECK(b.segments.size() == 2);
}

TEST_CASE("plausible: the gate is a function of the IR, not of any format",
          "[plausible]") {
    // called directly, so any importer (present or future) can be gated
    BoardIR b;
    b.segments.push_back({1, 0, 0, 0, 10, 0, 0.0004});   // 0.4 um routing
    b.bbox_x2 = 0.5; b.bbox_y2 = 0.3;
    auto notes = plausible::check(b);
    REQUIRE(!notes.empty());
    CHECK(notes[0].fatal);
    CHECK_THAT(notes[0].what,
               Catch::Matchers::ContainsSubstring("median routed conductor"));
}
