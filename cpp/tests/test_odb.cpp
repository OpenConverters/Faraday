// ODB++ importer tests. The fixture is a REAL artifact: fixture_2layer
// exported by kicad-cli 9.0.7 (`pcb export odb --units mm`), so the anchor
// test is cross-format consistency — the same board through import_kicad and
// import_odb must agree on everything that carries electrical meaning.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/Import.hpp>
#include <faraday/Screener.hpp>

#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

using namespace faraday;
using Catch::Matchers::WithinAbs;

namespace {

std::string slurp(const std::string& rel) {
    std::ifstream in(std::string(FARADAY_FIXTURE_DIR) + "/" + rel, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// the electrically-relevant subset of the exported job, with tree paths
const char* ODB_FILES[] = {
    "matrix/matrix",
    "steps/pcb/profile",
    "steps/pcb/eda/data",
    "steps/pcb/layers/f.cu/features",
    "steps/pcb/layers/b.cu/features",
    "steps/pcb/layers/drill_plated_f.cu-b.cu/features",
    "steps/pcb/layers/comp_+_top/components",
};

std::vector<gerber::NamedFile> odb_set() {
    std::vector<gerber::NamedFile> out;
    for (const char* rel : ODB_FILES)
        out.push_back({rel, slurp(std::string("odb/") + rel)});
    return out;
}

std::map<std::string, double> routed_per_net(const BoardIR& b) {
    std::map<std::string, double> d;
    for (const auto& s : b.segments)
        d[b.net_name(s.net)] += std::hypot(s.x2 - s.x1, s.y2 - s.y1);
    return d;
}

}  // namespace

TEST_CASE("odb: the same board agrees with the kicad importer exactly",
          "[odb]") {
    BoardFormat fmt;
    BoardIR o = import_board_set(odb_set(), builtin_stackup("default-2layer"),
                                 &fmt);
    CHECK(fmt == BoardFormat::Odb);
    BoardIR k = import_kicad(slurp("fixture_2layer.kicad_pcb"),
                             builtin_stackup("default-2layer"));

    CHECK(o.segments.size() == k.segments.size());
    CHECK(o.vias.size() == k.vias.size());
    CHECK(o.zones.size() == k.zones.size());
    CHECK(o.pads.size() == k.pads.size());

    // identical per-net routed copper, to the micron
    auto ro = routed_per_net(o), rk = routed_per_net(k);
    REQUIRE(ro.size() == rk.size());
    for (const auto& [net, len] : rk) {
        INFO("net " << net);
        REQUIRE(ro.count(net) == 1);
        CHECK_THAT(ro[net], WithinAbs(len, 1e-3));
    }

    // same frame after the y-negation, no mirroring
    CHECK_THAT(o.bbox_x2 - o.bbox_x1, WithinAbs(k.bbox_x2 - k.bbox_x1, 1e-6));
    CHECK_THAT(o.bbox_y2 - o.bbox_y1, WithinAbs(k.bbox_y2 - k.bbox_y1, 1e-6));

    // the via: same net, full span, real drill
    REQUIRE(o.vias.size() == 1);
    CHECK(o.net_name(o.vias[0].net) == k.net_name(k.vias[0].net));
    CHECK(o.vias[0].cu_from == 0);
    CHECK(o.vias[0].cu_to == 1);
    CHECK_THAT(o.vias[0].drill, WithinAbs(k.vias[0].drill, 1e-6));

    // and both reach the same screening verdicts
    nlohmann::json rep_o = analyze_board(o), rep_k = analyze_board(k);
    CHECK(rep_o["findings"].size() == rep_k["findings"].size());
}

TEST_CASE("odb: toeprints carry their component and its VALUE", "[odb]") {
    BoardIR o = import_board_set(odb_set(), builtin_stackup("default-2layer"));
    int named = 0;
    for (const auto& p : o.pads)
        if (p.component == "R1") ++named;
    CHECK(named == 2);                     // both R1 pads owned via SNT TOP
    REQUIRE(o.components.size() == 1);
    CHECK(o.components[0].reference == "R1");
    // PRP Value survives — this is what makes the PDN tool work on ODB++
    CHECK(o.components[0].value == "100n");
}

TEST_CASE("odb: a job without eda/data is refused, with the reason named",
          "[odb]") {
    auto files = odb_set();
    files.erase(std::remove_if(files.begin(), files.end(),
                               [](const gerber::NamedFile& f) {
                                   return f.name == "steps/pcb/eda/data";
                               }),
                files.end());
    CHECK_THROWS_WITH(import_board_set(files, builtin_stackup("default-2layer")),
                      Catch::Matchers::ContainsSubstring("eda/data"));
}

TEST_CASE("odb: no stackup names the copper count", "[odb]") {
    CHECK_THROWS_WITH(import_board_set(odb_set()),
                      Catch::Matchers::ContainsSubstring("default-2layer"));
    // and carries it structurally, so the UI never has to regex it back out
    try {
        import_board_set(odb_set());
        FAIL("expected StackupNeeded");
    } catch (const StackupNeeded& e) {
        CHECK(e.copper_count == 2);
    }
}

// A job that declares no units at all is INCH — that is the format's default,
// and Altium's exporter writes no UNITS= line anywhere. Reading it as mm
// shrank a real 92 x 80 mm board to 3.6 x 3.1 mm and every derived number
// with it.
TEST_CASE("odb: a units declaration is read, and its absence means inch",
          "[odb]") {
    CHECK(odb::declared_unit("UNITS=MM\nF 1\n") == 1.0);
    CHECK(odb::declared_unit("UNITS=INCH\nF 1\n") == 25.4);
    // not always the first line — eda/data carries it under a comment header
    CHECK(odb::declared_unit("# generated\nHDR x\nUNITS=MM\n") == 1.0);
    CHECK(odb::declared_unit("#\n#Num Features\nF 1\n") == 0.0);   // undeclared
}

// Same fixture, same bytes, only the declaration removed: the board must come
// out 25.4x LARGER (it is now read as an inch job), and because a 50 x 30 mm
// board read as inches is 1270 x 762 mm, the plausibility gate must catch it
// and name the correction rather than screen a board the size of a door.
TEST_CASE("odb: an undeclared job is inches, and an impossible one is refused",
          "[odb]") {
    auto files = odb_set();
    int stripped = 0;
    for (auto& f : files)
        for (size_t at; (at = f.text.find("UNITS=")) != std::string::npos;) {
            if (at != 0 && f.text[at - 1] != '\n') break;   // not a line start
            f.text.erase(at, f.text.find('\n', at) + 1 - at);
            ++stripped;
        }
    REQUIRE(stripped >= 5);   // the fixture really did declare MM everywhere

    REQUIRE_THROWS_WITH(
        import_board_set(files, builtin_stackup("default-2layer")),
        Catch::Matchers::ContainsSubstring("1270") &&
            Catch::Matchers::ContainsSubstring("read as inches"));
}

TEST_CASE("odb: an unsupported copper symbol throws, never guesses a width",
          "[odb]") {
    auto files = odb_set();
    for (auto& f : files)
        if (f.name == "steps/pcb/layers/f.cu/features") {
            const size_t at = f.text.find("$0 r300.0");
            REQUIRE(at != std::string::npos);
            f.text.replace(at, 9, "$0 hexagon5");
        }
    CHECK_THROWS_WITH(import_board_set(files, builtin_stackup("default-2layer")),
                      Catch::Matchers::ContainsSubstring("hexagon5"));
}
