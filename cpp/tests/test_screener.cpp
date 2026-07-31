#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <faraday/KicadImporter.hpp>
#include <faraday/Diff.hpp>
#include <faraday/Fixes.hpp>
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

// IEC/DIN designators. A European schematic names transistors T*, ICs IC*
// and test points TP* — the TPS23754 PoE flyback that exposed this has T1..T7
// and NOT ONE "Q", so the ANSI-only prefix left it with zero switch nodes and
// a permanently greyed-out near-field button. Two things are pinned here:
// the per-board convention call, and the isolated-converter shape (FET drain
// + transformer primary + RCD snubber) that neither buck_like nor bridge_like
// can see, because the net has no inductor and no C/L/P/J power path.
namespace {
// n pads of one component, all on `net`, laid out along a row at y
std::string iec_part(const std::string& ref, int n, double x, double y,
                     int net, const std::string& net_name) {
    std::string s = "(footprint \"F\" (layer \"F.Cu\") (at " +
                    std::to_string(x) + " " + std::to_string(y) +
                    ") (property \"Reference\" \"" + ref + "\")";
    for (int i = 0; i < n; ++i)
        s += " (pad \"" + std::to_string(i + 1) +
             "\" smd rect (at " + std::to_string(i * 0.5) +
             " 0) (size 0.4 0.4) (layers \"F.Cu\") (net " +
             std::to_string(net) + " \"" + net_name + "\"))";
    return s + ")";
}
}  // namespace

TEST_CASE("screener: an IEC-designator flyback is a switch node, not silence",
          "[screener][switchnode]") {
    std::string b = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "DRAIN") (net 2 "G")
      (segment (start 5 5) (end 15 5) (width 1.0) (layer "F.Cu") (net 1))
      (zone (net 2) (net_name "G") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
    )";
    b += iec_part("T7", 9, 15, 5, 1, "DRAIN");    // SI7852DP, PowerPAK SO-8
    b += iec_part("T2", 10, 5, 5, 1, "DRAIN");    // flyback transformer
    b += iec_part("D17", 3, 10, 3, 1, "DRAIN");   // RCD snubber diode
    b += iec_part("R29", 2, 10, 7, 1, "DRAIN");   // RCD snubber resistor
    b += iec_part("T5", 3, 12, 7, 1, "DRAIN");    // SOT-23 gate helper
    b += iec_part("IC4", 21, 25, 20, 0, "");      // the IEC tell: IC, not U
    b += iec_part("TP9", 1, 27, 20, 0, "");       // test points have their own
    BoardIR board = import_kicad(b + ")", builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(board);

    const auto& sw = report["meta"]["switchNodes"];
    REQUIRE(sw.size() == 1);
    CHECK(sw[0] == "DRAIN");
}

TEST_CASE("screener: on an ANSI board T stays a test point", "[screener][switchnode]") {
    // The same shape, but the board also carries a "Q" — which is what HackRF
    // One does, where T1..T4 are TEST POINTS. Any Q at all means the board is
    // named the ANSI way and T must not be read as a transistor.
    std::string b = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "PROBE") (net 2 "G")
      (segment (start 5 5) (end 15 5) (width 1.0) (layer "F.Cu") (net 1))
      (zone (net 2) (net_name "G") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
    )";
    b += iec_part("T7", 9, 15, 5, 1, "PROBE");
    b += iec_part("T2", 10, 5, 5, 1, "PROBE");
    b += iec_part("D17", 3, 10, 3, 1, "PROBE");
    b += iec_part("R29", 2, 10, 7, 1, "PROBE");
    b += iec_part("Q1", 3, 25, 20, 0, "");        // the ANSI tell
    b += iec_part("IC4", 21, 27, 20, 0, "");      // present but outvoted by Q
    BoardIR board = import_kicad(b + ")", builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(board);

    CHECK(report["meta"]["switchNodes"].empty());
}

