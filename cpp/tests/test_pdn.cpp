// The PDN model. Anchors: exact series-RLC identities, the classic two-cap
// anti-resonance, and mounting inductance that tracks the board's own via
// distances — the one number nobody's datasheet can supply.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <faraday/Import.hpp>
#include <faraday/Pdn.hpp>

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
