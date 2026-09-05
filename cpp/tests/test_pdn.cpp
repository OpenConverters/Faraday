// The PDN model. Anchors: exact series-RLC identities, the classic two-cap
// anti-resonance, and mounting inductance that tracks the board's own via
// distances — the one number nobody's datasheet can supply.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/Import.hpp>
#include <faraday/Pdn.hpp>
#include <faraday/Operating.hpp>

using namespace faraday;
using Catch::Matchers::WithinRel;

namespace {

BoardIR rig(double via_d = 1.0) {
    BoardIR b;
    b.copper_names = {"F.Cu", "B.Cu"};
    b.stackup.layers = {
        {LayerKind::Copper, "F.Cu", 0.035, std::nullopt, "signal"},
        {LayerKind::Dielectric, "core", 0.2, 4.3, ""},
        {LayerKind::Copper, "B.Cu", 0.035, std::nullopt, "signal"},
    };
    b.stackup.source = "test";
    b.nets = {{0, ""}, {1, "3V3"}, {2, "GND"}};
    b.bbox_x1 = 0; b.bbox_y1 = 0; b.bbox_x2 = 30; b.bbox_y2 = 30;
    b.bbox_from_outline = true;
    b.zones.push_back({2, 1, {{0, 0}, {30, 0}, {30, 30}, {0, 30}}});

    b.components.push_back({"C1", "lib:C_0402", "100n", 10.0, 10.0, 0.0});
    b.pads.push_back({"C1", 1, 9.5, 10.0, 0.5, 0.5, false, 0});
    b.pads.push_back({"C1", 2, 10.5, 10.0, 0.5, 0.5, false, 0});
    b.vias.push_back({1, 9.5 - via_d, 10.0, 0.6, 0.3, 0, 1});
    b.vias.push_back({2, 10.5 + via_d, 10.0, 0.6, 0.3, 0, 1});
    return b;
}

}  // namespace

