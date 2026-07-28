// IPC-2581 importer. No open-source reader for this format existed as of
// 2026 — only closed viewers — so this is the one Faraday owns outright.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <faraday/Import.hpp>
#include <faraday/IpcImporter.hpp>
#include <faraday/KicadImporter.hpp>
#include <faraday/Screener.hpp>

#include <cstring>
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

TEST_CASE("xml: parser handles the constructs IPC-2581 files use", "[xml]") {
    XmlNode n = parse_xml(R"(<?xml version="1.0"?>
        <!-- a comment -->
        <root a="1" b='two'>
          <child x="3.5"/>
          <ns:child xmlns:ns="urn:x" x="4.5">text &amp; more</ns:child>
          <![CDATA[raw <not markup>]]>
        </root>)");
    CHECK(n.name == "root");
    CHECK(n.attr("a") == "1");
    CHECK(n.attr("b") == "two");
    auto kids = n.all("child");                 // namespace prefix stripped
    REQUIRE(kids.size() == 2);
    CHECK(kids[0]->num("x") == Approx(3.5));
    CHECK(kids[1]->text == "text & more");
    CHECK(n.text.find("raw <not markup>") != std::string::npos);
    CHECK(n.first("missing") == nullptr);
    CHECK_THROWS_AS(n.attr("nope"), XmlError);
}

TEST_CASE("xml: malformed input throws rather than half-parsing", "[xml]") {
    CHECK_THROWS_AS(parse_xml("<a><b></a>"), XmlError);       // mismatched close
    CHECK_THROWS_AS(parse_xml("<a>"), XmlError);              // unclosed
    CHECK_THROWS_AS(parse_xml("<a b=unquoted/>"), XmlError);  // bare value
    CHECK_THROWS_AS(parse_xml("not xml at all"), XmlError);
}

TEST_CASE("ipc2581: 4-layer board imports fully", "[ipc]") {
    BoardIR b = import_ipc2581(read_fixture("fixture_4layer.xml"));

    REQUIRE(b.copper_names.size() == 4);
    CHECK(b.copper_names[0] == "TOP");
    CHECK(b.copper_names[1] == "INNER1");
    CHECK(b.copper_names[3] == "BOTTOM");
    CHECK(b.stackup.source == "board-file");
    // layerFunction="PLANE" carries through as the power hint
    auto cu = b.stackup.copper_indices();
    CHECK(b.stackup.layers[cu[1]].copper_type == "power");
    CHECK(b.stackup.layers[cu[0]].copper_type == "signal");
    // permittivity arrives via <Attribute>, the only place exporters put it
    double h, eps;
    b.stackup.dielectric_between(0, 1, h, eps);
    CHECK(h == Approx(0.2));
    CHECK(eps == Approx(4.4));

    // width resolved through the LineDesc dictionary
    REQUIRE(b.segments.size() == 2);
    CHECK(b.segments[0].width == Approx(0.3));
    CHECK(b.segments[0].cu == 0);
    CHECK(b.segments[0].x1 == Approx(5.0));

    REQUIRE(b.zones.size() == 1);
    CHECK(b.zones[0].cu == 1);
    CHECK(std::abs(b.zones[0].signed_area()) == Approx(48.0 * 28.0));

    CHECK(b.nets.size() == 3);
    CHECK(b.components.size() == 2);
    CHECK(b.pads.size() == 3);
    CHECK(b.pads[0].component == "U1");
    CHECK(b.bbox_from_outline);
    CHECK(b.bbox_x2 == Approx(50.0));
}

