// Gerber X2 set importer tests. The fixtures are synthesized in-test so every
// assertion is against a NUMBER WE PUT IN, not a captured baseline.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/Import.hpp>
#include <faraday/Screener.hpp>
#include <faraday/Report.hpp>

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

// The Altium fab-output path (Tier-1 validation pipeline, ABT #420): classic
// RS-274X named by Protel extensions, standalone D03 flashes, fixed-format
// Excellon, and the IPC-D-356 netlist supplying nets + refdes + PINS. This
// is what every TI EVM/TIDA design zip contains.
TEST_CASE("gerber: an Altium-style set with IPC-D-356 gets exact nets and pins",
          "[gerber][ipc356]") {
    // top copper: two pads (standalone D03 after a D02 move) joined by a
    // track; a via ring at (30,10); FSAX44 fixed format, mm
    const char* gtl =
        "%FSAX44Y44*%\n%MOMM*%\n"
        "%ADD10C,1.0*%\n%ADD11C,0.6*%\n"
        "D10*\nX00100000Y00100000D02*\nD03*\n"
        "X00200000Y00100000D02*\nD03*\n"
        "X00100000Y00100000D02*\nX00200000Y00100000D01*\n"
        "D11*\nX00300000Y00100000D02*\nD03*\n"
        "M02*\n";
    // bottom: a plane pour region + the via ring
    const char* gbl =
        "%FSAX44Y44*%\n%MOMM*%\n%ADD11C,0.6*%\n"
        "G36*\nX00000000Y00000000D02*\nX00400000Y00000000D01*\n"
        "X00400000Y00200000D01*\nX00000000Y00200000D01*\n"
        "X00000000Y00000000D01*\nG37*\n"
        "D11*\nX00300000Y00100000D02*\nD03*\nM02*\n";
    // drill: fixed format 4:4 metric, the via hole
    const char* drl =
        "M48\n;FILE_FORMAT=4:4\nMETRIC\nT01C0.30\n%\nT01\n"
        "X00300000Y00100000\nM30\n";
    // IPC-D-356A: pads R7-1 (net SWX) and R7-2, via record on GND
    const char* ipc =
        "P  JOB\nP  UNITS CUST 1\nP  VER IPC-D-356A\n"
        "327SWX              R7    -1         A01X 010000Y 010000X0100Y0100R000 S0\n"
        "327SWX              R7    -2         A01X 020000Y 010000X0100Y0100R000 S0\n"
        "317GND              VIA   -          D0300PA00X 030000Y 010000X0060Y0060 S0\n"
        "999\n";
    std::vector<gerber::NamedFile> files{{"b.GTL", gtl},
                                         {"b.GBL", gbl},
                                         {"b.TXT", drl},
                                         {"b.ipc", ipc}};
    BoardIR b = import_board_set(files, builtin_stackup("default-2layer"));
    // pads carry component AND pin from the netlist
    REQUIRE(b.pads.size() == 2);
    CHECK(b.pads[0].component == "R7");
    CHECK(b.pads[0].pin == "1");
    CHECK(b.pads[1].pin == "2");
    CHECK(b.net_name(b.pads[0].net) == "SWX");
    // the track between the pads picked SWX up by connectivity
    int swx = b.pads[0].net;
    int netted = 0;
    for (const auto& s : b.segments)
        if (s.net == swx) ++netted;
    CHECK(netted >= 1);
    // the via and the plane pour both got GND through the 317 record
    REQUIRE(b.vias.size() == 1);
    CHECK(b.net_name(b.vias[0].net) == "GND");
    REQUIRE(!b.zones.empty());
    CHECK(b.net_name(b.zones[0].net) == "GND");
    CHECK(b.ipc_net_conflicts == 0);
    // netting reached every segment — no coverage-loss note
    CHECK(b.plausibility_notes.empty());
}

TEST_CASE("gerber: copper the netlist cannot reach is COUNTED, not silently lost",
          "[gerber][ipc356]") {
    // The Altium-style set above, plus a 30 mm orphan track nowhere near any
    // netlist record. It stays net-0 — and because every net-aware rule
    // (coupled-run, 3W, return-path, mesh derivation) skips net-0 copper,
    // that loss of screened surface must surface as a note, never silently.
    const char* gtl =
        "%FSAX44Y44*%\n%MOMM*%\n"
        "%ADD10C,1.0*%\n%ADD11C,0.6*%\n"
        "D10*\nX00100000Y00100000D02*\nD03*\n"
        "X00200000Y00100000D02*\nD03*\n"
        "X00100000Y00100000D02*\nX00200000Y00100000D01*\n"
        "X00100000Y00300000D02*\nX00400000Y00300000D01*\n"
        "D11*\nX00300000Y00100000D02*\nD03*\n"
        "M02*\n";
    const char* gbl =
        "%FSAX44Y44*%\n%MOMM*%\n%ADD11C,0.6*%\n"
        "G36*\nX00000000Y00000000D02*\nX00400000Y00000000D01*\n"
        "X00400000Y00200000D01*\nX00000000Y00200000D01*\n"
        "X00000000Y00000000D01*\nG37*\n"
        "D11*\nX00300000Y00100000D02*\nD03*\nM02*\n";
    const char* drl =
        "M48\n;FILE_FORMAT=4:4\nMETRIC\nT01C0.30\n%\nT01\n"
        "X00300000Y00100000\nM30\n";
    const char* ipc =
        "P  JOB\nP  UNITS CUST 1\nP  VER IPC-D-356A\n"
        "327SWX              R7    -1         A01X 010000Y 010000X0100Y0100R000 S0\n"
        "327SWX              R7    -2         A01X 020000Y 010000X0100Y0100R000 S0\n"
        "317GND              VIA   -          D0300PA00X 030000Y 010000X0060Y0060 S0\n"
        "999\n";
    std::vector<gerber::NamedFile> files{{"b.GTL", gtl},
                                         {"b.GBL", gbl},
                                         {"b.TXT", drl},
                                         {"b.ipc", ipc}};
    BoardIR b = import_board_set(files, builtin_stackup("default-2layer"));
    // the orphan is net-0: 10 of 40 routed mm reached = 25%
    REQUIRE(b.plausibility_notes.size() == 1);
    CHECK(b.plausibility_notes[0].find("netting reached 25%") != std::string::npos);
    CHECK(b.plausibility_notes[0].find("invisible") != std::string::npos);
}

