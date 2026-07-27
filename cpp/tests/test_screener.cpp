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

TEST_CASE("diff-pair name recognition", "[screener][diffpair]") {
    CHECK(is_differential_pair_name("USB_DP", "USB_DM"));
    CHECK(is_differential_pair_name("CAN_H", "CAN_L"));
    CHECK(is_differential_pair_name("LVDS0_P", "LVDS0_N"));
    CHECK(is_differential_pair_name("RX+", "RX-"));
    CHECK(is_differential_pair_name("clk_p", "clk_n"));  // case-insensitive
    // NOT pairs — these must keep producing real findings
    CHECK_FALSE(is_differential_pair_name("PWM_HS", "PWM_LS"));  // differs at [-2]
    CHECK_FALSE(is_differential_pair_name("SCL", "SDA"));
    CHECK_FALSE(is_differential_pair_name("CLK", "CLK"));
    CHECK_FALSE(is_differential_pair_name("D1", "D2"));
    CHECK_FALSE(is_differential_pair_name("A_P", "AB_N"));    // different length
    CHECK_FALSE(is_differential_pair_name("", ""));
}

TEST_CASE("screener: differential pair is info, not a defect, and gets no 3W",
          "[screener][diffpair]") {
    // two tightly coupled traces 0.15 mm apart — a 3W violation and a strong
    // coupled run if they were unrelated nets
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "USB_DP") (net 2 "USB_DM") (net 3 "G")
      (segment (start 5 10) (end 25 10) (width 0.2) (layer "F.Cu") (net 1))
      (segment (start 5 10.35) (end 25 10.35) (width 0.2) (layer "F.Cu") (net 2))
      (zone (net 3) (net_name "G") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    const auto* dp = find_rule(report["findings"], "diff-pair");
    REQUIRE(dp != nullptr);
    CHECK((*dp)["severityLabel"] == "info");
    CHECK((*dp)["confidence"] == "exact");
    CHECK((*dp)["nextDb"].get<double>() < 0.0);   // the number is still reported
    CHECK(find_rule(report["findings"], "3w") == nullptr);          // no defect
    CHECK(find_rule(report["findings"], "coupled-run") == nullptr); // reclassified
    CHECK(report["meta"]["diffPairsRecognized"].get<int>() == 1);
}

