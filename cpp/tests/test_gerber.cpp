// Gerber X2 set importer tests. The fixtures are synthesized in-test so every
// assertion is against a NUMBER WE PUT IN, not a captured baseline.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/Import.hpp>
#include <faraday/Screener.hpp>

using namespace faraday;
using Catch::Matchers::WithinAbs;

namespace {

// Two copper layers, X2 attributes throughout. Coordinates are FSLAX46Y46
// (6 decimal digits), so 10 mm = 10000000.
const char* TOP = R"(%TF.FileFunction,Copper,L1,Top*%
%FSLAX46Y46*%
%MOMM*%
%ADD10C,0.250000*%
%ADD11R,1.000000X0.600000*%
G01*
D10*
%TO.N,SIG*%
X0Y5000000D02*
X10000000Y5000000D01*
D11*
%TO.C,R1*%
X0Y5000000D03*
%TD*%
%TO.N,VIA1*%
D11*
X5000000Y2000000D03*
%TD*%
M02*
)";

const char* BOT = R"(%TF.FileFunction,Copper,L2,Bot*%
%FSLAX46Y46*%
%MOMM*%
G01*
%TO.N,GND*%
G36*
X0Y0D02*
X12000000Y0D01*
X12000000Y8000000D01*
X0Y8000000D01*
X0Y0D01*
G37*
%TD*%
M02*
)";

const char* PROFILE = R"(%TF.FileFunction,Profile,NP*%
%FSLAX46Y46*%
%MOMM*%
%ADD10C,0.100000*%
G01*
D10*
X0Y0D02*
X12000000Y0D01*
X12000000Y8000000D01*
X0Y8000000D01*
X0Y0D01*
M02*
)";

const char* DRILL = R"(M48
METRIC
T1C0.300
%
T1
X5.0Y2.0
M30
)";

std::vector<gerber::NamedFile> fixture_set() {
    return {{"top.gbr", TOP}, {"bot.gbr", BOT},
            {"profile.gbr", PROFILE}, {"drill.drl", DRILL}};
}

}  // namespace

TEST_CASE("a gerber X2 set imports with exact geometry", "[gerber]") {
    BoardFormat fmt;
    BoardIR b = import_board_set(fixture_set(),
                                 builtin_stackup("default-2layer"), &fmt);
    CHECK(fmt == BoardFormat::GerberSet);
    REQUIRE(b.copper_names.size() == 2);
    CHECK(b.copper_names[0] == "Top");
    CHECK(b.copper_names[1] == "Bottom");

    // the one track: 10 mm at y=5, 0.25 mm wide, net SIG, on copper 0
    REQUIRE(b.segments.size() == 1);
    const auto& s = b.segments[0];
    CHECK(s.cu == 0);
    CHECK_THAT(s.x1, WithinAbs(0.0, 1e-9));
    CHECK_THAT(s.y1, WithinAbs(5.0, 1e-9));
    CHECK_THAT(s.x2, WithinAbs(10.0, 1e-9));
    CHECK_THAT(s.width, WithinAbs(0.25, 1e-9));
    CHECK(b.net_name(s.net) == "SIG");

    // the pour: net GND, on copper 1, containing an interior point and
    // excluding an exterior one
    REQUIRE(b.zones.size() == 1);
    CHECK(b.zones[0].cu == 1);
    CHECK(b.net_name(b.zones[0].net) == "GND");
    CHECK(b.zones[0].contains(6.0, 4.0));
    CHECK(!b.zones[0].contains(13.0, 4.0));
    CHECK_THAT(std::abs(b.zones[0].signed_area()), WithinAbs(96.0, 1e-9));

    // the drill at (5,2) matched the VIA1 flash → a through via, not a pad
    REQUIRE(b.vias.size() == 1);
    CHECK_THAT(b.vias[0].x, WithinAbs(5.0, 1e-9));
    CHECK_THAT(b.vias[0].y, WithinAbs(2.0, 1e-9));
    CHECK_THAT(b.vias[0].drill, WithinAbs(0.3, 1e-9));
    CHECK(b.net_name(b.vias[0].net) == "VIA1");
    CHECK(b.vias[0].cu_from == 0);
    CHECK(b.vias[0].cu_to == 1);

    // R1's pad survives as a pad, with the component assembled from %TO.C
    REQUIRE(b.pads.size() == 1);
    CHECK(b.pads[0].component == "R1");
    CHECK_THAT(b.pads[0].w, WithinAbs(1.0, 1e-9));
    CHECK_THAT(b.pads[0].h, WithinAbs(0.6, 1e-9));
    REQUIRE(b.components.size() == 1);
    CHECK(b.components[0].reference == "R1");

    // bbox from the Profile file, exactly 12 x 8
    CHECK(b.bbox_from_outline);
    CHECK_THAT(b.bbox_x2 - b.bbox_x1, WithinAbs(12.0, 1e-9));
    CHECK_THAT(b.bbox_y2 - b.bbox_y1, WithinAbs(8.0, 1e-9));

    // and the full analysis runs on it
    nlohmann::json rep = analyze_board(b);
    CHECK(rep.contains("findings"));
}

