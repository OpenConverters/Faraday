// HyperLynx .HYP importer. The format reaches Altium, PADS, Expedition and
// Eagle, so one importer covers four more EDA tools.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <faraday/HypImporter.hpp>
#include <faraday/Screener.hpp>
#include <faraday/Report.hpp>

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

TEST_CASE("hyp: 4-layer metric board imports fully", "[hyp]") {
    BoardIR b = import_hyp(read_fixture("fixture_4layer.hyp"));

    // stackup: copper order follows the STACKUP section, PLANE keeps its hint
    REQUIRE(b.copper_names.size() == 4);
    CHECK(b.copper_names[0] == "Top");
    CHECK(b.copper_names[1] == "GNDPLANE");
    CHECK(b.copper_names[3] == "Bottom");
    REQUIRE(b.stackup.layers.size() == 7);       // 4 copper + 3 dielectrics
    auto cu = b.stackup.copper_indices();
    CHECK(b.stackup.layers[cu[1]].copper_type == "power");   // (PLANE ...)
    CHECK(b.stackup.layers[cu[0]].copper_type == "signal");
    // permittivity is C= in this format, not ER=
    double h, eps;
    b.stackup.dielectric_between(0, 1, h, eps);
    CHECK(h == Approx(0.2));
    CHECK(eps == Approx(4.4));

    // geometry: SEG + ARC (chord-approximated and counted)
    CHECK(b.segments.size() == 3);
    CHECK(b.approximated_arcs == 1);
    CHECK(b.segments[0].cu == 0);
    CHECK(b.segments[0].width == Approx(0.3));
    REQUIRE(b.vias.size() == 1);
    CHECK(b.vias[0].cu_from == 0);
    CHECK(b.vias[0].cu_to == 3);
    CHECK(b.vias[0].drill == Approx(0.3));

    // PIN records give pads with their component reference
    REQUIRE(b.pads.size() == 3);
    CHECK(b.pads[0].component == "U1");          // from R=U1.1
    CHECK(b.pads[0].x == Approx(5.0));
    CHECK(b.pads[0].w == Approx(0.9));           // from PADSTACK=SMD001

    // POLYGON: layer and first vertex live in the block header
    REQUIRE(b.zones.size() == 1);
    CHECK(b.zones[0].cu == 1);                   // GNDPLANE
    CHECK(std::abs(b.zones[0].signed_area()) == Approx(48.0 * 28.0));
    CHECK(b.zones[0].contains(25, 15));

    CHECK(b.components.size() == 2);
    CHECK(b.bbox_from_outline);
    CHECK(b.bbox_x2 == Approx(50.0));
    CHECK(b.stackup.source == "board-file");
}

TEST_CASE("hyp: imperial boards are converted to millimetres", "[hyp]") {
    // The format's default unit is the inch; 0.7874 in = 20 mm.
    std::string txt = R"(
{VERSION=2.10}
{UNITS=ENGLISH LENGTH}
{BOARD
(PERIMETER_SEGMENT X1=0.0 Y1=0.0 X2=0.7874 Y2=0.0)
(PERIMETER_SEGMENT X1=0.7874 Y1=0.7874 X2=0.0 Y2=0.7874)
}
{STACKUP
(SIGNAL T=0.0014 L=Top)
(DIELECTRIC T=0.0594 C=4.8 L=DL01)
(SIGNAL T=0.0014 L=Bottom)
}
{NET=A
(SEG X1=0.1 Y1=0.1 X2=0.5 Y2=0.1 W=0.008 L=Top)
}
)";
    BoardIR b = import_hyp(txt);
    CHECK(b.bbox_x2 == Approx(20.0).margin(0.01));
    CHECK(b.bbox_y2 == Approx(20.0).margin(0.01));
    CHECK(b.segments[0].width == Approx(0.2032));       // 8 mil
    CHECK(b.stackup.layers[0].thickness_mm == Approx(0.03556));
}

TEST_CASE("hyp: a HYP board screens like any other", "[hyp]") {
    BoardIR b = import_hyp(read_fixture("fixture_4layer.hyp"));
    nlohmann::json report = analyze_board(b);
    // CLK/DATA run 40 mm at 0.5 mm centre spacing over the plane 0.2 mm below:
    // k = 0.25/(1+(0.5/0.2)^2) = 0.0345 -> -29.2 dB
    const nlohmann::json* cr = nullptr;
    for (const auto& f : report["findings"])
        if (f["rule"] == "coupled-run") { cr = &f; break; }
    REQUIRE(cr != nullptr);
    CHECK((*cr)["nextDb"].get<double>() == Approx(-29.2).margin(0.3));
    CHECK((*cr)["coupledLenMm"].get<double>() == Approx(40.0).margin(0.5));
    // the PLANE layer is recognised as the reference
    CHECK(report["meta"]["planes"][1]["isPlane"] == true);
}

TEST_CASE("hyp: missing units or stackup is refused, never guessed", "[hyp]") {
    CHECK_THROWS_WITH(import_hyp("{VERSION=2.10}\n{STACKUP\n(SIGNAL T=1 L=Top)\n}\n"),
                      Catch::Matchers::ContainsSubstring("no {UNITS"));
    CHECK_THROWS_WITH(import_hyp("{UNITS=METRIC LENGTH=MM}\n{BOARD\n}\n"),
                      Catch::Matchers::ContainsSubstring("no {STACKUP"));
    // a dielectric without permittivity cannot yield an impedance
    CHECK_THROWS_WITH(
        import_hyp("{UNITS=METRIC LENGTH=MM}\n{STACKUP\n(SIGNAL T=0.035 L=Top)\n"
                   "(DIELECTRIC T=1.5 L=DL01)\n(SIGNAL T=0.035 L=Bot)\n}\n"),
        Catch::Matchers::ContainsSubstring("no C="));
}
