// The parasitic-annotated netlist (ABT #805): what the copper contributes,
// written where a simulator can read it. What is pinned here is that every
// number in the deck is the SAME number the rest of the tool reports — an
// export that quietly disagrees with the panel is worse than no export — and
// that the file states its own absences.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <faraday/KicadImporter.hpp>
#include <faraday/Report.hpp>
#include <faraday/SpiceExport.hpp>

#include <fstream>
#include <sstream>

using Catch::Approx;
using namespace faraday;

static std::string read_fixture(const char* name) {
    std::ifstream in(std::string(FARADAY_FIXTURE_DIR) + "/real/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::vector<std::string> subckt_ports(const std::string& cir) {
    std::istringstream is(cir);
    std::string line;
    while (std::getline(is, line)) {
        if (line.rfind(".subckt", 0) != 0) continue;
        std::istringstream ls(line);
        std::string tok;
        std::vector<std::string> out;
        ls >> tok >> tok;                      // ".subckt", the name
        while (ls >> tok) out.push_back(tok);
        return out;
    }
    return {};
}

TEST_CASE("the deck carries the same numbers the tool reports", "[spice]") {
    BoardIR b = import_kicad(read_fixture("mppt-2420-hc.kicad_pcb"),
                             builtin_stackup("default-4layer"));
    Screener sc(b);
    spice::ExportOptions o;
    o.chassis_gap_mm = 8.0;
    const spice::Deck d = spice::build(b, sc, o);

    // the mesh inductance is the commutation-loop finding's own figure
    const nlohmann::json rep = analyze_board(b);
    double finding_nh = 0;
    for (const auto& f : rep["findings"])
        if (f["rule"] == "commutation-loop" && f.contains("emit"))
            finding_nh = f["emit"]["loopNh"].get<double>();
    REQUIRE(finding_nh > 0);
    double deck_nh = 0;
    for (const auto& e : d.manifest["entries"])
        if (e["name"] == "L_MESH") deck_nh = e["value"].get<double>();
    CHECK(deck_nh == Approx(finding_nh));
    CHECK(d.cir.find("L_MESH") != std::string::npos);

    // the input branch is op::input_branch, part for part
    auto ib = op::input_branch(b, sc);
    REQUIRE(ib.has_value());
    for (const auto& c : ib->caps) {
        CHECK(d.cir.find("C_" + c.ref + " ") != std::string::npos);
        bool found_c = false, found_l = false;
        for (const auto& e : d.manifest["entries"]) {
            if (e["name"] == "C_" + c.ref) {
                found_c = true;
                CHECK(e["value"].get<double>() == Approx(c.c_f));
                CHECK(e["source"] == "measured");
            }
            if (e["name"] == "L_" + c.ref) {
                found_l = true;
                CHECK(e["value"].get<double>() ==
                      Approx((c.esl_h + c.l_mount_h) * 1e9));
                CHECK(e["source"] == "derived");
            }
        }
        CHECK(found_c);
        CHECK(found_l);
    }

    // C_stray is emc::chassis_stray_c_f of the screener's own dv/dt area
    const double area = sc.dvdt_copper()["totalMm2"].get<double>();
    for (const auto& e : d.manifest["entries"])
        if (e["name"] == "C_STRAY")
            CHECK(e["value"].get<double>() ==
                  Approx(emc::chassis_stray_c_f(area, 8.0)));
    CHECK(d.cir.find("C_STRAY") != std::string::npos);
}

TEST_CASE("the manifest's ports are exactly the .subckt line's", "[spice]") {
    // These drifted apart once — the CELL port was added to the deck and not
    // to the manifest, and a harness built from the manifest wired the
    // subcircuit wrong while still parsing cleanly. Nothing but this catches
    // that: SPICE nodes are positional, and wrong ones are still legal.
    BoardIR b = import_kicad(read_fixture("mppt-2420-hc.kicad_pcb"),
                             builtin_stackup("default-4layer"));
    Screener sc(b);
    const spice::Deck d = spice::build(b, sc, {});
    const auto ports = subckt_ports(d.cir);
    REQUIRE(ports.size() == 5);
    const auto& m = d.manifest["ports"];
    REQUIRE(m.size() == ports.size());
    for (size_t i = 0; i < ports.size(); ++i)
        CHECK(m[i].get<std::string>() == ports[i]);
    // and the capacitor bank hangs off the ARRIVING rail, not past the mesh:
    // the whole point of the mesh element is that the switching cell sees the
    // capacitors through it
    CHECK(d.cir.find("L_MESH " + ports[0] + " " + ports[1]) != std::string::npos);
    CHECK(d.cir.find("C_C1 " + ports[0] + " ") != std::string::npos);
}

TEST_CASE("the deck names what it does not contain", "[spice]") {
    BoardIR b = import_kicad(read_fixture("mppt-2420-hc.kicad_pcb"),
                             builtin_stackup("default-4layer"));
    Screener sc(b);
    const spice::Deck d = spice::build(b, sc, {});
    for (const char* absent : {"device models", "gate loop", "control loop"})
        CHECK(d.cir.find(absent) != std::string::npos);
    CHECK(d.manifest["absent"].size() >= 4);
    // every value states where it came from, and from a closed set
    REQUIRE(!d.manifest["entries"].empty());
    for (const auto& e : d.manifest["entries"]) {
        const std::string src = e["source"];
        CHECK((src == "measured" || src == "derived" || src == "stated" ||
               src == "default"));
        CHECK(!e["how"].get<std::string>().empty());
    }
    // ESR is the one constant in the branch, and it says so
    bool esr_default = false;
    for (const auto& e : d.manifest["entries"])
        if (e["quantity"] == "resistance" && e["source"] == "default")
            esr_default = true;
    CHECK(esr_default);
}

TEST_CASE("no chassis gap means no common-mode element, and a warning",
          "[spice]") {
    BoardIR b = import_kicad(read_fixture("mppt-2420-hc.kicad_pcb"),
                             builtin_stackup("default-4layer"));
    Screener sc(b);
    const spice::Deck d = spice::build(b, sc, {});     // no gap stated
    CHECK(d.cir.find("C_STRAY ") == std::string::npos);
    CHECK(d.cir.find("NO COMMON-MODE SOURCE ELEMENT") != std::string::npos);
    REQUIRE(d.manifest["warnings"].size() >= 1);
    // the area it WOULD have used is still reported — the measurement exists,
    // only the mounting distance is missing
    bool has_area = false;
    for (const auto& e : d.manifest["entries"])
        if (e["name"] == "dvdt_copper_area") {
            has_area = true;
            CHECK(e["value"].get<double>() > 0);
        }
    CHECK(has_area);
}

TEST_CASE("a board with no switch node refuses instead of emitting an empty deck",
          "[spice]") {
    BoardIR b = import_kicad(read_fixture("hackrf-one.kicad_pcb"));
    Screener sc(b);
    if (sc.switch_nets().empty())
        CHECK_THROWS_AS(spice::build(b, sc, {}), std::invalid_argument);
}

TEST_CASE("net names become legal SPICE tokens, reversibly stated", "[spice]") {
    CHECK(spice::spice_token("/DC/DC/SW_NODE") == "DC_DC_SW_NODE");
    CHECK(spice::spice_token("/DCDC_HV+") == "DCDC_HV");
    CHECK(spice::spice_token("Net-(C20-Pad2)") == "NET_C20_PAD2");
    CHECK(spice::spice_token("3V3") == "N3V3");      // must not start with a digit
    CHECK(spice::spice_token("") == "N");
    // the original name survives in the manifest, so a reader can map back
    BoardIR b = import_kicad(read_fixture("mppt-2420-hc.kicad_pcb"),
                             builtin_stackup("default-4layer"));
    Screener sc(b);
    const spice::Deck d = spice::build(b, sc, {});
    CHECK(d.manifest["nets"]["switch"] == "/DC/DC/SW_NODE");
    CHECK(d.manifest["nets"]["rail"] == "/DCDC_HV+");
}