TEST_CASE("inch units scale by 25.4 exactly", "[gerber]") {
    const char* in_file = R"(%TF.FileFunction,Copper,L1,Top*%
%FSLAX24Y24*%
%MOIN*%
%ADD10C,0.010000*%
G01*
D10*
%TO.N,A*%
X0Y0D02*
X10000Y0D01*
M02*
)";
    gerber::GerberLayer L = gerber::parse_gerber(in_file);
    REQUIRE(L.tracks.size() == 1);
    // 1.0000 inch with FSLAX24 (4 decimals) = 25.4 mm; 10 mil aperture
    CHECK_THAT(L.tracks[0].x2, WithinAbs(25.4, 1e-9));
    CHECK_THAT(L.tracks[0].w, WithinAbs(0.254, 1e-9));
}

TEST_CASE("arcs are chord-approximated and counted", "[gerber]") {
    const char* arc = R"(%TF.FileFunction,Copper,L1,Top*%
%FSLAX46Y46*%
%MOMM*%
%ADD10C,0.200000*%
G75*
D10*
%TO.N,A*%
X0Y0D02*
G03*
X0Y10000000I0J5000000D01*
M02*
)";
    gerber::GerberLayer L = gerber::parse_gerber(arc);
    CHECK(L.arcs_approximated == 1);
    REQUIRE(L.tracks.size() == 8);
    // the half-circle's chords land back on the endpoint (0, 10)
    CHECK_THAT(L.tracks.back().x2, WithinAbs(0.0, 1e-9));
    CHECK_THAT(L.tracks.back().y2, WithinAbs(10.0, 1e-9));
    // total chord length is close to (and below) the true arc pi*r
    double len = 0;
    for (const auto& t : L.tracks) len += std::hypot(t.x2 - t.x1, t.y2 - t.y1);
    CHECK(len < M_PI * 5.0);
    CHECK(len > M_PI * 5.0 * 0.98);
}

TEST_CASE("clear-polarity objects are skipped AND counted", "[gerber]") {
    const char* lpc = R"(%TF.FileFunction,Copper,L1,Top*%
%FSLAX46Y46*%
%MOMM*%
%ADD10C,0.200000*%
G01*
D10*
%TO.N,A*%
%LPC*%
G36*
X0Y0D02*
X1000000Y0D01*
X1000000Y1000000D01*
X0Y0D01*
G37*
%LPD*%
X0Y0D02*
X5000000Y0D01*
M02*
)";
    gerber::GerberLayer L = gerber::parse_gerber(lpc);
    CHECK(L.clear_skipped == 1);
    CHECK(L.regions.empty());
    REQUIRE(L.tracks.size() == 1);   // the dark draw after LPD survives
}

TEST_CASE("a set without net attributes is refused, with the fix named",
          "[gerber]") {
    auto files = fixture_set();
    for (auto& f : files) {
        size_t p;
        while ((p = f.text.find("%TO.N,")) != std::string::npos) {
            const size_t e = f.text.find('\n', p);
            f.text.erase(p, e - p + 1);
        }
    }
    CHECK_THROWS_WITH(
        import_board_set(files, builtin_stackup("default-2layer")),
        Catch::Matchers::ContainsSubstring("netlist attributes"));
}

TEST_CASE("a set without a stackup names the copper count", "[gerber]") {
    CHECK_THROWS_WITH(import_board_set(fixture_set()),
                      Catch::Matchers::ContainsSubstring("default-2layer"));
}

TEST_CASE("a lone gerber file gets told it needs the set", "[gerber]") {
    CHECK_THROWS_WITH(import_board(TOP),
                      Catch::Matchers::ContainsSubstring("SET"));
}

TEST_CASE("fixed-format excellon is refused, not misplaced", "[gerber]") {
    CHECK_THROWS_WITH(gerber::parse_excellon("M48\nMETRIC\nT1C0.3\n%\nT1\nX10000Y10000\nM30\n"),
                      Catch::Matchers::ContainsSubstring("decimal"));
}
