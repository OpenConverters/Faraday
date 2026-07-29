#pragma once
// Mechanical fix generation, v1: STITCHING VIAS at unstitched layer changes.
//
// Scope discipline, because this writes copper. The generator never modifies
// the input — it emits a NEW .kicad_pcb text with vias appended, and only
// proposes a via where all of this holds:
//
//   * the signal net changes layers and no reference via spans that pair
//     within reach (the same test the return-path map flags);
//   * the reference net's pour actually COVERS the candidate point on at
//     least two copper layers — a stitch that lands in one pour and air is
//     a barrel to nowhere, so a board with a single reference plane gets
//     ZERO proposals and the honest explanation that it is a stackup
//     problem, not a missing via;
//   * the whole via (pad + clearance) clears every foreign-net segment, pad,
//     via and pour on every copper layer — a fix that shorts the board is
//     worse than no tool at all;
//   * the via STYLE (size/drill) is copied from the board's own most common
//     reference via — no invented defaults; a board with no reference vias
//     to copy from is refused.
//
// apply() re-imports its own output and throws unless exactly the proposed
// vias appeared on the right net — the generator proves its work or fails
// loudly, never silently emits a broken file.

#include "Import.hpp"
#include "ReturnPath.hpp"
#include "Screener.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace faraday::fixes {

struct StitchVia {
    double x = 0, y = 0;
    double size = 0, drill = 0;
    int net = -1;                 // reference net (board net id = KiCad net nr)
    std::string net_name;
    std::string near_net;         // the signal net whose return this stitches
};

struct StitchPlan {
    std::vector<StitchVia> vias;
    // why nothing (or less than everything) was proposed — shown to the user
    std::vector<std::string> notes;
    size_t unstitched_seen = 0;
};

namespace detail {

// Foreign-copper clearance: the via pad must not touch any other net's
// copper on ANY layer (it is a through via). Pours are checked by sampling
// the clearance circle — pessimistic in the right direction.
inline bool location_clear(const BoardIR& b, double x, double y,
                           double pad_radius_mm, int own_net) {
    const double clr = pad_radius_mm + 0.2;   // 0.2 mm minimum clearance
    for (const auto& s : b.segments) {
        if (s.net == own_net) continue;
        // distance point-to-segment
        const double dx = s.x2 - s.x1, dy = s.y2 - s.y1;
        const double len2 = dx * dx + dy * dy;
        double t = len2 > 0 ? ((x - s.x1) * dx + (y - s.y1) * dy) / len2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        const double d = std::hypot(x - (s.x1 + t * dx), y - (s.y1 + t * dy));
        if (d < clr + s.width / 2) return false;
    }
    for (const auto& p : b.pads) {
        if (p.net == own_net) continue;
        if (std::abs(x - p.x) < clr + p.w / 2 &&
            std::abs(y - p.y) < clr + p.h / 2)
            return false;
    }
    for (const auto& v : b.vias) {
        if (v.net == own_net) continue;
        if (std::hypot(x - v.x, y - v.y) < clr + v.size / 2) return false;
    }
    for (const auto& z : b.zones) {
        if (z.net == own_net) continue;
        for (int k = 0; k < 8; ++k) {
            const double a = k * M_PI / 4;
            if (z.contains(x + clr * std::cos(a), y + clr * std::sin(a)))
                return false;
        }
    }
    return true;
}

// The reference pour must contain the point on >= 2 copper layers, or the
// stitch connects nothing to nothing.
inline int pour_layer_count(const BoardIR& b, double x, double y, int net) {
    std::set<int> layers;
    for (const auto& z : b.zones)
        if (z.net == net && z.contains(x, y)) layers.insert(z.cu);
    return (int)layers.size();
}

}  // namespace detail