TEST_CASE("screener: hard break vs sparse reference — a farther plane counts",
          "[screener][planes]") {
    // 4 copper: F.Cu signal, In1/In2 planes, B.Cu signal.
    // In1 is void on the left half; In2 covers the whole board EXCEPT a strip
    // on the far left. So: x in [2,10] -> In1 void but In2 covers  = detour
    //                      x in [0,2]  -> neither covers            = hard break
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SIG") (net 2 "GND")
      (segment (start 0.5 10) (end 29 10) (width 0.2) (layer "F.Cu") (net 1))
      (zone (net 2) (net_name "GND") (layer "In1.Cu")
        (filled_polygon (layer "In1.Cu") (pts (xy 11 0) (xy 30 0) (xy 30 30) (xy 11 30))))
      (zone (net 2) (net_name "GND") (layer "In2.Cu")
        (filled_polygon (layer "In2.Cu") (pts (xy 2 0) (xy 30 0) (xy 30 30) (xy 2 30))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-4layer"));
    nlohmann::json report = analyze_board(b);

    // hard break only over the ~1.5 mm where NO plane covers
    const auto* hard = find_rule(report["findings"], "plane-crossing");
    REQUIRE(hard != nullptr);
    CHECK((*hard)["coupledLenMm"].get<double>() == Approx(1.5).margin(1.2));
    // the ~9 mm In1-void-but-In2-covers stretch is the aggregated detour
    const auto* sparse = find_rule(report["findings"], "sparse-reference");
    REQUIRE(sparse != nullptr);
    CHECK((*sparse)["coupledLenMm"].get<double>() == Approx(9.0).margin(1.5));
    CHECK((*sparse)["severityLabel"] == "medium");
    CHECK((*sparse)["title"].get<std::string>().find("Sparse reference") == 0);
}

TEST_CASE("screener: switch node found by connectivity; rails and LEDs are not",
          "[screener][switchnode]") {
    // SW: L1 + Q1, compact.  LEDNET: LED1 + Q2 (LED prefix must NOT match L).
    // VRAIL: L2 + Q3 but 14 pads -> a rail, not a switch node.
    std::string head = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SW") (net 2 "LEDNET") (net 3 "VRAIL") (net 4 "G")
      (segment (start 5 5) (end 15 5) (width 1.0) (layer "F.Cu") (net 1))
      (segment (start 5 9) (end 15 9) (width 0.3) (layer "F.Cu") (net 2))
      (segment (start 5 13) (end 15 13) (width 0.5) (layer "F.Cu") (net 3))
      (zone (net 4) (net_name "G") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
      (footprint "L" (layer "F.Cu") (at 5 5)
        (property "Reference" "L1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "SW")))
      (footprint "Q" (layer "F.Cu") (at 15 5)
        (property "Reference" "Q1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "SW")))
      (footprint "LED" (layer "F.Cu") (at 5 9)
        (property "Reference" "LED1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 2 "LEDNET")))
      (footprint "Q" (layer "F.Cu") (at 15 9)
        (property "Reference" "Q2")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 2 "LEDNET")))
      (footprint "L" (layer "F.Cu") (at 5 13)
        (property "Reference" "L2")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 3 "VRAIL")))
      (footprint "Q" (layer "F.Cu") (at 15 13)
        (property "Reference" "Q3")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 3 "VRAIL")))
    )";
    // 13 decoupling caps on VRAIL -> 15 pads total, over the sw_max_pads limit
    std::string caps;
    for (int i = 0; i < 13; ++i)
        caps += "(footprint \"C\" (layer \"F.Cu\") (at " + std::to_string(20 + i) +
                " 13) (property \"Reference\" \"C" + std::to_string(i) + "\")"
                " (pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\")"
                " (net 3 \"VRAIL\")))";
    BoardIR b = import_kicad(head + caps + ")", builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);

    const auto& sw = report["meta"]["switchNodes"];
    REQUIRE(sw.size() == 1);
    CHECK(sw[0] == "SW");                     // L1 + Q1, compact
    const auto* f = find_rule(report["findings"], "switch-node");
    REQUIRE(f != nullptr);
    CHECK((*f)["confidence"] == "heuristic");  // never claims certainty
    CHECK((*f)["coupledLenMm"].get<double>() > 0.0);  // copper extent, mm^2
}

TEST_CASE("screener: switch-node aggressor boosts coupled-run severity",
          "[screener][switchnode]") {
    auto board = [](bool with_switch) {
        std::string txt = R"((kicad_pcb
          (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
          (net 0 "") (net 1 "AGG") (net 2 "FB") (net 3 "G")
          (segment (start 5 10) (end 25 10) (width 0.3) (layer "F.Cu") (net 1))
          (segment (start 5 12) (end 25 12) (width 0.3) (layer "F.Cu") (net 2))
          (zone (net 3) (net_name "G") (layer "B.Cu")
            (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
          (footprint "L" (layer "F.Cu") (at 5 10)
            (property "Reference" "L1")
            (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "AGG")))
        )";
        if (with_switch)
            txt += R"((footprint "Q" (layer "F.Cu") (at 25 10)
                        (property "Reference" "Q1")
                        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "AGG"))))";
        return import_kicad(txt + ")", builtin_stackup("default-2layer"));
    };
    nlohmann::json plain = analyze_board(board(false));
    nlohmann::json boosted = analyze_board(board(true));
    const auto* a = find_rule(plain["findings"], "coupled-run");
    const auto* c = find_rule(boosted["findings"], "coupled-run");
    REQUIRE(a != nullptr);
    REQUIRE(c != nullptr);
    // identical geometry -> identical dB, but the SW-node case ranks higher
    CHECK((*a)["nextDb"].get<double>() == Approx((*c)["nextDb"].get<double>()));
    CHECK((*c)["severity"].get<double>() ==
          Approx((*a)["severity"].get<double>() + 0.15));
    CHECK((*c)["title"].get<std::string>().find("[SW aggressor]") != std::string::npos);
}

TEST_CASE("screener: zone-only routed nets are reported as a blind spot",
          "[screener][coverage]") {
    // NETPOLY is routed purely as a polygon — invisible to segment coupling
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SIG") (net 2 "NETPOLY") (net 3 "G")
      (segment (start 5 10) (end 25 10) (width 0.3) (layer "F.Cu") (net 1))
      (zone (net 2) (net_name "NETPOLY") (layer "F.Cu")
        (filled_polygon (layer "F.Cu") (pts (xy 5 12) (xy 25 12) (xy 25 14) (xy 5 14))))
      (zone (net 3) (net_name "G") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    const auto& po = report["meta"]["polygonOnlyNets"];
    REQUIRE(po.size() == 1);
    CHECK(po[0] == "NETPOLY");
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
