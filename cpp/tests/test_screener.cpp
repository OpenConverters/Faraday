#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <faraday/KicadImporter.hpp>
#include <faraday/Screener.hpp>

#include <fstream>
#include <sstream>

using Catch::Approx;
using namespace faraday;

static BoardIR fixture_board() {
    std::ifstream in(std::string(FARADAY_FIXTURE_DIR) + "/fixture_2layer.kicad_pcb");
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return import_kicad(ss.str());
}

static const nlohmann::json* find_rule(const nlohmann::json& findings,
                                       const std::string& rule) {
    for (const auto& f : findings)
        if (f["rule"] == rule) return &f;
    return nullptr;
}

TEST_CASE("screener: plane classification on the fixture", "[screener]") {
    Screener sc(fixture_board());
    const auto& lm = sc.layer_models();
    REQUIRE(lm.size() == 2);
    CHECK_FALSE(lm[0].is_plane);
    CHECK(lm[1].is_plane);           // B.Cu: 96% GND pour
    CHECK(lm[1].plane_net == 1);
    CHECK(lm[1].zone_coverage == Approx(0.96));
    CHECK(lm[0].ref_dn == 1);        // F.Cu references B.Cu through the core
    CHECK(lm[0].h_dn == Approx(1.51));
    CHECK(lm[0].eps_dn == Approx(4.5));
    CHECK(lm[0].ref_up == -1);
}

TEST_CASE("screener: fixture findings — coupled run, 3W, plane crossings", "[screener]") {
    nlohmann::json report = analyze_board(fixture_board());
    const auto& findings = report["findings"];

    // coupled run CLK<->DATA: d_cc = 0.5 mm, h = 1.51 mm
    // k = 0.25 / (1 + (0.5/1.51)^2) = 0.2253 -> -12.9 dB, 40 mm run
    const auto* cr = find_rule(findings, "coupled-run");
    REQUIRE(cr != nullptr);
    CHECK((*cr)["netA"] == 2);
    CHECK((*cr)["netB"] == 3);
    CHECK((*cr)["nextDb"].get<double>() == Approx(-12.9).margin(0.1));
    CHECK((*cr)["coupledLenMm"].get<double>() == Approx(40.0).margin(0.5));
    CHECK((*cr)["confidence"] == "screening-estimate");
    CHECK((*cr)["severityLabel"] == "high");
    CHECK((*cr)["geom"]["lines"].size() == 2);  // both spans for the overlay

    // 3W: edge separation 0.2 mm < 2 x 0.3 mm
    const auto* w3 = find_rule(findings, "3w");
    REQUIRE(w3 != nullptr);
    CHECK((*w3)["minSepMm"].get<double>() == Approx(0.2).margin(1e-6));

    // both CLK and DATA cross the 2 mm plane split -> ~3 mm sampled gap each
    int crossings = 0;
    for (const auto& f : findings)
        if (f["rule"] == "plane-crossing") {
            ++crossings;
            CHECK(f["coupledLenMm"].get<double>() == Approx(3.0).margin(1.5));
            CHECK(f["geom"]["markers"].size() >= 2);
        }
    CHECK(crossings == 2);

    // the GND trace couples to nothing (pour net) and no no-reference-plane
    // finding exists (B.Cu is a valid reference)
    CHECK(find_rule(findings, "no-reference-plane") == nullptr);

    // ranking: severities are non-increasing
    double prev = 2.0;
    for (const auto& f : findings) {
        CHECK(f["severity"].get<double>() <= prev + 1e-12);
        prev = f["severity"].get<double>();
    }

    // Z0 table carries the microstrip estimate for the 0.3 mm trace
    bool found_z0 = false;
    for (const auto& e : report["z0Table"])
        if (e["cu"] == 0 && e["widthMm"].get<double>() == Approx(0.3)) {
            found_z0 = true;
            CHECK(e["z0Ohm"].get<double>() == Approx(127.7).margin(1.5));
        }
    CHECK(found_z0);

    // nothing silently dropped on this small board
    CHECK(report["meta"]["droppedBelowFloorDb"].get<int>() == 0);
    CHECK(report["meta"]["droppedByFindingCap"].get<int>() == 0);
}

TEST_CASE("screener: board without any plane — loud, geometric-only", "[screener]") {
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "A") (net 2 "B")
      (segment (start 5 10) (end 25 10) (width 0.3) (layer "F.Cu") (net 1))
      (segment (start 5 10.5) (end 25 10.5) (width 0.3) (layer "F.Cu") (net 2))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    const auto& findings = report["findings"];

    const auto* nrp = find_rule(findings, "no-reference-plane");
    REQUIRE(nrp != nullptr);
    CHECK((*nrp)["severityLabel"] == "high");

    const auto* cr = find_rule(findings, "coupled-run");
    REQUIRE(cr != nullptr);
    CHECK((*cr)["confidence"] == "geometric-only");
    CHECK_FALSE(cr->contains("nextDb"));  // no dB claim without a reference plane

    const auto* w3 = find_rule(findings, "3w");
    REQUIRE(w3 != nullptr);  // 3W is pure geometry, still checked
}

TEST_CASE("screener: far-apart traces produce no coupled-run finding", "[screener]") {
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "A") (net 2 "B") (net 3 "GNDPOUR")
      (segment (start 5 5) (end 25 5) (width 0.3) (layer "F.Cu") (net 1))
      (segment (start 5 25) (end 25 25) (width 0.3) (layer "F.Cu") (net 2))
      (zone (net 3) (net_name "GNDPOUR") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    // 20 mm apart with h = 1.51 mm: far outside the 4h screening radius
    CHECK(find_rule(report["findings"], "coupled-run") == nullptr);
    CHECK(find_rule(report["findings"], "3w") == nullptr);
}

TEST_CASE("screener: orthogonal crossing is not a coupled run", "[screener]") {
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "A") (net 2 "B") (net 3 "G")
      (segment (start 5 10) (end 25 10) (width 0.3) (layer "F.Cu") (net 1))
      (segment (start 15 2) (end 15 28) (width 0.3) (layer "F.Cu") (net 2))
      (zone (net 3) (net_name "G") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    CHECK(find_rule(report["findings"], "coupled-run") == nullptr);
}

TEST_CASE("screener: broadside coupling across adjacent signal layers", "[screener]") {
    // 4-layer with NO pours: In1/In2 both signal; stacked runs on F.Cu/In1.Cu
    // separated by the 0.2 mm prepreg -> saturated broadside coupling.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "A") (net 2 "B")
      (segment (start 5 10) (end 35 10) (width 0.2) (layer "F.Cu") (net 1))
      (segment (start 5 10) (end 35 10) (width 0.2) (layer "In1.Cu") (net 2))
      (segment (start 5 20) (end 35 20) (width 0.2) (layer "In2.Cu") (net 0))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-4layer"));
    nlohmann::json report = analyze_board(b);
    const auto* cr = find_rule(report["findings"], "coupled-run");
    REQUIRE(cr != nullptr);
    CHECK((*cr)["cuA"] != (*cr)["cuB"]);  // cross-layer
    // zero lateral offset -> saturated bound 0.25 -> -12.04 dB
    CHECK((*cr)["nextDb"].get<double>() == Approx(-12.04).margin(0.05));
    CHECK((*cr)["coupledLenMm"].get<double>() == Approx(30.0).margin(0.5));
}
