#pragma once
// The deck: what the copper adds, written where a simulator can read it.
//
// ABT #805 (epic #804). Faraday measures the parasitics that decide a
// converter's conducted spectrum — the commutation mesh's inductance, the
// input capacitor branch WITH its mounting inductance, the stray capacitance
// from the switching copper to the chassis — and until now they could only be
// read in a panel. A SPICE simulation of the same board has the opposite
// problem: it knows the devices and nothing about the copper. This writes one
// half so the other half can use it.
//
// TWO RULES SHAPE THE OUTPUT.
//
// It is TOPOLOGY-NEUTRAL. Faraday measures copper; it does not decide that a
// board is a buck. The subcircuit is four ports and the parasitics between
// them, and what switches inside is the caller's business.
//
// It says WHAT IS NOT IN IT. A .cir that silently omits device models looks
// runnable, and a transient built on it produces numbers that carry the
// authority of a simulation and the content of a guess. So the file names its
// own absences, at the top, before anything a machine reads.
//
// Every value carries provenance in the manifest — measured (off the copper),
// derived (computed from measured quantities), stated (the caller said so), or
// default (a model constant, and named as such). A deck whose reader cannot
// tell which is which will be trusted uniformly, and most of it should not be.

#include "Bench.hpp"
#include "Operating.hpp"

#include <iomanip>
#include <sstream>

