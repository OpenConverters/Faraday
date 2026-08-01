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