TEST_CASE("gerber: the outline is the loop that holds the copper, not the "
          "drawing frame",
          "[gerber][ipc356]") {
    // Altium stamps the whole drawing sheet — frame, title block — into the
    // mechanical layer AND every copper gerber. The board is the SMALLEST
    // profile loop containing the pads and vias; copper outside it is the
    // drawing, cropped and counted. (LM5143's shipped gerbers: a 168x105 mm
    // "board" that is really 96x73, with 8.3 m of title-block strokes.)
    const char* gko =
        "%FSAX44Y44*%\n%MOMM*%\n%ADD10C,0.2*%\nD10*\n"
        // the board outline: (5,5)-(50,30)
        "X00050000Y00050000D02*\nX00500000Y00050000D01*\n"
        "X00500000Y00300000D01*\nX00050000Y00300000D01*\n"
        "X00050000Y00050000D01*\n"
        // the sheet frame: (1,1)-(99,79), also contains everything
        "X00010000Y00010000D02*\nX00990000Y00010000D01*\n"
        "X00990000Y00790000D01*\nX00010000Y00790000D01*\n"
        "X00010000Y00010000D01*\nM02*\n";
    const char* gtl =
        "%FSAX44Y44*%\n%MOMM*%\n"
        "%ADD10C,1.0*%\n%ADD11C,0.6*%\n"
        "D10*\nX00100000Y00100000D02*\nD03*\n"
        "X00200000Y00100000D02*\nD03*\n"
        "X00100000Y00100000D02*\nX00200000Y00100000D01*\n"
        // title-block text stroke at y=60: inside the frame, off the board
        "X00100000Y00600000D02*\nX00300000Y00600000D01*\n"
        "D11*\nX00300000Y00100000D02*\nD03*\n"
        "M02*\n";
    const char* gbl =
        "%FSAX44Y44*%\n%MOMM*%\n%ADD11C,0.6*%\n"
        "G36*\nX00060000Y00060000D02*\nX00400000Y00060000D01*\n"
        "X00400000Y00200000D01*\nX00060000Y00200000D01*\n"
        "X00060000Y00060000D01*\nG37*\n"
        "D11*\nX00300000Y00100000D02*\nD03*\nM02*\n";
    const char* drl =
        "M48\n;FILE_FORMAT=4:4\nMETRIC\nT01C0.30\n%\nT01\n"
        "X00300000Y00100000\nM30\n";
    const char* ipc =
        "P  JOB\nP  UNITS CUST 1\nP  VER IPC-D-356A\n"
        "327SWX              R7    -1         A01X 010000Y 010000X0100Y0100R000 S0\n"
        "327SWX              R7    -2         A01X 020000Y 010000X0100Y0100R000 S0\n"
        "317GND              VIA   -          D0300PA00X 030000Y 010000X0060Y0060 S0\n"
        "999\n";
    std::vector<gerber::NamedFile> files{{"b.GKO", gko},
                                         {"b.GTL", gtl},
                                         {"b.GBL", gbl},
                                         {"b.TXT", drl},
                                         {"b.ipc", ipc}};
    BoardIR b = import_board_set(files, builtin_stackup("default-2layer"));
    // bbox is the BOARD loop, not the sheet frame
    CHECK(b.bbox_from_outline);
    CHECK_THAT(b.bbox_x1, Catch::Matchers::WithinAbs(5.0, 0.2));
    CHECK_THAT(b.bbox_y1, Catch::Matchers::WithinAbs(5.0, 0.2));
    CHECK_THAT(b.bbox_x2, Catch::Matchers::WithinAbs(50.0, 0.2));
    CHECK_THAT(b.bbox_y2, Catch::Matchers::WithinAbs(30.0, 0.2));
    // the title stroke was cropped — and counted, never silently
    REQUIRE(b.plausibility_notes.size() == 1);
    CHECK(b.plausibility_notes[0].find("outside the board outline excluded") !=
          std::string::npos);
    // what remains is board copper, fully netted: no netting-loss note
    for (const auto& s : b.segments) CHECK(s.net > 0);
    CHECK(b.segments.size() == 1);
}