namespace faraday::spice {

struct ExportOptions {
    // No default. The gap to the chassis is the one number a layout cannot
    // carry, so without it the stray-capacitance element is simply not
    // emitted — and the manifest says the area it would have used.
    std::optional<double> chassis_gap_mm;
    double chassis_eps_r = 1.0;          // 1 = air; the laminate is ~4.5
    // The frequency the switch-net trace resistance is reported at. Reported,
    // not emitted: putting it in series with the loop inductance would double
    // count copper the loop element already contains.
    double r_ac_f_hz = 1e6;
    std::string subckt = "faraday_parasitics";
};

struct Deck {
    std::string cir;
    nlohmann::json manifest;
};

// SPICE tokens cannot carry '/', spaces or parentheses, and real net names are
// full of all three ("/DC/DC/SW_NODE"). Sanitise, but keep the original in the
// manifest so a reader can map back to the board.
inline std::string spice_token(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (std::isalnum((unsigned char)c)) out += (char)std::toupper((unsigned char)c);
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "N";
    if (std::isdigit((unsigned char)out.front())) out = "N" + out;
    // SPICE RESERVES ITS GROUND NAMES. ngspice treats "0" and "gnd" (any case)
    // as the global reference node — so a board whose return net is called GND,
    // which is nearly all of them, emitted a subcircuit port that SILENTLY
    // BECAME ground. The deck still parsed, the operating point still solved,
    // and the return-line LISN measured exactly zero because every return
    // current had bypassed it through the alias. Renaming is safe: the manifest
    // carries the original net name, and nothing but SPICE reads these tokens.
    for (const char* reserved : {"GND", "GROUND", "GND!", "0"})
        if (out == reserved) return "N_" + out;
    return out;
}

namespace detail {
inline std::string eng(double v, int sig = 6) {
    std::ostringstream os;
    os << std::setprecision(sig) << v;
    return os.str();
}
inline nlohmann::json entry(const std::string& name, const std::string& quantity,
                            double value, const std::string& unit,
                            const std::string& source, const std::string& how,
                            const std::string& band = "") {
    nlohmann::json j{{"name", name}, {"quantity", quantity}, {"value", value},
                     {"unit", unit}, {"source", source}, {"how", how}};
    if (!band.empty()) j["band"] = band;
    return j;
}
}  // namespace detail

inline Deck build(const BoardIR& board, const Screener& sc,
                  const ExportOptions& o = {}) {
    if (sc.switch_nets().empty())
        throw std::invalid_argument(
            "spice: this board has no switch node, so there is no commutation "
            "mesh to annotate — an empty deck would be worse than none. Promote "
            "a candidate switch net if the converter is monolithic.");

    // The mesh: the first switch net with a derived commutation loop.
    int sw_net = -1;
    std::optional<Screener::LoopResult> loop;
    for (int n : sc.switch_nets()) {
        auto l = sc.commutation_loop(n);
        if (l && l->area_mm2 > 0 && l->hull.size() >= 3) { sw_net = n; loop = l; break; }
    }
    if (!loop)
        throw std::invalid_argument(
            "spice: a switch node was found but no commutation loop could be "
            "derived from it, so the mesh inductance this deck exists to carry "
            "does not exist either");

    // median routed width of the switch net — the same convention the
    // commutation-loop finding uses for its own inductance figure
    std::vector<double> ws;
    double sw_len_mm = 0;
    for (const auto& s : board.segments)
        if (s.net == sw_net) {
            ws.push_back(s.width);
            sw_len_mm += std::hypot(s.x2 - s.x1, s.y2 - s.y1);
        }
    double w_med = 1.0;
    if (!ws.empty()) {
        std::nth_element(ws.begin(), ws.begin() + ws.size() / 2, ws.end());
        w_med = ws[ws.size() / 2];
    }
    const double l_loop_nh = Screener::hull_loop_inductance_nh(loop->hull, w_med);

    auto branch = op::input_branch(board, sc);
    const nlohmann::json dvdt = sc.dvdt_copper();
    const double dvdt_mm2 = dvdt.value("totalMm2", 0.0);

    nlohmann::json entries = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();

    const std::string sw_tok = spice_token(board.net_name(sw_net));
    const std::string rail_tok = branch ? spice_token(branch->rail) : std::string("VIN");
    const std::string rtn_tok = branch ? spice_token(branch->return_net) : std::string("RTN");

    std::ostringstream f;
    f << "* ===================================================================\n"
      << "*  Faraday — parasitic-annotated netlist\n"
      << "*  format: " << board.copper_names.size() << " copper layers, stackup: "
      << board.stackup.source << "\n"
      << "*  switch net: " << board.net_name(sw_net)
      << "   mesh area: " << detail::eng(loop->area_mm2, 4) << " mm^2\n"
      << "*\n"
      << "*  WHAT THIS IS: the copper, as measured. Every element below was\n"
      << "*  extracted from the layout — the mesh inductance from the derived\n"
      << "*  commutation loop, the input branch from the parts actually placed\n"
      << "*  with their mounting inductance measured pad-to-via, the stray\n"
      << "*  capacitance from the switching copper's own area.\n"
      << "*\n"
      << "*  WHAT IS NOT IN IT, and must be supplied before any transient run\n"
      << "*  means anything:\n"
      << "*    - device models: the FET, the diode, Coss / Qg / trr\n"
      << "*    - the gate loop and its drive (dv/dt is set there, not here)\n"
      << "*    - the control loop\n"
      << "*    - the magnetics beyond terminal inductance\n"
      << "*    - anything off the board: cable, enclosure, heatsink\n"
      << "*  This file is deliberately not runnable on its own. A deck that\n"
      << "*  omitted these silently would look runnable and would produce\n"
      << "*  numbers with the authority of a simulation and the content of a\n"
      << "*  guess.\n*\n"
      << "*  HOW TO CONNECT IT\n"
      << "*    " << rail_tok << " / " << rtn_tok
      << "  : the rail as it ARRIVES on the board — put the supply,\n"
      << "*      or the LISN, here. The input capacitor bank hangs off this node.\n"
      << "*    " << rail_tok << "_CELL   : the same rail AFTER the commutation mesh's\n"
      << "*      inductance — attach the high side of the switching cell here.\n"
      << "*    " << sw_tok << " : the switch node itself.\n"
      << "*    CHASSIS : earth/metalwork, for the common-mode path.\n"
      << "* ===================================================================\n\n"
      << ".subckt " << o.subckt << " " << rail_tok << " " << rail_tok
      << "_CELL " << sw_tok << " " << rtn_tok << " CHASSIS\n\n";

    // ---- the commutation mesh -------------------------------------------
    f << "* The commutation mesh's self-inductance, measured off the copper:\n"
      << "* Grover's equivalent-rectangle over the derived loop hull, conductor\n"
      << "* width " << detail::eng(w_med, 3) << " mm (median of the switch net).\n"
      << "* Place it where the current step traverses it — in series between the\n"
      << "* input capacitor bank and the switching cell. Screening estimate,\n"
      << "* ~15%, validated against FastHenry.\n"
      << "L_MESH " << rail_tok << " " << rail_tok << "_CELL "
      << detail::eng(l_loop_nh) << "n\n\n";
    entries.push_back(detail::entry(
        "L_MESH", "inductance", l_loop_nh, "nH", "measured",
        "Grover equivalent-rectangle over the commutation-loop hull derived by "
        "current-switching analysis (Screener::hull_loop_inductance_nh)",
        "~15% vs FastHenry"));
    entries.push_back(detail::entry(
        "mesh_area", "area", loop->area_mm2, "mm^2", "measured",
        "enclosed area of the derived commutation-loop hull"));

    // ---- the input capacitor branch --------------------------------------
    if (branch && !branch->caps.empty()) {
        f << "* The input capacitor branch, part by part. C is the board's own\n"
          << "* value field; ESL is by package size; the MOUNTING inductance is\n"
          << "* measured pad-to-via on this layout and is usually the larger of\n"
          << "* the two. ESR is NOT on the board — the 15 mOhm below is a model\n"
          << "* constant, and for an electrolytic it is optimistic.\n"
          << "* Rail: " << branch->rail << "  (named by loop capacitor "
          << branch->loop_cap_ref << ")\n";
        for (const auto& c : branch->caps) {
            const std::string t = spice_token(c.ref);
            const double l_h = c.esl_h + c.l_mount_h;
            f << "C_" << t << " " << rail_tok << " N_" << t << "_A "
              << detail::eng(c.c_f) << "\n"
              << "R_" << t << " N_" << t << "_A N_" << t << "_B 15m\n"
              << "L_" << t << " N_" << t << "_B " << rtn_tok << " "
              << detail::eng(l_h * 1e9) << "n"
              << "   $ ESL " << detail::eng(c.esl_h * 1e9, 3) << "n + mount "
              << detail::eng(c.l_mount_h * 1e9, 3) << "n (vias "
              << detail::eng(c.via_d1_mm, 3) << "/" << detail::eng(c.via_d2_mm, 3)
              << " mm)\n";
            entries.push_back(detail::entry(
                "C_" + t, "capacitance", c.c_f, "F", "measured",
                "the part's own value field on this board (" + c.package + ")"));
            entries.push_back(detail::entry(
                "L_" + t, "inductance", l_h * 1e9, "nH", "derived",
                "package ESL by case size + mounting inductance measured "
                "pad-to-via (~0.8 nH/mm escape, 0.3 nH per barrel)"));
            entries.push_back(detail::entry(
                "R_" + t, "resistance", 0.015, "ohm", "default",
                "the PDN model's stated 15 mOhm; ESR is not carried by any "
                "layout and a large electrolytic's is several times this"));
        }
        f << "\n";
    } else {
        warnings.push_back(
            "no input-capacitor branch could be derived, so the deck carries "
            "no differential-mode source impedance — the DM path is the one "
            "thing this file exists to supply, and it is missing here");
        f << "* NO INPUT CAPACITOR BRANCH could be derived from this board.\n"
          << "* The differential-mode path is therefore ABSENT from this deck.\n\n";
    }

    // ---- the common-mode source term --------------------------------------
    if (o.chassis_gap_mm && dvdt_mm2 > 0) {
        const double c_stray =
            emc::chassis_stray_c_f(dvdt_mm2, *o.chassis_gap_mm, o.chassis_eps_r);
        f << "* The common-mode path: " << detail::eng(dvdt_mm2, 4)
          << " mm^2 of dv/dt copper at a stated " << detail::eng(*o.chassis_gap_mm, 3)
          << " mm from the chassis (eps_r " << detail::eng(o.chassis_eps_r, 3)
          << ").\n"
          << "* Parallel-plate, fringing ignored, so this is a FLOOR: a heatsink\n"
          << "* on the device tab and the transformer's inter-winding capacitance\n"
          << "* add paths no layout can see.\n"
          << "C_STRAY " << sw_tok << " CHASSIS " << detail::eng(c_stray) << "\n\n";
        entries.push_back(detail::entry(
            "C_STRAY", "capacitance", c_stray, "F", "derived",
            "eps0*eps_r*A/d with A the dv/dt copper area measured off this "
            "board and d the stated chassis gap", "lower bound"));
        entries.push_back(detail::entry(
            "chassis_gap", "length", *o.chassis_gap_mm, "mm", "stated",
            "the one quantity a layout cannot carry"));
    } else {
        f << "* NO COMMON-MODE SOURCE ELEMENT: the dv/dt copper measures "
          << detail::eng(dvdt_mm2, 4) << " mm^2, but the distance to the\n"
          << "* chassis was not stated (--chassis-gap-mm), and it is not a\n"
          << "* number this tool will invent.\n\n";
        if (dvdt_mm2 > 0)
            warnings.push_back(
                "C_stray omitted: no chassis gap was stated. The common-mode "
                "path is absent from this deck.");
    }
    entries.push_back(detail::entry(
        "dvdt_copper_area", "area", dvdt_mm2, "mm^2", "measured",
        "tracks, pads and pours on every switch net (Screener::dvdt_copper)"));

    f << ".ends " << o.subckt << "\n";

    // ---- reported, not emitted -------------------------------------------
    if (sw_len_mm > 0) {
        const double r_ac = bench::r_ac_per_m(w_med * 1e-3, 35e-6, o.r_ac_f_hz) *
                            sw_len_mm * 1e-3;
        f << "\n* Reported, deliberately NOT emitted as an element: the switch\n"
          << "* net's own trace resistance is " << detail::eng(r_ac * 1e3, 3)
          << " mOhm at " << detail::eng(o.r_ac_f_hz * 1e-6, 3) << " MHz ("
          << detail::eng(sw_len_mm, 4) << " mm of "
          << detail::eng(w_med, 3) << " mm copper, skin effect included).\n"
          << "* Putting it in series with L_MESH would count the same copper\n"
          << "* twice — the mesh element already contains this conductor.\n"
          << "* ROUTED SEGMENTS ONLY: where the switch node is a POUR — which on\n"
          << "* a power board it usually is — most of its copper is not in this\n"
          << "* figure, and the real resistance is lower.\n";
        entries.push_back(detail::entry(
            "R_switch_trace", "resistance", r_ac, "ohm", "derived",
            "skin-effect resistance of the routed switch net at the stated "
            "frequency (bench::r_ac_per_m x routed length) — reported only"));
    }

    Deck d;
    d.cir = f.str();
    d.manifest = {
        {"faraday", "0.2.0"},
        {"subckt", o.subckt},
        // EXACTLY the tokens on the .subckt line, in order. They drifted apart
        // once already (the CELL port was added to the deck and not to the
        // manifest, and the harness that consumed the manifest silently wired
        // the subcircuit wrong while still parsing) — a test now pins that the
        // two agree, because nothing else can catch it.
        {"ports", {rail_tok, rail_tok + "_CELL", sw_tok, rtn_tok, "CHASSIS"}},
        {"nets", {{"rail", branch ? branch->rail : ""},
                  {"switch", board.net_name(sw_net)},
                  {"return", branch ? branch->return_net : ""}}},
        {"stackupSource", board.stackup.source},
        {"entries", entries},
        {"warnings", warnings},
        {"absent",
         {"device models (FET, diode: Coss, Qg, trr)",
          "the gate loop and its drive — dv/dt is set there",
          "the control loop",
          "magnetics beyond terminal inductance",
          "everything off the board: cable, enclosure, heatsink"}},
        {"note",
         "Parasitics extracted from the layout. Provenance per entry: measured "
         "= off the copper, derived = computed from measured quantities, "
         "stated = supplied by the caller, default = a model constant. This "
         "deck is not runnable on its own by design; see 'absent'."}};
    return d;
}

}  // namespace faraday::spice
