// A bill of materials needs no stackup. ImportPurpose::ComponentsOnly imports
// the parts and their pads from a board that carries no dielectric — where a
// screening import is (rightly) refused with StackupNeeded — and must produce
// EXACTLY the parts a screening import with a stackup produces: the parts on
// a board do not depend on what it is laminated from. And a board imported
// that way must never be screened.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/Import.hpp>
#include <faraday/Report.hpp>
#include <faraday/Screener.hpp>

#include <fstream>
#include <sstream>

using namespace faraday;

namespace {

std::string fixture(const std::string& rel) {
    std::ifstream in(std::string(FARADAY_FIXTURE_DIR) + "/" + rel, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// fixture_2layer with its (setup (stackup ...)) removed: a real-shaped KiCad
// board that, like most hobby boards, states no dielectric.
std::string kicad_without_stackup() {
    std::string t = fixture("fixture_2layer.kicad_pcb");
    const size_t at = t.find("(stackup");
    REQUIRE(at != std::string::npos);
    int depth = 0;
    size_t end = at;
    for (; end < t.size(); ++end) {
        if (t[end] == '(') ++depth;
        else if (t[end] == ')' && --depth == 0) break;
    }
    REQUIRE(depth == 0);
    t.erase(at, end + 1 - at);
    REQUIRE(t.find("(stackup") == std::string::npos);
    return t;
}

std::vector<gerber::NamedFile> odb_set() {
    static const char* files[] = {
        "matrix/matrix",
        "steps/pcb/profile",
        "steps/pcb/eda/data",
        "steps/pcb/layers/f.cu/features",
        "steps/pcb/layers/b.cu/features",
        "steps/pcb/layers/drill_plated_f.cu-b.cu/features",
        "steps/pcb/layers/comp_+_top/components",
    };
    std::vector<gerber::NamedFile> out;
    for (const char* rel : files) out.push_back({rel, fixture(std::string("odb/") + rel)});
    return out;
}

// The parts and pads, compared through the very serialisation the BOM is
// written with — so "the same" means the same bytes reach the consumer.
void same_parts(const BoardIR& parts_only, const BoardIR& screened) {
    CHECK(components_json(parts_only) == components_json(screened));
    CHECK(pads_json(parts_only) == pads_json(screened));
    CHECK(parts_only.copper_names == screened.copper_names);
}

}  // namespace

TEST_CASE("components-only: a kicad board with no stackup still lists its parts",
          "[components-only]") {
    const std::string txt = kicad_without_stackup();
    // the screen still refuses, exactly as before
    CHECK_THROWS_AS(import_board_set({{"b.kicad_pcb", txt}}), StackupNeeded);

    BoardIR parts = import_board_set({{"b.kicad_pcb", txt}}, std::nullopt, nullptr, {},
                                     ImportPurpose::ComponentsOnly);
    CHECK(parts.components_only);
    CHECK(parts.stackup.layers.empty());
    CHECK(parts.stackup.source.rfind("none", 0) == 0);
    REQUIRE(parts.components.size() == 1);
    CHECK(parts.components[0].reference == "R1");
    CHECK(parts.components[0].value == "100n");

    same_parts(parts, import_board_set({{"b.kicad_pcb", txt}},
                                       builtin_stackup("default-2layer")));
}

TEST_CASE("components-only: a stackup in the file is not even read",
          "[components-only]") {
    // A dielectric with no epsilon_r is refused by the stackup parser — it
    // is a number Faraday will not invent. A parts list never reads the
    // dielectric, so it must not fail on one.
    std::string txt = fixture("fixture_2layer.kicad_pcb");
    const std::string eps = "(epsilon_r 4.5)";
    const size_t at = txt.find(eps);
    REQUIRE(at != std::string::npos);
    txt.erase(at, eps.size());
    CHECK_THROWS_WITH(import_kicad(txt), Catch::Matchers::ContainsSubstring("epsilon_r"));
    BoardIR parts = import_kicad(txt, std::nullopt, ImportPurpose::ComponentsOnly);
    CHECK(parts.components.size() == 1);
}

TEST_CASE("components-only: an ODB++ job lists its parts without a stackup",
          "[components-only]") {
    CHECK_THROWS_AS(import_board_set(odb_set()), StackupNeeded);
    BoardFormat fmt;
    BoardIR parts = import_board_set(odb_set(), std::nullopt, &fmt, {},
                                     ImportPurpose::ComponentsOnly);
    CHECK(fmt == BoardFormat::Odb);
    CHECK(parts.components_only);
    REQUIRE(parts.components.size() == 1);
    CHECK(parts.components[0].reference == "R1");
    same_parts(parts, import_board_set(odb_set(), builtin_stackup("default-2layer")));
}

TEST_CASE("components-only: IPC-2581 lists its parts without reading the dielectric",
          "[components-only]") {
    const std::string txt = fixture("fixture_4layer.xml");
    BoardIR parts = import_ipc2581(txt, std::nullopt, ImportPurpose::ComponentsOnly);
    CHECK(parts.components_only);
    CHECK(parts.stackup.layers.empty());
    REQUIRE(parts.components.size() == 2);
    same_parts(parts, import_ipc2581(txt));
}

TEST_CASE("components-only: a supplied stackup is still used, and screens",
          "[components-only]") {
    BoardIR b = import_board_set({{"b.kicad_pcb", kicad_without_stackup()}},
                                 builtin_stackup("default-2layer"), nullptr, {},
                                 ImportPurpose::ComponentsOnly);
    CHECK_FALSE(b.components_only);
    CHECK(b.stackup.source == "user:default-2layer");
    CHECK_NOTHROW(analyze_board(b));
}

TEST_CASE("components-only: a board imported for its parts is never screened",
          "[components-only]") {
    BoardIR parts = import_board_set({{"b.kicad_pcb", kicad_without_stackup()}},
                                     std::nullopt, nullptr, {},
                                     ImportPurpose::ComponentsOnly);
    CHECK_THROWS_WITH(Screener(parts), Catch::Matchers::ContainsSubstring("components only"));
    CHECK_THROWS_WITH(analyze_board(parts),
                      Catch::Matchers::ContainsSubstring("components only"));
}
