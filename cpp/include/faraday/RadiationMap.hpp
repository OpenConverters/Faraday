#pragma once
// Which copper on this board radiates, and how much of the total each piece is
// responsible for.
//
// WHAT THIS IS. Every trace and its return path form a loop. Over an intact
// plane that loop is the trace length times the dielectric height — square
// MICROMETRES, which is why a well-referenced trace barely radiates. Take the
// reference away and the return current has to find another way round; the loop
// becomes the trace length times the distance to whatever plane IS there, or
// times the board itself when there is none. That single number, area times
// current, is the whole differential-mode radiation design rule, and Faraday
// already knows both factors: the geometry from the copper, the reference
// height from the stackup.
//
// So the map is an ATTRIBUTION, not a field simulation. It answers "which
// twenty traces are eighty percent of my differential-mode radiation", which is
// the question that changes a layout. It is deliberately not rendered in
// absolute field units per pixel: coloured that way it would look like a
// full-wave solve, and it is not one.
//
// WHAT IT IS NOT. Not a full-wave solve — FastCap and FastHenry could not do
// this either, being quasi-static, and neither has a retarded potential in it.
// No radiation from the planes themselves, no cavity resonance between plane
// pairs, no common-mode current on cables (that is the budget in Emissions.hpp,
// and it is usually the bigger term). Contributions are summed INCOHERENTLY:
// the relative phase of a hundred loops scattered over a board is not knowable
// from geometry, and a power sum is the defensible aggregate rather than a
// prediction.
//
// THE DOMINANT UNCERTAINTY IS CURRENT. Faraday does not know what any net
// carries. Signal traces are given swing / Z0, which it can compute; nets the
// screener identified as switch nodes are given the switched current the user
// supplies. Both are assumptions and both scale the answer linearly, so the UI
// has to show them, not bury them.

#include "Emissions.hpp"
#include "Screener.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <vector>

namespace faraday::radmap {

// Vias, indexed on a coarse grid so a nearest-stitch query is affordable per
// segment on a 4000-segment board.
struct ViaIndex {
    struct Entry { double x, y; int cu_from, cu_to; };
    double cell = 5.0;                       // mm
    std::map<std::pair<int,int>, std::vector<Entry>> grid;

    ViaIndex(const BoardIR& b, const std::set<int>& ref_nets) {
        for (const auto& v : b.vias) {
            if (!ref_nets.count(v.net)) continue;
            grid[{(int)std::floor(v.x / cell), (int)std::floor(v.y / cell)}]
                .push_back({v.x, v.y, v.cu_from, v.cu_to});
        }
    }