TEST_CASE("screener: multi-pad power FETs do not hide the switch node",
          "[screener][switchnode]") {
    // mppt-2420-lc in miniature: the node has only 6 COMPONENTS but the FETs
    // are multi-pad power packages, so a pad count crosses 12 and the old
    // discriminator silently rejected the true switch node of a 20 A buck.
    auto fet = [](const std::string& ref, double x, const std::string& n1,
                  const std::string& other, int id1, int id2) {
        std::string f = "(footprint \"Q\" (layer \"F.Cu\") (at " +
                        std::to_string(x) + " 10) (property \"Reference\" \"" +
                        ref + "\")";
        for (int k = 0; k < 4; ++k)   // 4 drain pads, fused
            f += " (pad \"D" + std::to_string(k) + "\" smd rect (at " +
                 std::to_string(k * 1.5) + " 0) (size 1 1) (layers \"F.Cu\")"
                 " (net " + std::to_string(id1) + " \"" + n1 + "\"))";
        f += " (pad \"S\" smd rect (at 0 2) (size 1 1) (layers \"F.Cu\")"
             " (net " + std::to_string(id2) + " \"" + other + "\"))";
        return f + ")";
    };
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SW") (net 2 "VIN") (net 3 "GND")
      (footprint "L" (layer "F.Cu") (at 30 10)
        (property "Reference" "L1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "SW")))
    )";
    // NB the )" above terminates the raw string — the board's own closer is
    // appended after the FETs, as the VRAIL test does
    BoardIR b = import_kicad(txt + fet("Q1", 10, "SW", "VIN", 1, 2) +
                                 fet("Q2", 20, "SW", "GND", 1, 3) + ")",
                             builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    const auto& sw = report["meta"]["switchNodes"];
    REQUIRE(sw.size() == 1);
    CHECK(sw[0] == "SW");
}

TEST_CASE("screener: a capacitor crowd marks a rail, never a switch node",
          "[screener][switchnode]") {
    // Two real FETs and an inductor sit on VBULK — but so do five bulk caps,
    // and bulk capacitance is what a dv/dt node cannot have. mppt-2420-hc's
    // HV+ input rail is exactly this shape.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "VBULK") (net 2 "X")
      (footprint "L" (layer "F.Cu") (at 30 10)
        (property "Reference" "L1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "VBULK")))
      (footprint "Q" (layer "F.Cu") (at 10 10)
        (property "Reference" "Q1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "VBULK")))
      (footprint "Q" (layer "F.Cu") (at 20 25)
        (property "Reference" "Q2")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "VBULK")))
    )";
    std::string caps;
    for (int i = 0; i < 5; ++i)
        caps += "(footprint \"C\" (layer \"F.Cu\") (at " + std::to_string(40 + i * 3) +
                " 10) (property \"Reference\" \"C" + std::to_string(i) + "\")"
                " (pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\")"
                " (net 1 \"VBULK\")))";
    BoardIR b = import_kicad(txt + caps + ")", builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    CHECK(report["meta"]["switchNodes"].empty());
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

TEST_CASE("screener: half-bridge phase node found; gate nets excluded",
          "[screener][switchnode]") {
    // An inverter leg: PHASE joins two FETs plus a bulk cap and a connector
    // (no inductor — the motor is the inductance). GATE_L also gathers FET
    // pads but only sees the driver and gate resistors, so it must NOT be
    // flagged. (VESC: phase nodes {Q,C,P,R,U} vs gate nets {Q,R,U}.)
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "PHASE") (net 2 "GATE_L") (net 3 "GND") (net 4 "VBUS")
      (segment (start 10 10) (end 20 10) (width 2.0) (layer "F.Cu") (net 1))
      (segment (start 10 20) (end 20 20) (width 0.3) (layer "F.Cu") (net 2))
      (footprint "Q" (layer "F.Cu") (at 10 10) (property "Reference" "Q1")
        (pad "1" smd rect (at 0 0) (size 2 2) (layers "F.Cu") (net 1 "PHASE"))
        (pad "2" smd rect (at 0 3) (size 2 2) (layers "F.Cu") (net 4 "VBUS"))
        (pad "3" smd rect (at 0 -3) (size 1 1) (layers "F.Cu") (net 2 "GATE_L")))
      (footprint "Q" (layer "F.Cu") (at 20 10) (property "Reference" "Q2")
        (pad "1" smd rect (at 0 0) (size 2 2) (layers "F.Cu") (net 1 "PHASE"))
        (pad "2" smd rect (at 0 3) (size 2 2) (layers "F.Cu") (net 3 "GND"))
        (pad "3" smd rect (at 0 -3) (size 1 1) (layers "F.Cu") (net 2 "GATE_L")))
      (footprint "C" (layer "F.Cu") (at 15 4) (property "Reference" "C1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 4 "VBUS"))
        (pad "2" smd rect (at 2 0) (size 1 1) (layers "F.Cu") (net 3 "GND")))
      (footprint "P" (layer "F.Cu") (at 25 10) (property "Reference" "P1")
        (pad "1" smd rect (at 0 0) (size 2 2) (layers "F.Cu") (net 1 "PHASE")))
      (footprint "U" (layer "F.Cu") (at 15 25) (property "Reference" "U1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 2 "GATE_L")))
      (footprint "R" (layer "F.Cu") (at 12 22) (property "Reference" "R1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 2 "GATE_L")))
      (zone (net 3) (net_name "GND") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 40 0) (xy 40 40) (xy 0 40))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    const auto& sw = report["meta"]["switchNodes"];
    REQUIRE(sw.size() == 1);
    CHECK(sw[0] == "PHASE");   // gate net rejected: no C/L/P/J in the power path

    // and its commutation loop is measured through the bulk cap
    const auto* loop = find_rule(report["findings"], "commutation-loop");
    REQUIRE(loop != nullptr);
    CHECK((*loop)["confidence"] == "heuristic");
    CHECK((*loop)["coupledLenMm"].get<double>() > 0.0);   // enclosed area, mm^2
    CHECK((*loop)["detail"].get<std::string>().find("C1") != std::string::npos);
    CHECK((*loop)["geom"]["lines"].size() >= 3);          // hull drawn
}

TEST_CASE("screener: commutation loop area grows when the cap moves away",
          "[screener][switchnode]") {
    auto with_cap_at = [](double cx) {
        std::string txt = R"((kicad_pcb
          (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
          (net 0 "") (net 1 "SW") (net 2 "GND") (net 3 "VIN")
          (segment (start 10 10) (end 20 10) (width 2.0) (layer "F.Cu") (net 1))
          (segment (start 10 30) (end 20 30) (width 2.0) (layer "F.Cu") (net 3))
          (footprint "L" (layer "F.Cu") (at 22 10) (property "Reference" "L1")
            (pad "1" smd rect (at 0 0) (size 2 2) (layers "F.Cu") (net 1 "SW")))
          (footprint "Q" (layer "F.Cu") (at 10 10) (property "Reference" "Q1")
            (pad "1" smd rect (at 0 0) (size 2 2) (layers "F.Cu") (net 1 "SW"))
            (pad "2" smd rect (at 0 2) (size 2 2) (layers "F.Cu") (net 3 "VIN")))
          (footprint "Q" (layer "F.Cu") (at 16 10) (property "Reference" "Q2")
            (pad "1" smd rect (at 0 0) (size 2 2) (layers "F.Cu") (net 1 "SW"))
            (pad "2" smd rect (at 0 2) (size 2 2) (layers "F.Cu") (net 2 "GND")))
          (footprint "C" (layer "F.Cu") (at )" + std::to_string(cx) + R"( 16)
            (property "Reference" "C1")
            (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 3 "VIN"))
            (pad "2" smd rect (at 1.5 0) (size 1 1) (layers "F.Cu") (net 2 "GND")))
          (zone (net 2) (net_name "GND") (layer "B.Cu")
            (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 40 0) (xy 40 40) (xy 0 40))))
        ))";
        return analyze_board(import_kicad(txt, builtin_stackup("default-2layer")));
    };
    const auto near = with_cap_at(13.0);
    const auto far = with_cap_at(34.0);
    const auto* ln = find_rule(near["findings"], "commutation-loop");
    const auto* lf = find_rule(far["findings"], "commutation-loop");
    REQUIRE(ln != nullptr);
    REQUIRE(lf != nullptr);
    // the physics the rule exists to express: a distant bulk cap = bigger loop
    CHECK((*lf)["coupledLenMm"].get<double>() > (*ln)["coupledLenMm"].get<double>());
    CHECK((*lf)["severity"].get<double>() >= (*ln)["severity"].get<double>());
}

TEST_CASE("screener: finding cap gives every rule a fair share",
          "[screener][ranking]") {
    // 40 tightly-packed victim traces around one aggressor: coupled-run and
    // 3w both fire in bulk. With a small cap, a global sort would hand every
    // slot to whichever rule ranks highest; round-robin must keep both.
    std::string full =
        "(kicad_pcb"
        " (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal))"
        " (net 0 \"\") (net 99 \"G\")"
        " (zone (net 99) (net_name \"G\") (layer \"B.Cu\")"
        "   (filled_polygon (layer \"B.Cu\")"
        "     (pts (xy 0 0) (xy 90 0) (xy 90 90) (xy 0 90))))";
    for (int i = 1; i <= 40; ++i) {
        std::string y = std::to_string(5.0 + i * 0.45);
        std::string n = std::to_string(i);
        full += " (net " + n + " \"N" + n + "\")"
                " (segment (start 5 " + y + ") (end 60 " + y + ")"
                " (width 0.3) (layer \"F.Cu\") (net " + n + "))";
    }
    full += ")";
    BoardIR b = import_kicad(full, builtin_stackup("default-2layer"));
    ScreenerParams p;
    p.max_findings = 20;
    nlohmann::json report = analyze_board(b, p);
    std::map<std::string, int> by_rule;
    for (const auto& f : report["findings"]) ++by_rule[f["rule"].get<std::string>()];
    CHECK(report["findings"].size() == 20);
    CHECK(report["meta"]["droppedByFindingCap"].get<int>() > 0);
    REQUIRE(by_rule.size() >= 2);            // no single rule monopolises
    CHECK(by_rule["coupled-run"] > 0);
    CHECK(by_rule["3w"] > 0);
    // and the surviving set is still ranked for display
    double prev = 2.0;
    for (const auto& f : report["findings"]) {
        CHECK(f["severity"].get<double>() <= prev + 1e-12);
        prev = f["severity"].get<double>();
    }
}

TEST_CASE("tline: quarter-wave resonance of an open stub", "[tline]") {
    // 1.5 mm stub in eps_r 4.5 laminate: c/(4*1.5e-3*sqrt(4.5)) = 23.6 GHz
    CHECK(tline::quarter_wave_hz(1.5, 4.5) / 1e9 == Approx(23.55).margin(0.1));
    // free space, 75 mm -> 1 GHz
    CHECK(tline::quarter_wave_hz(74.95, 1.0) / 1e9 == Approx(1.0).margin(0.01));
    // longer stub resonates lower
    CHECK(tline::quarter_wave_hz(3.0, 4.5) < tline::quarter_wave_hz(1.5, 4.5));
    CHECK_THROWS(tline::quarter_wave_hz(0.0, 4.5));
}

TEST_CASE("screener: via stubs aggregate per span and state the frequency",
          "[screener][stubs]") {
    // 4-layer: net routed only F.Cu->In1.Cu but stitched with through vias.
    // The unused In1..B.Cu barrel is the stub; three such vias must produce
    // ONE aggregated finding, not three.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" power) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SIG") (net 2 "GND")
      (segment (start 5 10) (end 10 10) (width 0.3) (layer "F.Cu") (net 1))
      (segment (start 10 10) (end 15 10) (width 0.3) (layer "In1.Cu") (net 1))
      (segment (start 5 14) (end 10 14) (width 0.3) (layer "F.Cu") (net 1))
      (segment (start 10 14) (end 15 14) (width 0.3) (layer "In1.Cu") (net 1))
      (segment (start 5 18) (end 10 18) (width 0.3) (layer "F.Cu") (net 1))
      (segment (start 10 18) (end 15 18) (width 0.3) (layer "In1.Cu") (net 1))
      (via (at 10 10) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
      (via (at 10 14) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
      (via (at 10 18) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
      (zone (net 2) (net_name "GND") (layer "In2.Cu")
        (filled_polygon (layer "In2.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-4layer"));
    nlohmann::json report = analyze_board(b);
    int n = 0;
    const nlohmann::json* vs = nullptr;
    for (const auto& f : report["findings"])
        if (f["rule"] == "via-stub") { ++n; vs = &f; }
    REQUIRE(n == 1);                                  // aggregated, not 3 rows
    CHECK((*vs)["title"].get<std::string>().find("3 via(s)") != std::string::npos);
    CHECK((*vs)["detail"].get<std::string>().find("GHz") != std::string::npos);
    CHECK((*vs)["coupledLenMm"].get<double>() > 1.0);  // In1..B.Cu barrel
    CHECK((*vs)["geom"]["markers"].size() == 3);       // every via still shown
    // relevance is the reader's call: the rule never claims a band matters
    CHECK((*vs)["detail"].get<std::string>().find("only if your edge rates")
          != std::string::npos);
}

TEST_CASE("screener: a fully-used via barrel is not a stub", "[screener][stubs]") {
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SIG") (net 2 "GND")
      (segment (start 5 10) (end 10 10) (width 0.3) (layer "F.Cu") (net 1))
      (segment (start 10 10) (end 15 10) (width 0.3) (layer "B.Cu") (net 1))
      (via (at 10 10) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
      (zone (net 2) (net_name "GND") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    CHECK(find_rule(report["findings"], "via-stub") == nullptr);
}

TEST_CASE("screener: dangling track end is an open stub; connected ends are not",
          "[screener][stubs]") {
    // SIG runs pad -> via (fine). STUB ends in mid-air after 8 mm.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SIG") (net 2 "STUB") (net 3 "GND")
      (segment (start 5 10) (end 15 10) (width 0.3) (layer "F.Cu") (net 1))
      (segment (start 15 10) (end 15 20) (width 0.3) (layer "F.Cu") (net 1))
      (segment (start 5 25) (end 13 25) (width 0.3) (layer "F.Cu") (net 2))
      (via (at 15 20) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
      (footprint "R" (layer "F.Cu") (at 5 10) (property "Reference" "R1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "SIG")))
      (footprint "R" (layer "F.Cu") (at 5 25) (property "Reference" "R2")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 2 "STUB")))
      (zone (net 3) (net_name "GND") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 30 0) (xy 30 30) (xy 0 30))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    int n = 0;
    const nlohmann::json* d = nullptr;
    for (const auto& f : report["findings"])
        if (f["rule"] == "dangling-stub") { ++n; d = &f; }
    REQUIRE(n == 1);                       // only STUB; SIG is pad->via anchored
    CHECK((*d)["netA"] == 2);
    CHECK((*d)["coupledLenMm"].get<double>() == Approx(8.0));
    CHECK((*d)["title"].get<std::string>().find("MHz") != std::string::npos);
    CHECK((*d)["geom"]["markers"].size() == 1);   // the open end is marked
}

TEST_CASE("screener: decoupling cap far from its IC pin is flagged; near is not",
          "[screener][decoupling]") {
    auto board = [](double cap_x) {
        std::string txt = R"((kicad_pcb
          (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
          (net 0 "") (net 1 "VDD") (net 2 "GND")
          (segment (start 5 10) (end 40 10) (width 0.4) (layer "F.Cu") (net 1))
          (footprint "U" (layer "F.Cu") (at 5 10) (property "Reference" "U1")
            (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "VDD"))
            (pad "2" smd rect (at 0 2) (size 1 1) (layers "F.Cu") (net 2 "GND")))
          (footprint "C" (layer "F.Cu") (at )" + std::to_string(cap_x) + R"( 10)
            (property "Reference" "C1")
            (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "VDD"))
            (pad "2" smd rect (at 1 0) (size 1 1) (layers "F.Cu") (net 2 "GND")))
          (zone (net 2) (net_name "GND") (layer "B.Cu")
            (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 50 0) (xy 50 30) (xy 0 30))))
        ))";
        return analyze_board(import_kicad(txt, builtin_stackup("default-2layer")));
    };
    // NOTE: hold the reports in named values — find_rule returns a pointer
    // INTO the json, so passing a temporary dangles.
    const nlohmann::json near_report = board(7.0);
    const nlohmann::json far_report = board(23.0);
    // 2 mm away: good practice, no finding
    CHECK(find_rule(near_report["findings"], "decoupling-distance") == nullptr);
    // 18 mm away: flagged, and it names both parts
    const auto* far = find_rule(far_report["findings"], "decoupling-distance");
    REQUIRE(far != nullptr);
    CHECK((*far)["coupledLenMm"].get<double>() == Approx(18.0).margin(0.5));
    CHECK((*far)["title"].get<std::string>().find("C1") != std::string::npos);
    CHECK((*far)["title"].get<std::string>().find("U1") != std::string::npos);
    CHECK((*far)["confidence"] == "heuristic");
}

TEST_CASE("screener: a signal pour couples through its boundary",
          "[screener][pour]") {
    // POWER is routed as a pour, not tracks. A victim track runs 0.4 mm from
    // its edge: before polygon-aware coupling this was invisible.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "POWER") (net 2 "VICTIM") (net 3 "GND")
      (segment (start 5 14.4) (end 25 14.4) (width 0.3) (layer "F.Cu") (net 2))
      (zone (net 1) (net_name "POWER") (layer "F.Cu")
        (filled_polygon (layer "F.Cu") (pts (xy 5 10) (xy 25 10) (xy 25 14) (xy 5 14))))
      (zone (net 3) (net_name "GND") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 40 0) (xy 40 40) (xy 0 40))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    const auto* cr = find_rule(report["findings"], "coupled-run");
    REQUIRE(cr != nullptr);
    CHECK((*cr)["detail"].get<std::string>().find("copper-pour boundary")
          != std::string::npos);
    CHECK((*cr)["coupledLenMm"].get<double>() == Approx(20.0).margin(0.5));
    // "3x trace width" is meaningless against a pour edge
    CHECK(find_rule(report["findings"], "3w") == nullptr);
    // the pour-routed net is still listed so the reader knows how it was judged
    CHECK(report["meta"]["polygonOnlyNets"].size() == 1);
    CHECK(report["meta"]["polygonOnlyNets"][0] == "POWER");
}

TEST_CASE("screener: a large pour is a return conductor, not a crosstalk victim",
          "[screener][pour]") {
    // Same geometry, but the pour covers most of the board and is named like a
    // supply/return. Coupling to the return path is not crosstalk, and on
    // plane-less boards (Fomu, ULX3S) this produced "GND <-> SPI_IO3 -12 dB"
    // as a HIGH finding until large pours were excluded.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "GND") (net 2 "VICTIM")
      (segment (start 2 34.4) (end 38 34.4) (width 0.3) (layer "F.Cu") (net 2))
      (zone (net 1) (net_name "GND") (layer "F.Cu")
        (filled_polygon (layer "F.Cu") (pts (xy 0 0) (xy 40 0) (xy 40 34) (xy 0 34))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    CHECK(find_rule(report["findings"], "coupled-run") == nullptr);
}

TEST_CASE("screener: many hard breaks on one plane roll up into one finding",
          "[screener][planes]") {
    // 12 nets all crossing the same uncovered region. Individually that is 12
    // rows of the same problem (HackRF One produced 129); rolled up it is one
    // plane-coverage finding naming its worst offenders.
    std::string full =
        "(kicad_pcb"
        " (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal))"
        " (net 0 \"\") (net 99 \"GND\")"
        " (zone (net 99) (net_name \"GND\") (layer \"B.Cu\")"
        "   (filled_polygon (layer \"B.Cu\")"
        "     (pts (xy 0 0) (xy 20 0) (xy 20 60) (xy 0 60))))";
    for (int i = 1; i <= 12; ++i) {
        std::string y = std::to_string(3.0 + i * 4.0);
        std::string n = std::to_string(i);
        full += " (net " + n + " \"N" + n + "\")"
                " (segment (start 10 " + y + ") (end 34 " + y + ")"
                " (width 0.3) (layer \"F.Cu\") (net " + n + "))";
    }
    full += ")";
    BoardIR b = import_kicad(full, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    int rows = 0;
    const nlohmann::json* roll = nullptr;
    for (const auto& f : report["findings"])
        if (f["rule"] == "plane-crossing") { ++rows; roll = &f; }
    REQUIRE(rows == 1);                            // one roll-up, not twelve
    CHECK((*roll)["title"].get<std::string>().find("12 nets") != std::string::npos);
    CHECK((*roll)["severityLabel"] == "high");
    CHECK((*roll)["detail"].get<std::string>().find("Worst:") != std::string::npos);
    CHECK((*roll)["coupledLenMm"].get<double>() > 100.0);   // summed detour
}

TEST_CASE("screener: asynchronous buck — the freewheel diode closes the loop",
          "[screener][switchnode]") {
    // Q1 + D1 on the switch node (no synchronous low-side FET). The diode
    // carries half the commutation current, so it belongs in the loop; the
    // LibreSolar mppt-2420-lc taught this.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SW") (net 2 "GND") (net 3 "VIN")
      (segment (start 10 10) (end 20 10) (width 2.0) (layer "F.Cu") (net 1))
      (segment (start 10 20) (end 20 20) (width 2.0) (layer "F.Cu") (net 3))
      (footprint "L" (layer "F.Cu") (at 24 10) (property "Reference" "L1")
        (pad "1" smd rect (at 0 0) (size 2 2) (layers "F.Cu") (net 1 "SW")))
      (footprint "Q" (layer "F.Cu") (at 10 10) (property "Reference" "Q1")
        (pad "1" smd rect (at 0 0) (size 2 2) (layers "F.Cu") (net 1 "SW"))
        (pad "2" smd rect (at 0 3) (size 2 2) (layers "F.Cu") (net 3 "VIN")))
      (footprint "D" (layer "F.Cu") (at 18 10) (property "Reference" "D1")
        (pad "1" smd rect (at 0 0) (size 2 2) (layers "F.Cu") (net 1 "SW"))
        (pad "2" smd rect (at 0 3) (size 2 2) (layers "F.Cu") (net 2 "GND")))
      (footprint "C" (layer "F.Cu") (at 14 24) (property "Reference" "C1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 3 "VIN"))
        (pad "2" smd rect (at 1.5 0) (size 1 1) (layers "F.Cu") (net 2 "GND")))
      (zone (net 2) (net_name "GND") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 40 0) (xy 40 40) (xy 0 40))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    nlohmann::json report = analyze_board(b);
    const auto* loop = find_rule(report["findings"], "commutation-loop");
    REQUIRE(loop != nullptr);
    CHECK((*loop)["detail"].get<std::string>().find("C1") != std::string::npos);
    CHECK((*loop)["coupledLenMm"].get<double>() > 0.0);
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

TEST_CASE("diff: identity matching, thresholds and the regression gate",
          "[diff]") {
    auto mk = [](const char* rule, const char* na, const char* nb, int cu,
                 const char* sev, double db, double len, const char* id) {
        return nlohmann::json{{"rule", rule}, {"netA", na}, {"netB", nb},
                              {"cuA", cu}, {"cuB", cu},
                              {"severityLabel", sev}, {"nextDb", db},
                              {"coupledLenMm", len}, {"id", id},
                              {"title", std::string(rule) + " " + na}};
    };
    nlohmann::json base{{"findings", {
        mk("coupled-run", "CLK", "DATA", 0, "medium", -20.0, 30.0, "F-0001"),
        mk("via-stub", "CLK", "", 0, "low", -99, 5.0, "F-0002"),
        mk("plane-crossing", "PWM", "GND", 0, "high", -99, 12.0, "F-0003"),
    }}};
    nlohmann::json cur{{"findings", {
        // same key, order and ID shifted, coupling worse by 3 dB
        mk("coupled-run", "DATA", "CLK", 0, "medium", -17.0, 30.0, "F-0002"),
        // brand-new high
        mk("commutation-loop", "SW", "", 0, "high", -99, 200.0, "F-0001"),
        // via-stub unchanged within noise (0.5 mm ~ 10% but < 2 mm)
        mk("via-stub", "CLK", "", 0, "low", -99, 5.4, "F-0003"),
        // plane-crossing gone -> resolved
    }}};
    nlohmann::json d = diff::diff_reports(base, cur);
    REQUIRE(d["added"].size() == 1);
    CHECK(d["added"][0]["title"] == "commutation-loop SW");
    REQUIRE(d["worsened"].size() == 1);   // the swapped-net coupled-run, by dB
    CHECK(d["worsened"][0]["id"] == "F-0002");
    CHECK(d["worsened"][0]["why"].get<std::string>().find("coupling") == 0);
    REQUIRE(d["resolved"].size() == 1);
    CHECK(d["improved"].size() == 0);     // the 0.4 mm via-stub wiggle is noise
    CHECK(d["verdict"] == "regression");

    CHECK(diff::has_regression_at(d, "high"));    // the new commutation loop
    // at 'medium' the worsened coupled-run also gates — still true
    CHECK(diff::has_regression_at(d, "medium"));

    // an unchanged pair of reports says so
    nlohmann::json same = diff::diff_reports(base, base);
    CHECK(same["verdict"] == "unchanged");
    CHECK(!diff::has_regression_at(same, "medium"));

    // a sub-1-dB drift is NOT a regression
    nlohmann::json drift = base;
    drift["findings"][0]["nextDb"] = -19.4;
    CHECK(diff::diff_reports(base, drift)["verdict"] == "unchanged");
}

TEST_CASE("fixes: stitching vias are proposed, clear, verified and NOT a regression",
          "[fixes]") {
    // 4-layer board: signal runs on F.Cu then drops to B.Cu through a via at
    // (30,10); GND pours cover In1+In2; the only existing GND via sits 60 mm
    // away — far outside reach, so the layer change is unstitched.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SIG") (net 2 "GND")
      (segment (start 5 10) (end 30 10) (width 0.25) (layer "F.Cu") (net 1))
      (via (at 30 10) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
      (segment (start 30 10) (end 55 10) (width 0.25) (layer "B.Cu") (net 1))
      (via (at 90 40) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 2))
      (zone (net 2) (net_name "GND") (layer "In1.Cu")
        (polygon (pts (xy 0 0) (xy 100 0) (xy 100 45) (xy 0 45)))
        (filled_polygon (layer "In1.Cu")
          (pts (xy 0 0) (xy 100 0) (xy 100 45) (xy 0 45))))
      (zone (net 2) (net_name "GND") (layer "In2.Cu")
        (polygon (pts (xy 0 0) (xy 100 0) (xy 100 45) (xy 0 45)))
        (filled_polygon (layer "In2.Cu")
          (pts (xy 0 0) (xy 100 0) (xy 100 45) (xy 0 45))))
    )";
    txt += ")";   // the raw-string terminator ate the board's closer (lesson #3)
    BoardIR b = import_kicad(txt, builtin_stackup("default-4layer"));
    Screener sc(b);
    fixes::StitchPlan plan = fixes::propose_stitching(b, sc);
    REQUIRE(plan.unstitched_seen == 1);
    REQUIRE(plan.vias.size() == 1);
    const auto& sv = plan.vias[0];
    CHECK(sv.net_name == "GND");
    CHECK(sv.near_net == "SIG");
    // within the search ring of the signal via, in the board's via style
    CHECK(std::hypot(sv.x - 30.0, sv.y - 10.0) < 4.5);
    CHECK(sv.size == 0.6);
    CHECK(sv.drill == 0.3);
    // clear of the signal track (0.25 wide) by pad radius + 0.2
    CHECK(std::abs(sv.y - 10.0) > 0.2 + 0.3 + 0.125);

    // apply, and the patched board must be BETTER, never a regression
    std::string patched = fixes::apply_stitching(txt, b, plan.vias);
    BoardIR b2 = import_kicad(patched, builtin_stackup("default-4layer"));
    Screener sc2(b2);
    CHECK(fixes::propose_stitching(b2, sc2).unstitched_seen == 0);
    nlohmann::json before = analyze_board(b), after = analyze_board(b2);
    nlohmann::json d = diff::diff_reports(before, after);
    CHECK(d["added"].empty());
    CHECK(d["worsened"].empty());
}

TEST_CASE("fixes: a single-plane board gets zero vias and the honest reason",
          "[fixes]") {
    // 2-layer: only Bottom carries a pour — a stitch would land in air on top
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "SIG") (net 2 "GND")
      (segment (start 5 10) (end 30 10) (width 0.25) (layer "F.Cu") (net 1))
      (via (at 30 10) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
      (segment (start 30 10) (end 55 25) (width 0.25) (layer "B.Cu") (net 1))
      (via (at 90 40) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 2))
      (zone (net 2) (net_name "GND") (layer "B.Cu")
        (polygon (pts (xy 0 0) (xy 100 0) (xy 100 45) (xy 0 45)))
        (filled_polygon (layer "B.Cu")
          (pts (xy 0 0) (xy 100 0) (xy 100 45) (xy 0 45))))
    )";
    txt += ")";   // same raw-string-terminator trap
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    Screener sc(b);
    fixes::StitchPlan plan = fixes::propose_stitching(b, sc);
    CHECK(plan.unstitched_seen == 1);
    CHECK(plan.vias.empty());
    REQUIRE(!plan.notes.empty());
    CHECK(plan.notes[0].find("stackup problem") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Rules extracted from Franz, "EMV: Störungssicherer Aufbau elektronischer
// Schaltungen", 5th ed. (2013). Each cites the section it implements.
// ---------------------------------------------------------------------------

// §7.2: connector grounds scattered around the board = Reihenmassestruktur;
// the ground potential difference between the entries drives the cables.
TEST_CASE("franz: scattered connector grounds are flagged, a star is not",
          "[screener][franz]") {
    auto board = [](bool scattered) {
        std::string t = R"((kicad_pcb
          (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
          (net 0 "") (net 1 "GND") (net 2 "SIG")
          (segment (start 10 10) (end 60 10) (width 0.3) (layer "F.Cu") (net 2))
          (zone (net 1) (net_name "GND") (layer "B.Cu")
            (filled_polygon (layer "B.Cu")
              (pts (xy 0 0) (xy 80 0) (xy 80 50) (xy 0 50))))
          (gr_line (start 0 0) (end 80 0) (layer "Edge.Cuts"))
          (gr_line (start 80 0) (end 80 50) (layer "Edge.Cuts"))
          (gr_line (start 80 50) (end 0 50) (layer "Edge.Cuts"))
          (gr_line (start 0 50) (end 0 0) (layer "Edge.Cuts"))
          (footprint "conn" (layer "F.Cu") (at 2 2)
            (property "Reference" "J1")
            (pad "1" smd rect (at 0 0) (size 1.5 1.5) (layers "F.Cu") (net 1 "GND"))
            (pad "2" smd rect (at 2 0) (size 1.5 1.5) (layers "F.Cu") (net 2 "SIG")))
        )";
        // J2 either far away (opposite corner) or next to J1 (star). The raw
        // string above deliberately leaves (kicad_pcb open — close it here.
        std::string at = scattered ? "(at 78 48)" : "(at 6 2)";
        t += "(footprint \"conn\" (layer \"F.Cu\") " + at +
             " (property \"Reference\" \"J2\")"
             " (pad \"1\" smd rect (at 0 0) (size 1.5 1.5)"
             " (layers \"F.Cu\") (net 1 \"GND\"))))";
        return t;
    };
    nlohmann::json scattered = analyze_board(
        import_kicad(board(true), builtin_stackup("default-2layer")));
    const auto* f = find_rule(scattered["findings"], "connector-ground-spread");
    REQUIRE(f != nullptr);
    CHECK((*f)["title"].get<std::string>().find("J1") != std::string::npos);
    // the plane between them keeps it review-grade, per Franz's Vermaschung
    CHECK((*f)["severityLabel"] == "low");

    nlohmann::json star = analyze_board(
        import_kicad(board(false), builtin_stackup("default-2layer")));
    CHECK(find_rule(star["findings"], "connector-ground-spread") == nullptr);
}

// §5.9.3: the VCC/GND cavity's first modes from Gl. 5.3, and the corner
// placement of the aggressor named.
TEST_CASE("franz: plane cavity modes are computed from Gl. 5.3",
          "[screener][franz]") {
    // 4-layer with GND and VCC inner planes and a corner switch cluster
    std::string t = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal)
              (31 "B.Cu" signal))
      (net 0 "") (net 1 "SW") (net 2 "GND") (net 3 "VCC")
      (segment (start 5 5) (end 15 5) (width 1.0) (layer "F.Cu") (net 1))
      (zone (net 2) (net_name "GND") (layer "In1.Cu")
        (filled_polygon (layer "In1.Cu")
          (pts (xy 0 0) (xy 100 0) (xy 100 60) (xy 0 60))))
      (zone (net 3) (net_name "VCC") (layer "In2.Cu")
        (filled_polygon (layer "In2.Cu")
          (pts (xy 0 0) (xy 100 0) (xy 100 60) (xy 0 60))))
      (gr_line (start 0 0) (end 100 0) (layer "Edge.Cuts"))
      (gr_line (start 100 0) (end 100 60) (layer "Edge.Cuts"))
      (gr_line (start 100 60) (end 0 60) (layer "Edge.Cuts"))
      (gr_line (start 0 60) (end 0 0) (layer "Edge.Cuts"))
      (footprint "L" (layer "F.Cu") (at 5 5)
        (property "Reference" "L1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "SW")))
      (footprint "Q" (layer "F.Cu") (at 15 5)
        (property "Reference" "Q1")
        (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "SW")))
    ))";
    nlohmann::json rep = analyze_board(
        import_kicad(t, builtin_stackup("default-4layer")));
    const auto* f = find_rule(rep["findings"], "plane-cavity-mode");
    REQUIRE(f != nullptr);
    // f10 = c0 / (2*sqrt(er)*a); default-4layer's inner dielectric er=4.5,
    // a=100mm -> ~707 MHz. Title carries the rounded figure.
    CHECK((*f)["title"].get<std::string>().find("707") != std::string::npos);
    // the aggressor sits in a corner and the detail must say what that means
    CHECK((*f)["detail"].get<std::string>().find("corner") != std::string::npos);
}

// §5.6 / Tab. 5.2: a decoupling cap whose plane via is millimetres away is
// decoupling through a stub; one with a via at the pad is fine.
TEST_CASE("franz: a long via stub on a decoupling cap is flagged",
          "[screener][franz]") {
    auto board = [](double via_x) {
        std::string t = R"((kicad_pcb
          (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal)
                  (31 "B.Cu" signal))
          (net 0 "") (net 1 "GND") (net 2 "VCC") (net 3 "S")
          (segment (start 5 40) (end 25 40) (width 0.3) (layer "F.Cu") (net 3))
          (segment (start 5 41) (end 25 41) (width 0.3) (layer "F.Cu") (net 3))
          (zone (net 1) (net_name "GND") (layer "In1.Cu")
            (filled_polygon (layer "In1.Cu")
              (pts (xy 0 0) (xy 60 0) (xy 60 50) (xy 0 50))))
          (footprint "C" (layer "F.Cu") (at 10 10)
            (property "Reference" "C1")
            (pad "1" smd rect (at 0 0) (size 1 0.6) (layers "F.Cu") (net 1 "GND"))
            (pad "2" smd rect (at 1.6 0) (size 1 0.6) (layers "F.Cu") (net 2 "VCC")))
        )";
        // the raw string leaves (kicad_pcb open — the via and the final close
        // are appended, per the raw-string-terminator trap noted above
        char via[170];
        std::snprintf(via, sizeof via,
                      "(via (at %.1f 10) (size 0.6) (drill 0.3) "
                      "(layers \"F.Cu\" \"B.Cu\") (net 1)))",
                      via_x);
        t += via;
        return t;
    };
    // via 8 mm from the GND pad -> stub finding
    nlohmann::json far = analyze_board(
        import_kicad(board(2.0), builtin_stackup("default-4layer")));
    const auto* f = find_rule(far["findings"], "cap-via-stub");
    REQUIRE(f != nullptr);
    CHECK((*f)["detail"].get<std::string>().find("C1") != std::string::npos);
    // via at the pad -> no finding
    nlohmann::json near_ = analyze_board(
        import_kicad(board(10.5), builtin_stackup("default-4layer")));
    CHECK(find_rule(near_["findings"], "cap-via-stub") == nullptr);
}
