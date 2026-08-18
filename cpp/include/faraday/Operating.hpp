#pragma once
// The operating point, as far as a layout can supply it.
//
// The emissions estimate needs a switching waveform and an input-capacitor
// branch. Four of those numbers — switched current, switching frequency, edge
// rate, bus voltage — are properties of the CIRCUIT, and no layout carries
// them; they are asked for, or handed over by a tool that designed the
// converter (Kirchhoff/Heaviside, via the same URL-fragment route the Hertz
// bridge uses in the other direction).
//
// The input capacitor is different. It IS on the board: the parts are placed,
// their values are in the file, and their mounting inductance is a measurable
// property of the copper — the PDN pass already measures exactly that. A
// slider asking for "input capacitor: 10 uF" while three 10 uF parts sit in
// the commutation loop is a tool ignoring what it can see.
//
// WHICH capacitors. Not "the ones on the input net" by name — the commutation
// loop already identified the capacitor that supplies the current step
// (Franz's Stromumschaltanalyse, CriticalMesh.hpp). Its non-return net is the
// input rail, and every capacitor between that rail and the return sits in
// parallel across the same branch. That is the impedance the differential-mode
// residual appears across, which is what the conducted estimate needs.
//
// WHAT IS STILL ASSUMED, and stated: ESR. Nothing in a layout carries it, so
// the PDN model's 15 mOhm default rides along and is reported as an
// assumption. A bulk electrolytic's real ESR is much larger (and damps rather
// than resonates), so the derived branch is PESSIMISTIC around resonance —
// the safe direction for an emissions estimate, but not a measurement.

#include "Pdn.hpp"

namespace faraday::op {

struct BranchCap {
    std::string ref, package;
    double c_f = 0, esl_h = 0, l_mount_h = 0;
    double via_d1_mm = 0, via_d2_mm = 0;
    bool no_via = false;
};

struct InputBranch {
    int rail_net = -1;
    std::string rail;             // net name
    std::string return_net;       // what it was measured against
    std::vector<BranchCap> caps;
    double c_f = 0;               // parallel sum
    double l_h = 0;               // parallel of (package ESL + measured mount)
    double esr_ohm = 0;           // parallel of the stated default
    double l_mount_share = 0;     // fraction of l_h that is MOUNTING, not part
    double f_res_hz = 0;          // series resonance of the whole branch
    int unparsed = 0;             // caps whose value string was refused
    std::string loop_cap_ref;     // the capacitor the commutation loop named
    std::string switch_net;
};

// nullopt when the board does not answer the question: no switch node, no
// derived commutation loop, or a loop capacitor that bridges two pours (a
// domain stitch, not an input capacitor). No fallback rail is guessed — a
// wrong branch would silently move every conducted number.
inline std::optional<InputBranch> input_branch(const BoardIR& board,
                                               const Screener& sc) {
    if (sc.switch_nets().empty()) return std::nullopt;

    pdn::Params pp;
    pdn::Result pr;
    try {
        pr = pdn::discover(board, sc, pp);
    } catch (const std::invalid_argument&) {
        return std::nullopt;      // no ground, or no parseable capacitor
    }

    for (int sw : sc.switch_nets()) {
        auto loop = sc.commutation_loop(sw);
        if (!loop || loop->cap_ref.empty()) continue;
        // The loop capacitor's rail: its net that is not the RETURN. Not
        // "not poured" — a converter's input rail is very often a pour of its
        // own (the MPPT's /DCDC_HV+ is), and excluding poured nets rejected
        // exactly the boards this exists for. A capacitor whose two nets are
        // both the return is a domain stitch and is skipped below.
        int rail_net = -1;
        for (int n : loop->cap_nets)
            if (n > 0 && n != pr.gnd_net) rail_net = n;
        if (rail_net < 0) continue;

        for (const auto& rail : pr.rails) {
            if (rail.net != rail_net) continue;
            InputBranch ib;
            ib.rail_net = rail_net;
            ib.rail = rail.name;
            ib.return_net = pr.gnd_name;
            ib.unparsed = rail.skipped_unparsed;
            ib.loop_cap_ref = loop->cap_ref;
            ib.switch_net = board.net_name(sw);
            double y_l = 0, y_r = 0, l_mount_weighted = 0;
            for (const auto& c : rail.caps) {
                if (!(c.c_f > 0)) continue;
                const double l = c.esl_h + c.l_mount_h;
                if (!(l > 0) || !(c.esr_ohm > 0)) continue;
                ib.caps.push_back({c.ref, c.package, c.c_f, c.esl_h, c.l_mount_h,
                                   c.via_d1_mm, c.via_d2_mm, c.no_via});
                ib.c_f += c.c_f;
                y_l += 1.0 / l;
                y_r += 1.0 / c.esr_ohm;
                l_mount_weighted += c.l_mount_h / l;
            }
            if (ib.caps.empty()) continue;
            ib.l_h = 1.0 / y_l;
            ib.esr_ohm = 1.0 / y_r;
            ib.l_mount_share = l_mount_weighted / (double)ib.caps.size();
            ib.f_res_hz = 1.0 / (2.0 * 3.14159265358979323846 *
                                 std::sqrt(ib.l_h * ib.c_f));
            return ib;
        }
    }
    return std::nullopt;
}

// Inductance parsing lives in Values.hpp with its capacitance sibling; the
// alias keeps op::parse_inductance meaningful at the call sites that think in
// operating-point terms.
using values::parse_inductance;

}  // namespace faraday::op