    // Distance to the nearest reference-net via that SPANS the layer pair the
    // return current has to cross. A via that does not span the pair is no use
    // to that return, which is the whole point of checking the span.
    double nearest_spanning(double x, double y, int cu_a, int cu_b,
                            double max_mm) const {
        const int lo = std::min(cu_a, cu_b), hi = std::max(cu_a, cu_b);
        const int rings = (int)std::ceil(max_mm / cell);
        const int gx = (int)std::floor(x / cell), gy = (int)std::floor(y / cell);
        double best = max_mm;
        for (int dx = -rings; dx <= rings; ++dx)
            for (int dy = -rings; dy <= rings; ++dy) {
                auto it = grid.find({gx + dx, gy + dy});
                if (it == grid.end()) continue;
                for (const auto& e : it->second) {
                    if (std::min(e.cu_from, e.cu_to) > lo ||
                        std::max(e.cu_from, e.cu_to) < hi) continue;
                    const double d = std::hypot(e.x - x, e.y - y);
                    if (d < best) best = d;
                }
            }
        return best;
    }
};

// A shield can drawn over part of the board. Copper inside it is attenuated
// by the can's shielding effectiveness AT THE FREQUENCY IN QUESTION, which is
// the honest way to model it: at HF the metal is opaque and only the seam
// matters, at LF the wall binds and permeability is worth tens of dB.
struct ShieldRect {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;   // mm
    double se_db = 0;                        // computed by Shielding.hpp
    bool contains(double x, double y) const {
        return x >= std::min(x1, x2) && x <= std::max(x1, x2) &&
               y >= std::min(y1, y2) && y <= std::max(y1, y2);
    }
};

struct MapParams {
    // source waveform, shared by every net — a screening assumption, stated
    double f_sw_hz = 500e3;
    double duty = 0.4;
    double rise_s = 20e-9;
    // per net class
    double swing_v = 3.3;         // logic swing, drives I = swing / Z0
    double sw_current_a = 10.0;   // switched current on identified switch nodes
    double default_z0 = 50.0;     // when a layer has no reference to derive one
    // measurement
    double r_m = 3.0;
    bool ground_reflection = true;
    // A layer with no reference plane at all has no defined return path. Rather
    // than drop those traces — they are the worst radiators on the board, and
    // hiding them would be exactly backwards — the loop is closed over this
    // height and the count is reported so the reader knows how much of the
    // total rests on it.
    double no_reference_height_mm = 1.6;
    // How far a return current is allowed to detour looking for a stitch
    // before the loop is simply closed over the board. Beyond this the exact
    // distance stops mattering — it is already a bad loop.
    double max_return_detour_mm = 25.0;
    // Shields drawn by the user. Empty by default: a board has no can until
    // someone says it does.
    std::vector<ShieldRect> shields;
};

struct SegmentContribution {
    double shield_db = 0;         // attenuation applied, 0 when unshielded
    // Return-path detour at a LAYER CHANGE. When a signal changes layers its
    // return must transfer between reference planes, and it can only do that
    // through a stitching via or an interplane capacitor. With none nearby the
    // return travels a long way round — a large loop, and one of the classic
    // ways a board fails. Before this the map added NO penalty for it at all,
    // so a via-heavy signal with the nearest ground stitch 20 mm away coloured
    // identically to one that never changed layers. That is the map being
    // silent exactly where it should have been loudest.
    double stitch_distance_mm = 0;
    double return_area_m2 = 0;    // extra loop the detour encloses
    bool layer_change = false;
    bool unstitched = false;      // no reference via within the search radius
    // Contribution per unit length — height x current, stripped of the
    // segment's own length. THIS is what the map should be coloured by: a
    // router splits one trace into a dozen segments, so per-segment
    // contribution makes a single net render as a dozen different shades and
    // buries 89% of the copper in the dark end of the ramp. Loudness per
    // millimetre is uniform along a net, which is both legible and the honest
    // reading of "how noisy is this piece of copper".
    double e_per_m = 0;
    double area_m2 = 0;
    double current_a = 0;
    double e_v_per_m = 0;
    double height_mm = 0;
    bool no_reference = false;
    bool switch_node = false;
    bool over_void = false;             // the plane is missing under part of it
    double unreferenced_fraction = 0;   // how much of it
};

struct MapResult {
    std::vector<SegmentContribution> segments;   // parallel to board.segments
    double total_v_per_m = 0;      // incoherent (power) sum
    double total_dbuv_m = 0;
    double max_e_v_per_m = 0;
    double max_e_per_m = 0;      // peak loudness per unit length, for colouring
    size_t counted = 0, no_reference_count = 0, over_void_count = 0;
    size_t layer_change_count = 0, unstitched_count = 0, shielded_count = 0;
    double total_unshielded_v_per_m = 0;   // for the before/after comparison
    double no_reference_share = 0;  // fraction of total POWER from those
    std::vector<size_t> top;        // segment indices, largest first
};

// Height of the loop a segment on copper layer `cu` closes over: the distance
// to the nearest reference plane, or the fallback when the layer has none.
inline double loop_height_mm(const LayerModel& lm, const MapParams& p,
                             bool* no_reference) {
    const bool up = lm.ref_up >= 0, dn = lm.ref_dn >= 0;
    if (up && dn) { *no_reference = false; return std::min(lm.h_up, lm.h_dn); }
    if (up) { *no_reference = false; return lm.h_up; }
    if (dn) { *no_reference = false; return lm.h_dn; }
    *no_reference = true;
    return p.no_reference_height_mm;
}

// Is there reference-plane copper directly beneath (or above) this point?
// Bounding boxes first, then the even-odd ray cast — the same two-step the
// plane-crossing rule uses, and the reason this is affordable per segment.
struct PlaneLookup {
    struct Box { double x1, y1, x2, y2; const ZonePoly* z; };
    std::vector<std::vector<Box>> by_layer;