TEST_CASE("capacitor values parse in every idiom, and refuse ambiguity",
          "[pdn]") {
    CHECK_THAT(*pdn::parse_capacitance("100n"), WithinRel(100e-9, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("100nF"), WithinRel(100e-9, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("4u7"), WithinRel(4.7e-6, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("0.1u"), WithinRel(100e-9, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("2200p"), WithinRel(2.2e-9, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("10 uF"), WithinRel(10e-6, 1e-12));
    // a bare number is REFUSED: "100" could be pF or nF depending on the
    // library's habit, and a wrong guess poisons the whole curve
    CHECK_FALSE(pdn::parse_capacitance("100").has_value());
    CHECK_FALSE(pdn::parse_capacitance("DNP").has_value());
    CHECK_FALSE(pdn::parse_capacitance("").has_value());
}

TEST_CASE("a single branch has its exact series-RLC identities", "[pdn]") {
    // At resonance the reactances cancel and |Z| = ESR, exactly. That is not a
    // property of the grid — it is the algebra, and it pins the branch model.
    pdn::Rail rail;
    rail.name = "T";
    pdn::CapBranch b;
    b.c_f = 100e-9;
    b.esl_h = 1e-9;
    b.l_mount_h = 0;
    b.esr_ohm = 0.02;
    b.f_res_hz = 1.0 / (2 * M_PI * std::sqrt(1e-9 * 100e-9));
    rail.caps.push_back(b);
    pdn::Params p;
    p.vrm_r_ohm = 1e9;         // park the VRM so the branch is alone
    p.vrm_l_h = 1.0;

    CHECK_THAT(std::abs(pdn::impedance_at(rail, p, b.f_res_hz)),
               WithinRel(0.02, 1e-6));
    // a decade below resonance the branch is capacitive: |Z| ~ 1/(wC)
    const double f = b.f_res_hz / 10;
    CHECK_THAT(std::abs(pdn::impedance_at(rail, p, f)),
               WithinRel(1.0 / (2 * M_PI * f * 100e-9), 0.02));
    // a decade above it is inductive: |Z| ~ wL
    const double fh = b.f_res_hz * 10;
    CHECK_THAT(std::abs(pdn::impedance_at(rail, p, fh)),
               WithinRel(2 * M_PI * fh * 1e-9, 0.02));
}

TEST_CASE("two staggered capacitors produce the classic anti-resonance",
          "[pdn]") {
    // Between one cap's inductive rise and the next cap's capacitive fall the
    // pair parallel-resonates and |Z| PEAKS — the counterintuitive fact a PDN
    // panel exists to show, because "add more caps" can make a frequency worse.
    pdn::Rail one, two;
    pdn::CapBranch big;
    big.c_f = 10e-6; big.esl_h = 1e-9; big.esr_ohm = 0.005;
    pdn::CapBranch small;
    small.c_f = 10e-9; small.esl_h = 0.5e-9; small.esr_ohm = 0.01;
    one.caps = {big};
    two.caps = {big, small};
    pdn::Params p;
    p.vrm_r_ohm = 1e9; p.vrm_l_h = 1.0;

    const pdn::Curve c2 = pdn::curve(two, p);
    REQUIRE_FALSE(c2.antires.empty());
    // the peak sits between the two series resonances
    const double f1 = 1 / (2 * M_PI * std::sqrt(1e-9 * 10e-6));
    const double f2 = 1 / (2 * M_PI * std::sqrt(0.5e-9 * 10e-9));
    bool between = false;
    for (auto& [f, z] : c2.antires) between = between || (f > f1 && f < f2);
    CHECK(between);
    // and AT that peak, the pair is WORSE than the big cap alone
    double worst_f = 0, worst_z = 0;
    for (auto& [f, z] : c2.antires)
        if (f > f1 && f < f2 && z > worst_z) { worst_z = z; worst_f = f; }
    CHECK(worst_z > std::abs(pdn::impedance_at(one, p, worst_f)));
}

TEST_CASE("mounting inductance is measured off the board's own vias", "[pdn]") {
    Screener s1(rig(0.5)), s2(rig(3.0));
    const BoardIR b1 = rig(0.5), b2 = rig(3.0);
    const auto r1 = pdn::discover(b1, s1, pdn::Params{});
    const auto r2 = pdn::discover(b2, s2, pdn::Params{});
    REQUIRE(r1.rails.size() == 1);
    REQUIRE(r1.rails[0].caps.size() == 1);
    const auto &c1 = r1.rails[0].caps[0], &c2 = r2.rails[0].caps[0];
    // same part, same value — the board decides the inductance
    CHECK_THAT(c1.c_f, WithinRel(100e-9, 1e-9));
    CHECK(c2.l_mount_h > 2.0 * c1.l_mount_h);
    // and the resonance moves down with it, as it does on a real board
    CHECK(c2.f_res_hz < c1.f_res_hz);
    // the 0402 package ESL came from the footprint string
    CHECK_THAT(c1.esl_h, WithinRel(0.4e-9, 1e-9));
}

TEST_CASE("rail discovery is grounded in the netlist, and refuses without one",
          "[pdn]") {
    const BoardIR b = rig();
    Screener s(b);
    const auto r = pdn::discover(b, s, pdn::Params{});
    CHECK(r.gnd_name == "GND");
    CHECK(r.rails[0].name == "3V3");

    // a board with no ground and no plane is refused, not guessed at
    BoardIR bare = rig();
    bare.nets = {{0, ""}, {1, "A"}, {2, "B"}};
    bare.zones.clear();
    Screener sb(bare);
    CHECK_THROWS_WITH(pdn::discover(bare, sb, pdn::Params{}),
                      Catch::Matchers::ContainsSubstring("no ground net"));
}

TEST_CASE("an unparseable value is counted, never guessed", "[pdn]") {
    BoardIR b = rig();
    b.components.push_back({"C2", "lib:C_0603", "DNP", 20.0, 10.0, 0.0});
    b.pads.push_back({"C2", 1, 19.5, 10.0, 0.5, 0.5, false, 0});
    b.pads.push_back({"C2", 2, 20.5, 10.0, 0.5, 0.5, false, 0});
    Screener s(b);
    const auto r = pdn::discover(b, s, pdn::Params{});
    CHECK(r.rails[0].caps.size() == 1);
    CHECK(r.rails[0].skipped_unparsed == 1);
}

TEST_CASE("the micro sign parses — real boards do not spell it 'u'",
          "[pdn][values]") {
    // KiCad writes U+00B5 (MICRO SIGN); some libraries write U+03BC (Greek
    // small letter mu). A parser that knows only ASCII 'u' drops every
    // electrolytic: on the LibreSolar MPPT that was 780 uF of input bulk,
    // three of the five capacitors on the converter's own input rail.
    CHECK_THAT(*pdn::parse_capacitance("390µF"), WithinRel(390e-6, 1e-9));
    CHECK_THAT(*pdn::parse_capacitance("1µF"), WithinRel(1e-6, 1e-9));
    CHECK_THAT(*pdn::parse_capacitance("4μ7"), WithinRel(4.7e-6, 1e-9));
    CHECK_THAT(*pdn::parse_capacitance("100nF"), WithinRel(100e-9, 1e-9));
    // and the refusals still refuse: a bare number has no unit
    CHECK_FALSE(pdn::parse_capacitance("100").has_value());
    CHECK_FALSE(pdn::parse_capacitance("DNP").has_value());
}

TEST_CASE("the reference is chosen by copper, not by net-table order",
          "[pdn][isolated]") {
    // An isolated front end: VSS_POE is a real return with a name that says
    // ground, listed FIRST, carrying nothing. GND is the board's actual
    // reference — a full pour with every decoupling capacitor on it.
    //
    // The old rule was "the first net whose name contains gnd or vss", so the
    // exporter's net ordering decided the answer, and this board reported
    // "no decoupling capacitor found between a power net and VSS_POE" while
    // sitting on a fully decoupled 3V3 rail.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "VSS_POE") (net 2 "GND") (net 3 "+3V3")
      (segment (start 2 2) (end 8 2) (width 0.3) (layer "F.Cu") (net 1))
      (zone (net 2) (net_name "GND") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 40 0) (xy 40 30) (xy 0 30))))
      (via (at 12.4 10) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 2))
      (via (at 12.4 12) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 3))
      (footprint "C" (layer "F.Cu") (at 12 10)
        (property "Reference" "C1") (property "Value" "100nF")
        (pad "1" smd rect (at 0 0) (size 0.6 0.6) (layers "F.Cu") (net 3 "+3V3"))
        (pad "2" smd rect (at 1 0) (size 0.6 0.6) (layers "F.Cu") (net 2 "GND")))
      (footprint "C" (layer "F.Cu") (at 16 10)
        (property "Reference" "C2") (property "Value" "10uF")
        (pad "1" smd rect (at 0 0) (size 0.6 0.6) (layers "F.Cu") (net 3 "+3V3"))
        (pad "2" smd rect (at 1 0) (size 0.6 0.6) (layers "F.Cu") (net 2 "GND")))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    Screener sc(b);
    const auto r = pdn::discover(b, sc, pdn::Params{});

    CHECK(r.gnd_name == "GND");                 // the copper decides
    REQUIRE(r.rails.size() == 1);
    CHECK(r.rails[0].name == "+3V3");
    CHECK(r.rails[0].caps.size() == 2);

    // and both returns are reported, best first, with the evidence
    REQUIRE(r.gnd_candidates.size() >= 2);
    CHECK(r.gnd_candidates[0].name == "GND");
    CHECK(r.gnd_candidates[0].pour_mm2 > 1000.0);
    CHECK(r.gnd_candidates[0].cap_terminals == 2);
    bool saw_poe = false;
    for (const auto& c : r.gnd_candidates)
        if (c.name == "VSS_POE") {
            saw_poe = true;
            CHECK(c.named);                     // it IS a candidate
            CHECK(c.pour_mm2 == 0.0);           // it just has no copper
            CHECK(c.cap_terminals == 0);
        }
    CHECK(saw_poe);
}