TEST_CASE("ipc2581: units are honoured, never assumed", "[ipc]") {
    auto with_units = [](const char* u) {
        std::string t = read_fixture("fixture_4layer.xml");
        size_t p = t.find("units=\"MILLIMETER\"");
        REQUIRE(p != std::string::npos);
        return t.replace(p, strlen("units=\"MILLIMETER\""),
                         std::string("units=\"") + u + "\"");
    };
    BoardIR inch = import_ipc2581(with_units("INCH"));
    CHECK(inch.bbox_x2 == Approx(50.0 * 25.4));
    CHECK(inch.segments[0].width == Approx(0.3 * 25.4));

    // no units at all, or an unknown one, is a refusal
    std::string t = read_fixture("fixture_4layer.xml");
    size_t p = t.find(" units=\"MILLIMETER\"");
    CHECK_THROWS_WITH(import_ipc2581(t.replace(p, strlen(" units=\"MILLIMETER\""), "")),
                      Catch::Matchers::ContainsSubstring("no units="));
    CHECK_THROWS_WITH(import_ipc2581(with_units("FURLONGS")),
                      Catch::Matchers::ContainsSubstring("unknown units"));
}

TEST_CASE("ipc2581: a dielectric with no permittivity is refused, not guessed",
          "[ipc]") {
    std::string t = read_fixture("fixture_4layer.xml");
    // strip every permittivity attribute
    for (size_t p = t.find("<Attribute name=\"DIELECTRIC_CONSTANT\"");
         p != std::string::npos;
         p = t.find("<Attribute name=\"DIELECTRIC_CONSTANT\"")) {
        size_t e = t.find("/>", p);
        t.erase(p, e + 2 - p);
    }
    CHECK_THROWS_WITH(import_ipc2581(t),
                      Catch::Matchers::ContainsSubstring("no permittivity"));
    // ...but an explicit user stackup unblocks the same file
    BoardIR b = import_ipc2581(t, builtin_stackup("default-4layer"));
    CHECK(b.stackup.source == "user:default-4layer");
    CHECK(b.copper_names.size() == 4);
}

TEST_CASE("ipc2581: a board screens like any other", "[ipc]") {
    BoardIR b = import_ipc2581(read_fixture("fixture_4layer.xml"));
    nlohmann::json report = analyze_board(b);
    // CLK/DATA: 40 mm at 0.5 mm spacing over the plane 0.2 mm below ->
    // k = 0.25/(1+(0.5/0.2)^2) = 0.0345 -> -29.2 dB, matching the .HYP fixture
    const nlohmann::json* cr = nullptr;
    for (const auto& f : report["findings"])
        if (f["rule"] == "coupled-run") { cr = &f; break; }
    REQUIRE(cr != nullptr);
    CHECK((*cr)["nextDb"].get<double>() == Approx(-29.2).margin(0.3));
    CHECK((*cr)["coupledLenMm"].get<double>() == Approx(40.0).margin(0.5));
    CHECK(report["meta"]["planes"][1]["isPlane"] == true);
}

TEST_CASE("ipc2581: wrong root element is rejected", "[ipc]") {
    CHECK_THROWS_WITH(import_ipc2581("<odb><x/></odb>"),
                      Catch::Matchers::ContainsSubstring("expected <IPC-2581>"));
}

TEST_CASE("import: format is detected from CONTENT, not the file name",
          "[import]") {
    CHECK(detect_format(read_fixture("fixture_4layer.xml")) == BoardFormat::Ipc2581);
    CHECK(detect_format(read_fixture("fixture_4layer.hyp")) == BoardFormat::Hyp);
    CHECK(detect_format(read_fixture("fixture_2layer.kicad_pcb")) == BoardFormat::Kicad);
    // a lone Gerber layer is now RECOGNISED — and told it needs the full set
    CHECK_THROWS_WITH(detect_format("G04 this is a gerber*\n"),
                      Catch::Matchers::ContainsSubstring("fabrication set"));
    // and the one-call entry point routes each to the right importer
    BoardFormat f;
    BoardIR b = import_board(read_fixture("fixture_4layer.hyp"), std::nullopt, &f);
    CHECK(f == BoardFormat::Hyp);
    CHECK(b.copper_names.size() == 4);
}
