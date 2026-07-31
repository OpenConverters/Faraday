#pragma once
// The full board report: every screener rule PLUS the passes that need more
// than the screener itself — today the PDN anti-resonance screen, which
// stands on the pdn:: model. analyze_board lives here (not in Screener.hpp)
// because the include direction is Pdn -> Screener, and a finding pass that
// uses the PDN model can therefore never be a Screener member.

#include "Pdn.hpp"

namespace faraday {

// ---------------------------------------------------------------------------
// PDN anti-resonance screen — Franz, EMV 5th ed., §5.5 / §5.9.5
// ---------------------------------------------------------------------------
// Franz's verdict on mixed-value decoupling (100n/10n/1n) is blunt: between
// any two different series resonances a PARALLEL resonance appears, and "es
// gibt kein vernünftiges Argument für die Parallelschaltung von Kondensatoren
// mit ungleichen Serienresonanzen". The Würth checklist teaches the opposite
// (three values per rail). The screen takes no side: it computes the rail's
// impedance from the same branch model the PDN panel uses and flags only an
// ACTUAL high-Q peak. His §5.9.5 second case rides along: the resulting cap
// inductance against the plane capacitance forms the parallel resonance that
// ENDS the capacitors' authority — the decisive frequency of the decoupling.
//
// Bulk capacitors (> 2.2 uF) are excluded from the screened curve: the model
// carries a stated 15 mOhm ESR default, and an electrolytic's real ESR is the
// very thing Franz says makes the elko+ceramic pairing "meist unkritisch" —
// screening it with a ceramic's ESR would manufacture the finding.
inline std::vector<Finding> pdn_antiresonance_findings(const BoardIR& board,
                                                       const Screener& sc) {
    pdn::Params pp;
    pdn::Result r;
    try {
        r = pdn::discover(board, sc, pp);
    } catch (const std::invalid_argument&) {
        // no ground reference on this board — the rule has nothing to
        // measure against. Anything else propagates: that is a bug, not a
        // precondition.
        return {};
    }
    // The screen looks at the CAPACITOR NETWORK alone: the VRM branch and
    // the bulk caps carry real damping the default model does not know
    // (electrolytic ESR, regulator loop), and leaving the VRM inductance in
    // while excluding the bulks that damp it manufactures a low-frequency
    // peak on every rail. An effectively-open VRM removes that artifact.
    pdn::Params pscreen = pp;
    pscreen.vrm_r_ohm = 1e9;
    std::vector<Finding> out;
    for (const auto& rail : r.rails) {
        pdn::Rail ceramic = rail;
        ceramic.caps.clear();
        int excluded = 0;
        for (const auto& c : rail.caps) {
            if (c.c_f > 2.2e-6) { ++excluded; continue; }
            ceramic.caps.push_back(c);
        }
        // Franz's case is capacitors fighting capacitors (or the plane):
        // a single bias cap resonating with the plane is RF biasing, not a
        // decoupling-doctrine finding
        if (ceramic.caps.size() < 2) continue;
        const pdn::Curve cv = pdn::curve(ceramic, pscreen);

        // worst peak with real prominence. Below ~5 MHz the excluded bulk
        // capacitors would really damp whatever the ceramic-only model
        // shows, so that band is not screened.
        double pf = 0, pz = 0, floor_z = 0;
        for (const auto& [f0, z0] : cv.antires) {
            if (f0 < 5e6 || f0 > 5e8) continue;
            // prominence: against the HIGHER of the surrounding minima
            double lo = 1e30, hi = 1e30;
            for (size_t i = 0; i < cv.f_hz.size(); ++i) {
                if (cv.f_hz[i] < f0 && cv.f_hz[i] > f0 / 10.0)
                    lo = std::min(lo, cv.z_ohm[i]);
                if (cv.f_hz[i] > f0 && cv.f_hz[i] < f0 * 10.0)
                    hi = std::min(hi, cv.z_ohm[i]);
            }
            const double base = std::max(lo, hi);
            if (base > 1e29 || z0 < 5.0 * base || z0 < 0.05) continue;
            if (z0 > pz) { pz = z0; pf = f0; floor_z = base; }
        }
        if (!(pz > 0)) continue;

        // name the mechanism: mixed values, or the cap/plane handover
        std::set<long long> decades;
        for (const auto& c : ceramic.caps)
            decades.insert((long long)std::llround(std::log10(c.c_f) * 3.0));
        double l_par = 0;
        for (const auto& c : ceramic.caps)
            l_par += 1.0 / (c.esl_h + c.l_mount_h);
        l_par = 1.0 / l_par;
        double f_plane = 0;
        if (ceramic.plane_c_f > 0)
            f_plane = 1.0 / (2.0 * 3.14159265358979323846 *
                             std::sqrt(l_par * ceramic.plane_c_f));
        const bool plane_res =
            f_plane > 0 && pf > f_plane / 2.0 && pf < f_plane * 2.0;
        const bool mixed = decades.size() >= 2;

        Finding f;
        f.rule = "pdn-antiresonance";
        // a peak only matters if something drives it: with a switching
        // aggressor on the board the harmonics comb sweeps straight through
        // it; without one it is a property to review, not a defect
        const bool driven = !sc.switch_nets().empty();
        f.severity = (driven && pz > 10.0 * floor_z) ? 0.45 : 0.25;
        f.severity_label = f.severity > 0.33 ? "medium" : "low";
        f.confidence = "screening-estimate";
        f.net_a = rail.net;
        char buf[520];
        std::snprintf(
            buf, sizeof buf,
            "Rail %s: the decoupling network has a parallel resonance at "
            "%.0f MHz peaking at %.2f Ohm — %.0fx above its surroundings. "
            "%s%s Model: the PDN panel's branch model (measured mounting "
            "inductance, package ESL, stated 15 mOhm ESR default)%s.",
            rail.name.c_str(), pf * 1e-6, pz, pz / floor_z,
            plane_res
                ? "This is the capacitor/plane handover resonance — above "
                  "it the capacitors no longer act and the plane cavity "
                  "takes over, so it is the frequency limit of this "
                  "decoupling. "
                : "",
            mixed ? "The rail mixes capacitor VALUES, which is what creates "
                    "parallel resonances between their series resonances "
                    "(equal values in parallel produce none)."
                  : "",
            excluded ? " ; bulk caps > 2.2 uF excluded from the screen — "
                       "their real ESR damps rather than resonates"
                     : "");
        f.title = "PDN anti-resonance: " + rail.name + " peaks at " +
                  std::to_string((int)std::lround(pf * 1e-6)) + " MHz";
        f.detail = buf;
        f.remediation =
            "Damp it, don't dodge it: same-value capacitors in parallel "
            "instead of a decade ladder, ESR-controlled capacitors, or a "
            "small series R on one branch (Franz's RC decoupling tripled "
            "the usable bandwidth). Confirm the real curve in the PDN "
            "panel, which uses these exact branches.";
        out.push_back(std::move(f));
    }
    return out;
}

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------

inline nlohmann::json analyze_board(const BoardIR& board, ScreenerParams params = {}) {
    Screener sc(board, params);
    auto findings = sc.run(pdn_antiresonance_findings(board, sc));
    nlohmann::json fj = nlohmann::json::array();
    for (const auto& f : findings) fj.push_back(to_json(f));

    // Z0 tooltip table: one entry per (cu, width) actually routed
    std::set<std::pair<int, long long>> seen;
    nlohmann::json z0s = nlohmann::json::array();
    for (const auto& s : board.segments) {
        auto key = std::make_pair(s.cu, (long long)std::llround(s.width * 1000.0));
        if (!seen.insert(key).second) continue;
        auto z0 = sc.z0_estimate(s.cu, s.width);
        nlohmann::json e{{"cu", s.cu}, {"widthMm", s.width}};
        if (z0) e["z0Ohm"] = *z0;
        z0s.push_back(e);
    }

    return {{"faraday", "0.1.0"},
            {"board", to_json(board)},
            {"findings", fj},
            {"z0Table", z0s},
            {"meta", sc.meta()}};
}

}  // namespace faraday