TEST_CASE("an isolated board can be asked about the other side by name",
          "[pdn][isolated]") {
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "VSS_POE") (net 2 "GND") (net 3 "+3V3") (net 4 "VPOE")
      (zone (net 2) (net_name "GND") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 40 0) (xy 40 30) (xy 0 30))))
      (zone (net 1) (net_name "VSS_POE") (layer "F.Cu")
        (filled_polygon (layer "F.Cu") (pts (xy 0 0) (xy 10 0) (xy 10 10) (xy 0 10))))
      (via (at 4.4 4) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
      (via (at 4.4 6) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 4))
      (via (at 12.4 10) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 2))
      (via (at 12.4 12) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 3))
      (footprint "C" (layer "F.Cu") (at 4 4)
        (property "Reference" "C9") (property "Value" "220nF")
        (pad "1" smd rect (at 0 0) (size 0.6 0.6) (layers "F.Cu") (net 4 "VPOE"))
        (pad "2" smd rect (at 1 0) (size 0.6 0.6) (layers "F.Cu") (net 1 "VSS_POE")))
      (footprint "C" (layer "F.Cu") (at 12 10)
        (property "Reference" "C1") (property "Value" "100nF")
        (pad "1" smd rect (at 0 0) (size 0.6 0.6) (layers "F.Cu") (net 3 "+3V3"))
        (pad "2" smd rect (at 1 0) (size 0.6 0.6) (layers "F.Cu") (net 2 "GND")))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    Screener sc(b);

    // by default the bigger return wins
    CHECK(pdn::discover(b, sc, pdn::Params{}).gnd_name == "GND");

    // …and the isolated side is reachable when the designer names it
    pdn::Params p;
    p.gnd_net = "VSS_POE";
    const auto iso = pdn::discover(b, sc, p);
    CHECK(iso.gnd_name == "VSS_POE");
    REQUIRE(iso.rails.size() == 1);
    CHECK(iso.rails[0].name == "VPOE");

    // a name that is not on the board is a typo, not a fallback
    p.gnd_net = "VSS_POF";
    CHECK_THROWS_WITH(pdn::discover(b, sc, p),
                      Catch::Matchers::ContainsSubstring("typo rather than a fallback"));
}

