// Characterization tests on unmodified real production boards (see
// fixtures/real/ATTRIBUTION.md). These pin importer + screener behaviour at
// scale and across format generations; counts are the values observed at
// fixture capture (2026-07-27) — a change here means the importer's coverage
// changed and must be reviewed, not silently re-pinned.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <faraday/KicadImporter.hpp>
#include <faraday/Screener.hpp>
#include <faraday/Report.hpp>

#include <fstream>
#include <sstream>

using Catch::Approx;
using namespace faraday;

static std::string read_real(const char* name) {
    std::ifstream in(std::string(FARADAY_FIXTURE_DIR) + "/real/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

TEST_CASE("real: HackRF One (KiCad v6, 4-layer RF) imports and screens", "[real]") {
    BoardIR b = import_kicad(read_real("hackrf-one.kicad_pcb"));  // own stackup

    REQUIRE(b.copper_names.size() == 4);
    CHECK(b.copper_names[0] == "F.Cu");
    CHECK(b.copper_names[1] == "In1.Cu");
    CHECK(b.copper_names[2] == "In2.Cu");
    CHECK(b.copper_names[3] == "B.Cu");
    CHECK(b.stackup.source == "board-file");
    CHECK(b.segments.size() == 3817);
    CHECK(b.components.size() == 437);  // root-level footprints (verified 2026-07-27)
    CHECK(b.zones.size() == 5);         // 6 filled_polygons; 1 is on F.SilkS
                                        // (graphic zone) — non-copper, skipped
    CHECK(b.bbox_from_outline);

    nlohmann::json report = analyze_board(b);
    const auto& planes = report["meta"]["planes"];
    CHECK(planes[1]["isPlane"] == true);   // In1.Cu: 85% GND pour
    CHECK(planes[2]["isPlane"] == true);   // In2.Cu: 77% GND pour
    CHECK(planes[0]["isPlane"] == false);
    CHECK(planes[3]["isPlane"] == false);
    CHECK(report["findings"].size() == 200);  // cap reached — big board
    CHECK(report["meta"]["droppedByFindingCap"].get<int>() > 0);
    // Differential pairs (USB DP/DM and 9 others) are recognized as
    // intentional coupling. They are info-grade, so on a board this dense
    // they correctly rank BELOW the finding cap and never appear as defects —
    // the count is still reported in meta so nothing is silently lost.
    CHECK(report["meta"]["diffPairsRecognized"].get<int>() == 10);
    for (const auto& f : report["findings"]) {
        if (f["rule"] != "diff-pair") continue;
        CHECK(f["severityLabel"] == "info");
    }
    // HackRF is not a converter: no inductor+FET switch node expected
    CHECK(report["meta"]["switchNodes"].empty());
    // Its dual-LDO + ferrite harness (L10/L11 on U21) is externally
    // ISOMORPHIC to a fixed-output monolithic buck — feedback resistors,
    // copper width and package size were each tested across the corpus and
    // none separates them. The irreducible ambiguity must be REPORTED as
    // candidates (ABT #410: a heuristic's negative as visible as its
    // positive), never silently screened, never silently dropped.
    const auto& cand = report["meta"]["switchNodeCandidates"];
    REQUIRE(cand.size() == 2);
    std::set<std::string> cnets;
    for (const auto& c : cand) cnets.insert(c["net"].get<std::string>());
    CHECK(cnets == std::set<std::string>{"Net-(L10-Pad1)", "Net-(L11-Pad2)"});
}

TEST_CASE("real: LibreSolar MPPT 2420 HC (KiCad v5, power converter) imports and screens",
          "[real]") {
    BoardIR b = import_kicad(read_real("mppt-2420-hc.kicad_pcb"),
                             builtin_stackup("default-4layer"));

    REQUIRE(b.copper_names.size() == 4);
    CHECK(b.copper_names[0] == "Top");     // v5 renamed copper layers
    CHECK(b.copper_names[1] == "GND");
    CHECK(b.copper_names[2] == "3V3");
    CHECK(b.copper_names[3] == "Bottom");
    CHECK(b.components.size() == 172);     // (module ...) footprints
    CHECK(b.zones.size() == 37);           // zone-layer-inherited fills
    CHECK(b.segments.size() == 1263);  // root-level segments (verified 2026-07-27)

    nlohmann::json report = analyze_board(b);
    const auto& planes = report["meta"]["planes"];
    CHECK(planes[1]["isPlane"] == true);   // GND: pour + 'power' type hint
    CHECK(planes[2]["isPlane"] == true);   // 3V3
    CHECK(planes[3]["isPlane"] == true);   // Bottom: 76% pour
    // both planes carry fill geometry -> the void check ran everywhere
    CHECK(report["meta"]["crossingCheckSkippedPlanes"].empty());
    // power-domain findings the 2026-07-27 review verified by hand: the
    // gate-drive return-path break (now inside the per-plane roll-up, since
    // this board has more than max_individual_breaks of them) and the
    // VBUS <-> shunt-sense coupled run.
    bool ls_drv = false, vbus_shunt = false;
    for (const auto& f : report["findings"]) {
        const std::string t = f["title"].get<std::string>();
        const std::string d = f["detail"].get<std::string>();
        if (f["rule"] == "plane-crossing" &&
            (t.find("LS_DRV") != std::string::npos ||
             d.find("LS_DRV") != std::string::npos))
            ls_drv = true;
        if (f["rule"] == "coupled-run" && t.find("VBUS") != std::string::npos &&
            t.find("SHUNT") != std::string::npos)
            vbus_shunt = true;
    }
    CHECK(ls_drv);
    CHECK(vbus_shunt);

    // switch-node identification: the literal /DC/DC/SW_NODE is found by
    // CONNECTIVITY (inductor pad + FET pad, compact net) — not by its name
    const auto& sw = report["meta"]["switchNodes"];
    bool found_sw = false, supply_input = false;
    for (const auto& n : sw) {
        if (n.get<std::string>().find("SW_NODE") != std::string::npos) found_sw = true;
        if (n.get<std::string>().find("SUPPLY_INPUT") != std::string::npos)
            supply_input = true;
    }
    CHECK(found_sw);
    CHECK(sw.size() <= 3);  // a converter has a handful, not dozens
    // SUPPLY_INPUT carries L2+Q4 and screened as a commutation loop for a
    // day — but its two 1 uF caps to GND mark a supply-ORing rail (solar,
    // battery and +12V ORed onto the internal supply). The shunt-cap veto
    // must keep it out; this was finding F-0002 on the DEMO board.
    CHECK(!supply_input);
    // CAN_H/CAN_L recognized as a differential pair, not a defect
    CHECK(report["meta"]["diffPairsRecognized"].get<int>() >= 1);
}

// ---------------------------------------------------------------------------
// Tier-4 regression pins (ABT #422): human-reviewed derived critical meshes
// on corpus boards. The corpus is FETCHED (scripts/fetch_corpus.py), not
// committed, so these pin only when the board is present — a fetched corpus
// that regresses must fail loudly, an absent corpus is not a failure.
// ---------------------------------------------------------------------------

static std::string corpus_path(const char* name) {
    return std::string(FARADAY_FIXTURE_DIR) + "/../../../corpus/" + name;
}

TEST_CASE("corpus pin: mppt-2420-lc derives its sync buck mesh Q1+Q4+C4",
          "[real][corpus-pin]") {
    std::ifstream in(corpus_path("mppt-2420-lc.kicad_pcb"));
    if (!in.good()) SKIP("corpus not fetched");
    std::stringstream ss;
    ss << in.rdbuf();
    BoardIR b = import_kicad(ss.str(), builtin_stackup("default-2layer"));
    Screener sc(b);
    int sw = -1;
    for (const auto& n : b.nets)
        if (n.name == "/DCDC power stage/SW_NODE") sw = n.id;
    REQUIRE(sw >= 0);
    auto loop = sc.commutation_loop(sw);
    REQUIRE(loop.has_value());
    // reviewed 2026-07-31: both FETs of the synchronous buck + the input
    // capacitor — the member set is the point, not the exact area
    std::set<std::string> got(loop->members.begin(), loop->members.end());
    CHECK(got == std::set<std::string>{"Q1", "Q4", "C4"});
    CHECK(loop->shape == "two-device");
    CHECK(loop->area_mm2 > 40.0);
    CHECK(loop->area_mm2 < 110.0);
}

TEST_CASE("corpus pin: mppt-1210-hus derives Q1+Q4+C4; LOAD_S falls back",
          "[real][corpus-pin]") {
    std::ifstream in(corpus_path("mppt-1210-hus.kicad_pcb"));
    if (!in.good()) SKIP("corpus not fetched");
    std::stringstream ss;
    ss << in.rdbuf();
    BoardIR b = import_kicad(ss.str(), builtin_stackup("default-2layer"));
    Screener sc(b);
    int sw = -1, load = -1;
    for (const auto& n : b.nets) {
        if (n.name == "/DCDC power stage/SW_NODE") sw = n.id;
        if (n.name == "LOAD_S") load = n.id;
    }
    REQUIRE(sw >= 0);
    auto loop = sc.commutation_loop(sw);
    REQUIRE(loop.has_value());
    std::set<std::string> got(loop->members.begin(), loop->members.end());
    CHECK(got == std::set<std::string>{"Q1", "Q4", "C4"});
    // LOAD_S: a load switch whose only 'clamp' candidates are dividers —
    // the derived trace must REFUSE (R+R rule) and fall back to geometry
    if (load >= 0) {
        auto ll = sc.commutation_loop(load);
        if (ll) CHECK(ll->members.empty());   // geometric fallback, not R+R
    }
}

TEST_CASE("corpus pin: ulx3s's three monolithic bucks are OFFERED as "
          "candidates (ABT #408's headline miss, now self-reporting)",
          "[real][corpus-pin]") {
    std::ifstream f(corpus_path("ulx3s.kicad_pcb"));
    if (!f) { SKIP("corpus not fetched"); }
    std::stringstream ss;
    ss << f.rdbuf();
    BoardIR b = import_kicad(ss.str(), builtin_stackup("default-4layer"));
    nlohmann::json report = analyze_board(b);
    // no discrete FET anywhere near the regulators: the discrete shapes stay
    // silent, and that silence is now accompanied by the evidence
    CHECK(report["meta"]["switchNodes"].empty());
    std::set<std::string> cnets;
    for (const auto& c : report["meta"]["switchNodeCandidates"])
        cnets.insert(c["net"].get<std::string>());
    CHECK(cnets == std::set<std::string>{"/power/L1", "/power/L2", "/power/L3"});

    // promoting one screens it: the commutation-loop rule now has a net to
    // work on, and provenance says the USER declared it
    ScreenerParams sp;
    sp.user_switch_nets = {"/power/L1"};
    nlohmann::json promoted = analyze_board(b, sp);
    REQUIRE(promoted["meta"]["switchNodes"].size() == 1);
    CHECK(promoted["meta"]["switchNodeSource"]["/power/L1"] == "user");
    bool loop_found = false;
    for (const auto& fi : promoted["findings"])
        if (fi["rule"] == "switch-node" || fi["rule"] == "commutation-loop")
            loop_found = true;
    CHECK(loop_found);
}

#include <faraday/Operating.hpp>

TEST_CASE("real: the MPPT's input-capacitor branch is derived from its own parts",
          "[real][operating]") {
    // ABT #797: the conducted estimate used to ask for "input capacitor" with
    // a slider while the parts sat on the board with their values in the file.
    // The commutation loop already names the capacitor that supplies the
    // current step; its rail is the branch, and the mounting inductance is
    // measured off this board's copper.
    BoardIR b = import_kicad(read_real("mppt-2420-hc.kicad_pcb"),
                             builtin_stackup("default-4layer"));
    Screener sc(b);
    auto ib = op::input_branch(b, sc);
    REQUIRE(ib.has_value());

    CHECK(!ib->caps.empty());
    CHECK(!ib->loop_cap_ref.empty());
    CHECK(!ib->switch_net.empty());
    // a real converter's input rail carries microfarads, not picofarads
    CHECK(ib->c_f > 1e-6);
    CHECK(ib->c_f < 1e-2);
    // parallel capacitors: the branch inductance is BELOW the smallest single
    // one, and the total capacitance is the sum
    double c_sum = 0, l_min = 1e30;
    for (const auto& c : ib->caps) {
        c_sum += c.c_f;
        l_min = std::min(l_min, c.esl_h + c.l_mount_h);
    }
    CHECK(ib->c_f == Approx(c_sum));
    CHECK(ib->l_h <= l_min);
    CHECK(ib->l_h > 0);
    // the point of measuring rather than assuming: on a real board most of the
    // branch inductance is the MOUNTING, not the part
    CHECK(ib->l_mount_share > 0.3);
    CHECK(ib->l_mount_share <= 1.0);
    // and the branch resonates somewhere a converter actually switches
    CHECK(ib->f_res_hz > 10e3);
    CHECK(ib->f_res_hz < 100e6);

    // A board with no converter answers "no", rather than picking a rail
    BoardIR hack = import_kicad(read_real("hackrf-one.kicad_pcb"));
    Screener hsc(hack);
    if (hsc.switch_nets().empty())
        CHECK_FALSE(op::input_branch(hack, hsc).has_value());
}

TEST_CASE("inductance values are parsed, and bare numbers are refused",
          "[operating]") {
    CHECK(*op::parse_inductance("4u7") == Approx(4.7e-6));
    CHECK(*op::parse_inductance("10uH") == Approx(10e-6));
    CHECK(*op::parse_inductance("100n") == Approx(100e-9));
    CHECK(*op::parse_inductance("2.2mH") == Approx(2.2e-3));
    CHECK(*op::parse_inductance("470p") == Approx(470e-12));
    // no unit: a 10 that means 10 uH and a 10 that means 10 nH are four
    // decades apart in every answer that uses it
    CHECK_FALSE(op::parse_inductance("10").has_value());
    CHECK_FALSE(op::parse_inductance("").has_value());
    CHECK_FALSE(op::parse_inductance("DNP").has_value());
}

#include <faraday/Pdn.hpp>

TEST_CASE("real: the MPPT's PDN rails carry their electrolytics (ABT #803)",
          "[real][pdn]") {
    // The micro-sign parse bug (fixed 2026-08-18) meant "390µF" was counted as
    // unparseable, so the converter's INPUT rail was modelled with one 100 nF
    // capacitor and none of its 780 µF of bulk. Nothing broke when it was
    // fixed because nothing pinned a real board's PDN at all — which is what
    // this test is: the missing regression guard, with the values checked
    // against the board's own BOM fields rather than recorded from the code.
    BoardIR b = import_kicad(read_real("mppt-2420-hc.kicad_pcb"),
                             builtin_stackup("default-4layer"));
    Screener sc(b);
    const auto r = pdn::discover(b, sc, pdn::Params{});

    const pdn::Rail* hv = nullptr;
    const pdn::Rail* v33 = nullptr;
    for (const auto& rail : r.rails) {
        if (rail.name == "/DCDC_HV+") hv = &rail;
        if (rail.name == "+3V3") v33 = &rail;
    }
    REQUIRE(hv != nullptr);
    REQUIRE(v33 != nullptr);

    // /DCDC_HV+: C1 and C2 are 390 µF radials (spelled with U+00B5), C3 is
    // 1 µF, C4 is 100 nF. Nothing on this rail is unparseable.
    CHECK(hv->caps.size() == 4);
    CHECK(hv->skipped_unparsed == 0);
    double c_total = 0;
    int bulk = 0;
    for (const auto& c : hv->caps) {
        c_total += c.c_f;
        if (c.c_f > 100e-6) ++bulk;
    }
    CHECK(bulk == 2);                                   // both radials present
    CHECK(c_total == Approx(781.1e-6).epsilon(1e-6));   // 390 + 390 + 1 + 0.1 µF

    // The bulk is in the CURVE, not just in the list: a 390 µF can with
    // ~4.8 nH of package + measured mounting resonates near 116 kHz, and below
    // that the rail is capacitor-dominated rather than VRM-dominated.
    for (const auto& c : hv->caps)
        if (c.c_f > 100e-6)
            CHECK(c.f_res_hz == Approx(116e3).epsilon(0.05));
    const pdn::Curve cv = pdn::curve(*hv, pdn::Params{});
    auto z_at = [&](double f) {
        double best = 1e30, z = 0;
        for (size_t i = 0; i < cv.f_hz.size(); ++i)
            if (std::abs(cv.f_hz[i] - f) < best) {
                best = std::abs(cv.f_hz[i] - f);
                z = cv.z_ohm[i];
            }
        return z;
    };
    // 780 µF against the model's 10 mΩ VRM: the rail is BELOW the VRM's own
    // impedance at 10 kHz, which it could not be with only 100 nF on it.
    CHECK(z_at(10e3) < 0.010);
    CHECK(z_at(100e3) < z_at(1e6));      // still bulk-dominated at 100 kHz

    // and the digital rail, which never had the bug, is unchanged: ten
    // ceramics, nothing refused
    CHECK(v33->caps.size() == 10);
    CHECK(v33->skipped_unparsed == 0);
}