inline StitchPlan propose_stitching(const BoardIR& b, const Screener& sc,
                                    double reach_mm = 25.0) {
    StitchPlan plan;

    const auto& layers = sc.layer_models();
    std::set<int> ref_nets;
    for (const auto& lm : layers)
        if (lm.is_plane && lm.plane_net >= 0) ref_nets.insert(lm.plane_net);
    if (ref_nets.empty()) {
        plan.notes.push_back(
            "no reference plane on this board — there is nothing to stitch");
        return plan;
    }
    const rp::ViaIndex ref_vias(b, ref_nets);

    // via style: the most common (size, drill) among reference vias
    std::map<std::pair<double, double>, int> styles;
    for (const auto& v : b.vias)
        if (ref_nets.count(v.net) && v.drill > 0)
            ++styles[{v.size, v.drill}];
    if (styles.empty()) {
        plan.notes.push_back(
            "the board has no reference-net vias to copy a via style from — "
            "refusing to invent pad and drill sizes");
        return plan;
    }
    auto style = std::max_element(styles.begin(), styles.end(),
                                  [](auto& a, auto& c) {
                                      return a.second < c.second;
                                  })->first;

    std::set<std::pair<long, long>> planned;   // avoid stacking proposals
    for (const auto& v : b.vias) {
        if (v.net <= 0 || ref_nets.count(v.net) || v.cu_from == v.cu_to)
            continue;   // only signal-net layer changes
        const double have = ref_vias.nearest_spanning(v.x, v.y, v.cu_from,
                                                      v.cu_to, reach_mm);
        if (have < reach_mm) continue;   // already stitched
        ++plan.unstitched_seen;

        bool placed = false;
        for (double r : {1.0, 1.5, 2.0, 3.0, 4.0}) {
            for (int k = 0; k < 12 && !placed; ++k) {
                const double a = k * M_PI / 6;
                const double x = v.x + r * std::cos(a);
                const double y = v.y + r * std::sin(a);
                for (int net : ref_nets) {
                    if (detail::pour_layer_count(b, x, y, net) < 2) continue;
                    if (!detail::location_clear(b, x, y, style.first / 2, net))
                        continue;
                    const auto key = std::make_pair(std::lround(x * 2),
                                                    std::lround(y * 2));
                    if (planned.count(key)) { placed = true; break; }
                    planned.insert(key);
                    plan.vias.push_back({x, y, style.first, style.second, net,
                                         b.net_name(net), b.net_name(v.net)});
                    placed = true;
                    break;
                }
            }
            if (placed) break;
        }
        if (!placed)
            plan.notes.push_back(
                "no stitch possible near (" + std::to_string(v.x) + ", " +
                std::to_string(v.y) + ") for net '" + b.net_name(v.net) +
                "': no point nearby has reference copper on two layers with "
                "clearance — on a single-plane stackup this is a stackup "
                "problem, not a missing via");
    }
    return plan;
}

// Emit the patched .kicad_pcb text. KiCad only — set formats cannot
// round-trip. The output is verified by re-import before it is returned.
inline std::string apply_stitching(const std::string& kicad_text,
                                   const BoardIR& b,
                                   const std::vector<StitchVia>& vias) {
    if (vias.empty())
        throw BoardError("fixes: nothing to apply");
    if (detect_format(kicad_text) != BoardFormat::Kicad)
        throw BoardError(
            "fixes: writeback needs the original .kicad_pcb — Gerber/ODB++ "
            "sets cannot round-trip");
    const size_t close = kicad_text.rfind(')');
    if (close == std::string::npos)
        throw BoardError("fixes: malformed board text");

    std::ostringstream add;
    add.setf(std::ios::fixed);
    add.precision(4);
    for (const auto& v : vias)
        add << "  (via (at " << v.x << " " << v.y << ") (size " << v.size
            << ") (drill " << v.drill << ") (layers \"" << b.copper_names.front()
            << "\" \"" << b.copper_names.back() << "\") (net " << v.net
            << "))\n";
    std::string out = kicad_text.substr(0, close) + add.str() +
                      kicad_text.substr(close);

    // prove the work: the patched board must import with exactly these vias
    // added, each on its reference net. Re-import with the board's own
    // RESOLVED stackup — the file may not carry one.
    BoardIR check = import_board(out, b.stackup);
    if (check.vias.size() != b.vias.size() + vias.size())
        throw BoardError("fixes: patched board did not gain the proposed vias");
    for (const auto& v : vias) {
        bool found = false;
        for (const auto& cv : check.vias)
            found = found || (std::abs(cv.x - v.x) < 1e-3 &&
                              std::abs(cv.y - v.y) < 1e-3 && cv.net == v.net);
        if (!found)
            throw BoardError(
                "fixes: a proposed via did not survive re-import on net '" +
                v.net_name + "' — refusing to emit an unverified file");
    }
    return out;
}

}  // namespace faraday::fixes