TEST_CASE("the refusal names what it chose and what else was there",
          "[pdn][isolated]") {
    // No decoupling anywhere: the message must still be actionable.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "VSS_POE") (net 2 "GND")
      (zone (net 2) (net_name "GND") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 40 0) (xy 40 30) (xy 0 30))))
      (zone (net 1) (net_name "VSS_POE") (layer "F.Cu")
        (filled_polygon (layer "F.Cu") (pts (xy 0 0) (xy 10 0) (xy 10 10) (xy 0 10))))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    Screener sc(b);
    REQUIRE_THROWS_WITH(
        pdn::discover(b, sc, pdn::Params{}),
        Catch::Matchers::ContainsSubstring("chosen as the reference") &&
        Catch::Matchers::ContainsSubstring("Other returns on this board") &&
        Catch::Matchers::ContainsSubstring("VSS_POE") &&
        Catch::Matchers::ContainsSubstring("isolated"));
}

TEST_CASE("a value field carries a rating too, and that is not ambiguity",
          "[pdn][values]") {
    // Refusing these was refusing most of the real world: the leading field is
    // a complete capacitance and what follows is a voltage, a dielectric or a
    // tolerance. Reading it is not a guess.
    CHECK_THAT(*pdn::parse_capacitance("220n 100V"), WithinRel(220e-9, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("10uF/25V"), WithinRel(10e-6, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("4.7µF 16V X7R"), WithinRel(4.7e-6, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("100 nF"), WithinRel(100e-9, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("100 nF 50V"), WithinRel(100e-9, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("4u7,50V"), WithinRel(4.7e-6, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("1µF ±10% X5R"), WithinRel(1e-6, 1e-12));

    // IEC 60062 with the multiplier standing in for the decimal point, and no
    // whole part at all — how 73 of Glasgow's 92 capacitors are written
    CHECK_THAT(*pdn::parse_capacitance("u1"), WithinRel(100e-9, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("n47"), WithinRel(0.47e-9, 1e-12));
    CHECK_THAT(*pdn::parse_capacitance("u1 25V"), WithinRel(100e-9, 1e-12));
    CHECK_FALSE(pdn::parse_capacitance("uF").has_value());   // a unit alone

    // and the refusals still refuse — a unit is still required, and a field
    // that is not a capacitance is not one because it sits next to a number
    CHECK_FALSE(pdn::parse_capacitance("100").has_value());
    CHECK_FALSE(pdn::parse_capacitance("100 50V").has_value());
    CHECK_FALSE(pdn::parse_capacitance("50V 100n").has_value());   // rating first
    CHECK_FALSE(pdn::parse_capacitance("DNP").has_value());
    CHECK_FALSE(pdn::parse_capacitance("X7R").has_value());
    CHECK_FALSE(pdn::parse_capacitance("").has_value());

    // inductances read the same way
    CHECK_THAT(*op::parse_inductance("4u7 20%"), WithinRel(4.7e-6, 1e-12));
    CHECK_THAT(*op::parse_inductance("10 uH"), WithinRel(10e-6, 1e-12));
    CHECK_FALSE(op::parse_inductance("10 A").has_value());
}

TEST_CASE("a board whose export carried part numbers instead of values",
          "[pdn][values]") {
    // Altium's ODB++ writes the manufacturer part number in the component
    // record and no value at all, so every capacitor arrives nameless. The
    // model must refuse (a guessed capacitance is a wrong impedance curve that
    // looks exactly like a right one) and must say how to get the values back.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "GND") (net 2 "+3V3")
      (zone (net 1) (net_name "GND") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 40 0) (xy 40 30) (xy 0 30))))
      (via (at 12.4 10) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
      (via (at 12.4 12) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 2))
      (footprint "885012206077" (layer "F.Cu") (at 12 10)
        (property "Reference" "C1")
        (pad "1" smd rect (at 0 0) (size 0.6 0.6) (layers "F.Cu") (net 2 "+3V3"))
        (pad "2" smd rect (at 1 0) (size 0.6 0.6) (layers "F.Cu") (net 1 "GND")))
      (footprint "885012206077" (layer "F.Cu") (at 16 10)
        (property "Reference" "C2")
        (pad "1" smd rect (at 0 0) (size 0.6 0.6) (layers "F.Cu") (net 2 "+3V3"))
        (pad "2" smd rect (at 1 0) (size 0.6 0.6) (layers "F.Cu") (net 1 "GND")))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));
    Screener sc(b);

    // the refusal names the cause and the way out
    REQUIRE_THROWS_WITH(
        pdn::discover(b, sc, pdn::Params{}),
        Catch::Matchers::ContainsSubstring("carry NO value at all") &&
        Catch::Matchers::ContainsSubstring("885012206077") &&
        Catch::Matchers::ContainsSubstring("--parts-out") &&
        Catch::Matchers::ContainsSubstring("--values"));

    // the question the board can ask a catalogue
    const auto parts = parts_without_values(b);
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].first == "C1");
    CHECK(parts[0].second == "885012206077");

    // …and the answer, handed back
    values::ValueTable vt = values::parse_value_table("refdes,value\nC1,100pF\nC2,100pF\n");
    CHECK(apply_values(b, vt) == 2);
    Screener sc2(b);
    const auto r = pdn::discover(b, sc2, pdn::Params{});
    REQUIRE(r.rails.size() == 1);
    CHECK(r.rails[0].caps.size() == 2);
    CHECK_THAT(r.rails[0].caps[0].c_f, WithinRel(100e-12, 1e-9));
}