    PlaneLookup(const BoardIR& b, size_t n_layers) : by_layer(n_layers) {
        for (const auto& z : b.zones) {
            if (z.cu < 0 || (size_t)z.cu >= n_layers) continue;
            Box box{1e30, 1e30, -1e30, -1e30, &z};
            for (const auto& q : z.pts) {
                box.x1 = std::min(box.x1, q.x); box.y1 = std::min(box.y1, q.y);
                box.x2 = std::max(box.x2, q.x); box.y2 = std::max(box.y2, q.y);
            }
            by_layer[z.cu].push_back(box);
        }
    }
    bool covered(int cu, double x, double y) const {
        if (cu < 0 || (size_t)cu >= by_layer.size()) return false;
        for (const auto& b : by_layer[cu]) {
            if (x < b.x1 || x > b.x2 || y < b.y1 || y > b.y2) continue;
            if (b.z->contains(x, y)) return true;
        }
        return false;
    }
};

inline MapResult compute(const BoardIR& board, const Screener& screener,
                         const MapParams& p) {
    if (!(p.r_m > 0) || !(p.rise_s > 0) || !(p.f_sw_hz > 0))
        throw std::invalid_argument("radmap: distance, edge and f_sw must be > 0");
    if (!(p.no_reference_height_mm > 0))
        throw std::invalid_argument("radmap: the fallback loop height must be > 0");
    if (!(p.default_z0 > 0))
        throw std::invalid_argument("radmap: default Z0 must be > 0");

    const std::vector<LayerModel>& layers = screener.layer_models();
    const double gain = p.ground_reflection ? emc::GROUND_REFLECTION : 1.0;
    const PlaneLookup planes(board, layers.size());

    // Which nets a return current may actually use, and where their vias are.
    std::set<int> ref_nets;
    for (const auto& lm : layers)
        if (lm.is_plane && lm.plane_net >= 0) ref_nets.insert(lm.plane_net);
    const ViaIndex vias(board, ref_nets);

    // Where each net changes layers: a via on the signal's OWN net. That is
    // the point at which its return has to hop planes.
    std::map<int, std::vector<const Via*>> net_vias;
    for (const auto& v : board.vias)
        if (v.net > 0 && !ref_nets.count(v.net) && v.cu_from != v.cu_to)
            net_vias[v.net].push_back(&v);

    MapResult r;
    r.segments.resize(board.segments.size());
    double power = 0, power_no_ref = 0, power_unshielded = 0;

    for (size_t i = 0; i < board.segments.size(); ++i) {
        const Segment& s = board.segments[i];
        if (s.cu < 0 || (size_t)s.cu >= layers.size()) continue;
        const double dx = s.x2 - s.x1, dy = s.y2 - s.y1;
        const double len_mm = std::sqrt(dx * dx + dy * dy);
        if (!(len_mm > 0)) continue;

        SegmentContribution c;
        c.height_mm = loop_height_mm(layers[s.cu], p, &c.no_reference);

        // THE PART THAT MAKES THIS A MAP. The stackup height is what the
        // reference plane would give you IF it were actually there under this
        // particular copper. Sample along the segment and check. Where the
        // plane is missing — a void, a split, the edge of a pour — the return
        // current cannot run underneath, it detours, and the loop closes over
        // the next plane down or over the board itself.
        //
        // Without this the colour varies only per LAYER and per net class, so
        // the picture reduces to "the switch nets are hot", which the findings
        // list already said. With it, a signal trace crossing a void outshines
        // a switch node over solid ground — which is the truth.
        const LayerModel& lm = layers[s.cu];
        const int ref_cu = (lm.ref_dn >= 0) ? lm.ref_dn : lm.ref_up;
        if (ref_cu >= 0) {
            const int samples = std::clamp((int)std::ceil(len_mm / 0.5), 2, 24);
            int uncovered = 0;
            for (int k = 0; k < samples; ++k) {
                const double f = (k + 0.5) / samples;
                if (!planes.covered(ref_cu, s.x1 + dx * f, s.y1 + dy * f)) ++uncovered;
            }
            c.unreferenced_fraction = (double)uncovered / samples;
            if (uncovered > 0) {
                // the fallback the return has to use where the plane is gone
                double alt = p.no_reference_height_mm;
                for (size_t j = 0; j < layers.size(); ++j) {
                    if ((int)j == ref_cu || (int)j == s.cu || !layers[j].is_plane)
                        continue;
                    double h = 0, eps = 0;
                    board.stackup.dielectric_between(std::min((size_t)s.cu, j),
                                                     std::max((size_t)s.cu, j), h, eps);
                    if (h > c.height_mm) { alt = std::min(alt, h); }
                }
                // area-weighted: the covered part keeps its tight loop, the
                // uncovered part closes over the detour
                c.height_mm = c.height_mm * (1.0 - c.unreferenced_fraction) +
                              alt * c.unreferenced_fraction;
                c.over_void = true;
            }
        }
        // ---- return path through vias ----
        // A layer change on this net near this segment means the return has to
        // cross between planes. The nearest reference-net via that spans the
        // same pair is how far it detours; that detour times the plane
        // separation is loop area the trace geometry alone cannot see.
        auto nv = net_vias.find(s.net);
        if (nv != net_vias.end() && ref_cu >= 0) {
            const double mx = 0.5 * (s.x1 + s.x2), my = 0.5 * (s.y1 + s.y2);
            for (const Via* v : nv->second) {
                // only vias belonging to this segment's own run
                if (std::hypot(v->x - mx, v->y - my) > len_mm + 2.0) continue;
                c.layer_change = true;
                const double d = vias.nearest_spanning(v->x, v->y, v->cu_from,
                                                       v->cu_to,
                                                       p.max_return_detour_mm);
                c.stitch_distance_mm = c.stitch_distance_mm > 0
                                           ? std::min(c.stitch_distance_mm, d) : d;
            }
            if (c.layer_change) {
                c.unstitched = c.stitch_distance_mm >= p.max_return_detour_mm;
                // the detour encloses stitch_distance x plane separation, ON
                // TOP of the segment's own loop; a stitch right beside the via
                // adds nothing, which is the behaviour a designer is aiming for
                double hv = 0, eps = 0;
                double sep = c.height_mm;
                for (size_t j = 0; j < layers.size(); ++j) {
                    if (!layers[j].is_plane || (int)j == ref_cu) continue;
                    board.stackup.dielectric_between(std::min((size_t)ref_cu, j),
                                                     std::max((size_t)ref_cu, j),
                                                     hv, eps);
                    if (hv > 0) { sep = std::max(sep, hv); break; }
                }
                c.return_area_m2 = (c.stitch_distance_mm * 1e-3) * (sep * 1e-3);
            }
        }

        c.area_m2 = (len_mm * 1e-3) * (c.height_mm * 1e-3) + c.return_area_m2;

        // current: switch nodes carry what the user says they switch; signals
        // carry swing / Z0, with Z0 taken from the real cross-section where the
        // layer has a reference to compute one against
        c.switch_node = screener.is_switch_node(s.net);
        double i0;
        if (c.switch_node) {
            i0 = p.sw_current_a;
        } else {
            const auto z = screener.z0_estimate(s.cu, s.width);
            i0 = p.swing_v / (z && *z > 0 ? *z : p.default_z0);
        }
        c.current_a = i0;

        // The PLATEAU level: above the edge knee the radiated field is flat,
        // set by area, current and edge rate alone. Using it makes the map a
        // frequency-independent attribution rather than a picture that changes
        // shape with a slider nobody knows how to set.
        emc::Trapezoid t;
        t.amplitude_a = i0;
        t.f_sw_hz = p.f_sw_hz;
        t.duty = p.duty;
        t.rise_s = p.rise_s;
        c.e_v_per_m = gain * emc::plateau_v_per_m(c.area_m2, t, p.r_m);
        c.e_per_m = c.e_v_per_m / (len_mm * 1e-3);

        // A can attenuates what is inside it as seen from outside. Applied to
        // the segment's contribution, not to the field everywhere — the can
        // does nothing for copper outside it, which is the point of drawing a
        // rectangle rather than a global slider.
        const double mx = 0.5 * (s.x1 + s.x2), my = 0.5 * (s.y1 + s.y2);
        for (const auto& sh : p.shields)
            if (sh.contains(mx, my)) {
                c.shield_db = std::max(c.shield_db, sh.se_db);
            }
        if (c.shield_db > 0) {
            const double f = std::pow(10.0, -c.shield_db / 20.0);
            c.e_v_per_m *= f;
            c.e_per_m *= f;
        }

        power += c.e_v_per_m * c.e_v_per_m;
        if (c.no_reference) {
            ++r.no_reference_count;
            power_no_ref += c.e_v_per_m * c.e_v_per_m;
        }
        if (c.over_void) ++r.over_void_count;
        if (c.layer_change) ++r.layer_change_count;
        if (c.unstitched) ++r.unstitched_count;
        if (c.shield_db > 0) ++r.shielded_count;
        {   // what the total would have been without any can
            const double un = c.shield_db > 0
                ? c.e_v_per_m * std::pow(10.0, c.shield_db / 20.0) : c.e_v_per_m;
            power_unshielded += un * un;
        }
        r.max_e_v_per_m = std::max(r.max_e_v_per_m, c.e_v_per_m);
        r.max_e_per_m = std::max(r.max_e_per_m, c.e_per_m);
        ++r.counted;
        r.segments[i] = c;
    }
    if (r.counted == 0)
        throw std::invalid_argument(
            "radmap: no routed copper with a known layer — nothing to attribute");

    r.total_v_per_m = std::sqrt(power);
    r.total_unshielded_v_per_m = std::sqrt(power_unshielded);
    r.total_dbuv_m = emc::to_dbuv_m(r.total_v_per_m);
    r.no_reference_share = power > 0 ? power_no_ref / power : 0.0;

    r.top.resize(board.segments.size());
    std::iota(r.top.begin(), r.top.end(), 0u);
    std::sort(r.top.begin(), r.top.end(), [&](size_t a, size_t b) {
        return r.segments[a].e_v_per_m > r.segments[b].e_v_per_m;
    });
    if (r.top.size() > 40) r.top.resize(40);
    return r;
}

}  // namespace faraday::radmap