TEST_CASE("a value the board itself carries always beats the side file",
          "[pdn][values]") {
    BoardIR b;
    b.components.push_back({"C1", "lib:C_0603", "100nF", 0, 0, 0});
    b.components.push_back({"C2", "885012206077", "", 0, 0, 0});
    values::ValueTable vt =
        values::parse_value_table("C1,10uF\nC2,100pF\n");
    CHECK(apply_values(b, vt) == 1);          // only the empty one
    CHECK(b.components[0].value == "100nF");  // the layout wins
    CHECK(b.components[1].value == "100pF");
    CHECK(vt.ignored == 1);
    CHECK_THROWS_AS(values::parse_value_table("nonsense\n"), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// The catalogue's measured figures, in place of the board's guesses.
// ---------------------------------------------------------------------------

TEST_CASE("a catalogued ESR replaces the assumed one, and says it did",
          "[pdn][values]") {
    // 0.015 ohm was assumed for every capacitor on every board, and it reaches
    // the conducted-emissions maths through Operating's input branch — so the
    // noise number carried an assumption nobody could see.
    std::string txt = R"((kicad_pcb
      (layers (0 "F.Cu" signal) (31 "B.Cu" signal))
      (net 0 "") (net 1 "GND") (net 2 "+3V3")
      (zone (net 1) (net_name "GND") (layer "B.Cu")
        (filled_polygon (layer "B.Cu") (pts (xy 0 0) (xy 40 0) (xy 40 30) (xy 0 30))))
      (via (at 12.4 10) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
      (via (at 12.4 12) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 2))
      (footprint "C_0603" (layer "F.Cu") (at 12 10)
        (property "Reference" "C1") (property "Value" "100nF")
        (pad "1" smd rect (at 0 0) (size 0.6 0.6) (layers "F.Cu") (net 2 "+3V3"))
        (pad "2" smd rect (at 1 0) (size 0.6 0.6) (layers "F.Cu") (net 1 "GND")))
      (footprint "C_0603" (layer "F.Cu") (at 16 10)
        (property "Reference" "C2") (property "Value" "100nF")
        (pad "1" smd rect (at 0 0) (size 0.6 0.6) (layers "F.Cu") (net 2 "+3V3"))
        (pad "2" smd rect (at 1 0) (size 0.6 0.6) (layers "F.Cu") (net 1 "GND")))
    ))";
    BoardIR b = import_kicad(txt, builtin_stackup("default-2layer"));

    // C1 is identified in the catalogue; C2 is not.
    values::PartData pd;
    pd.esr_ohm = 0.004;          // a real low-ESR MLCC, not the 0.015 constant
    pd.esl_h = 0.42e-9;          // and its measured ESL, not the package table's 0.5 nH
    pd.mpn = "GRM188R71H104KA93D";
    pd.source = "kelvin";
    b.part_data["C1"] = pd;

    Screener sc(b);
    const auto r = pdn::discover(b, sc, pdn::Params{});
    REQUIRE(r.rails.size() == 1);
    const pdn::CapBranch* c1 = nullptr;
    const pdn::CapBranch* c2 = nullptr;
    for (const auto& c : r.rails[0].caps) {
        if (c.ref == "C1") c1 = &c;
        if (c.ref == "C2") c2 = &c;
    }
    REQUIRE(c1);
    REQUIRE(c2);

    // the identified part carries its own figures, and is marked as doing so
    CHECK_THAT(c1->esr_ohm, WithinRel(0.004, 1e-9));
    CHECK_THAT(c1->esl_h, WithinRel(0.42e-9, 1e-9));
    CHECK(c1->esr_measured);
    CHECK(c1->esl_measured);
    CHECK(c1->mpn == "GRM188R71H104KA93D");

    // the unidentified one keeps the estimates, and says they ARE estimates
    CHECK_THAT(c2->esr_ohm, WithinRel(0.015, 1e-9));
    CHECK_FALSE(c2->esr_measured);
    CHECK_FALSE(c2->esl_measured);
    CHECK(c2->mpn.empty());

    // and the difference reaches the physics: a lower ESL resonates higher
    CHECK(c1->f_res_hz > c2->f_res_hz);
}

TEST_CASE("a part that publishes only an ESR keeps the estimated ESL",
          "[pdn][values]") {
    // Most of the catalogue publishes one and not the other. Taking the pair
    // together would throw away the half that is real.
    BoardIR b;
    values::PartData pd;
    pd.esr_ohm = 0.006;
    pd.mpn = "SOMEPART";
    b.part_data["C9"] = pd;
    CHECK(values::has(b.part_data["C9"].esr_ohm));
    CHECK_FALSE(values::has(b.part_data["C9"].esl_h));   // NaN, not 0
    CHECK_FALSE(values::has(b.part_data["C9"].c_f));
}
