#pragma once
// Where does each trace's return current actually flow?
//
// This replaces the "radiation attribution" map, and the reason is a
// measurement. On the MPPT board, ranking nets WITH the assumed current gave
// SUPPLY_INPUT 97% / SW_NODE 2% — a restatement of the switch-node rule the
// findings list already makes in words. Ranking the same board by GEOMETRY
// alone gave a completely different answer. The 45.7 dB current spread came
// from one invented binary (switch node -> 10 A, else swing/Z0), so the
// dBuV/m total was a claim the inputs could not support. The emissions panel
// already gives a defensible far-field number for the loops it covers, with a
// limit line to check it against; that is where dBuV/m belongs.
//
// What layout alone DOES determine, exactly, is the geometry of every return:
//
//   * the dielectric height to the reference plane where the plane is really
//     there (checked against the actual pour polygons, not the stackup's
//     promise);
//   * the detour where it is not — a void or split under the trace closes the
//     loop over the next plane down, or over the whole board;
//   * the hop at every LAYER CHANGE: the return must cross between planes
//     through a stitching via, and the distance to the nearest via that spans
//     that layer pair is loop area the trace geometry alone cannot see.
//
// THE REGIME IS HIGH FREQUENCY, and that is stated because it decides where
// the return actually flows. Above roughly 1 MHz on a PCB, inductance beats
// resistance and the return current concentrates in the plane DIRECTLY under
// the trace — which is why sampling the pour under each segment is the right
// model at all. (At DC it would spread across the copper and take the
// shortest resistive path; that regime is not what fails EMC and is not what
// this maps.) At an obstruction the HF return does not teleport to another
// layer: it follows the SLOT EDGE laterally on the same plane, unless a
// closely coupled second plane offers a shorter displacement-current path.
// The model therefore takes, at every uncovered sample, the CHEAPER of the
// lateral detour around the void and the vertical drop to the next plane.
//
// The map is coloured by EFFECTIVE LOOP HEIGHT — enclosed area per unit
// length, in millimetres. It reads as "how far away is this trace's return,
// really": 0.1-0.2 mm is a tight microstrip, 1.6 mm is a return detouring
// across the whole board. Per unit length so a router chopping one trace into
// a dozen segments renders as one colour, and with no current anywhere in it,
// so nothing here is an assumption. That is the point: this layer shows only
// what the layout proves.

#include "Screener.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace faraday::rp {

struct MapParams {
    // A layer with no reference plane at all has no defined return path.
    // Rather than drop those traces — they are the worst loops on the board,
    // and hiding them would be exactly backwards — the loop is closed over
    // this height, and the count is reported.
    double no_reference_height_mm = 1.6;
    // How far a return is allowed to detour looking for a stitch before the
    // loop is simply "unstitched". Beyond this the exact distance stops
    // mattering — it is already a bad loop.
    double max_return_detour_mm = 25.0;
};

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

struct SegmentQuality {
    double len_mm = 0;
    double height_mm = 0;          // effective height incl. void detours
    double area_mm2 = 0;           // len x height — this segment's own loop
    // The colouring metric: height plus the net's layer-change detour area
    // amortised over the net's total length. A detour is a FIXED area per via,
    // so folding it into one segment's own length is dimensionally wrong — a
    // 0.02 mm router micro-segment beside a via came out with an effective
    // height of a megametre, and the same detour was double-counted into every
    // nearby segment. Amortising per net is both stable and honest: it is the
    // net's average extra height, however the router chopped it.
    double eff_height_mm = 0;
    double unreferenced_fraction = 0;
    // Layer-change return hop: distance to the nearest stitching via spanning
    // the plane pair, and the extra loop area that detour encloses.
    double stitch_distance_mm = 0;
    double return_area_mm2 = 0;
    bool no_reference = false;
    bool over_void = false;
    bool layer_change = false;
    bool unstitched = false;       // no spanning reference via within reach
};

// Worst offenders, aggregated by net — enclosed loop area is a geometric fact,
// so the ranking carries no assumption.
struct NetWorst {
    int net = -1;
    double area_mm2 = 0;
    double worst_eff_mm = 0;
    bool over_void = false, unstitched = false, no_reference = false;
};

struct MapResult {
    std::vector<SegmentQuality> segments;   // parallel to board.segments
    double min_eff_mm = 1e30, max_eff_mm = 0;
    size_t counted = 0;
    size_t no_reference_count = 0, over_void_count = 0;
    size_t layer_change_count = 0, unstitched_count = 0;
    std::vector<NetWorst> worst;            // largest enclosed area first
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

inline MapResult compute(const BoardIR& board, const Screener& screener,
                         const MapParams& p) {
    if (!(p.no_reference_height_mm > 0))
        throw std::invalid_argument("rp: the fallback loop height must be > 0");
    if (!(p.max_return_detour_mm > 0))
        throw std::invalid_argument("rp: the detour search radius must be > 0");

    const std::vector<LayerModel>& layers = screener.layer_models();
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
    std::map<int, NetWorst> by_net;

    for (size_t i = 0; i < board.segments.size(); ++i) {
        const Segment& s = board.segments[i];
        if (s.cu < 0 || (size_t)s.cu >= layers.size()) continue;
        const double dx = s.x2 - s.x1, dy = s.y2 - s.y1;
        const double len_mm = std::sqrt(dx * dx + dy * dy);
        if (!(len_mm > 0)) continue;

        SegmentQuality c;
        c.len_mm = len_mm;
        c.height_mm = loop_height_mm(layers[s.cu], p, &c.no_reference);

        // The stackup height is what the reference plane would give you IF it
        // were actually there under this particular copper. Sample along the
        // segment and check against the real pour polygons. Where the plane is
        // missing the return detours, and the loop closes over the next plane
        // down — or over the board.
        const LayerModel& lm = layers[s.cu];
        const int ref_cu = (lm.ref_dn >= 0) ? lm.ref_dn : lm.ref_up;
        if (ref_cu >= 0) {
            const int samples = std::clamp((int)std::ceil(len_mm / 0.5), 2, 24);
            // unit normal to the segment — the direction the HF return diverts
            const double nx = -dy / len_mm, ny = dx / len_mm;
            // The vertical alternative: a closely coupled second plane the
            // return can reach by displacement current. Starts at the detour
            // cap, NOT at the whole-board height — on a 2-layer board there is
            // no conductor "over the board" for the return to use, so the slot
            // edge is the only path and must not be capped by a fiction.
            double alt_v = p.max_return_detour_mm;
            for (size_t j = 0; j < layers.size(); ++j) {
                if ((int)j == ref_cu || (int)j == s.cu || !layers[j].is_plane)
                    continue;
                double h = 0, eps = 0;
                board.stackup.dielectric_between(std::min((size_t)s.cu, j),
                                                 std::max((size_t)s.cu, j), h, eps);
                if (h > c.height_mm) { alt_v = std::min(alt_v, h); }
            }
            int uncovered = 0;
            double detour_sum = 0;
            for (int k = 0; k < samples; ++k) {
                const double f = (k + 0.5) / samples;
                const double px = s.x1 + dx * f, py = s.y1 + dy * f;
                if (planes.covered(ref_cu, px, py)) continue;
                ++uncovered;
                // The HF return follows the slot edge: probe perpendicular to
                // the trace for the nearest copper of the SAME plane. The loop
                // bulges sideways by that distance, so it acts as the local
                // effective height of the detour.
                double alt_l = p.max_return_detour_mm;
                for (double d : {0.5, 1.0, 2.0, 3.0, 5.0, 8.0, 12.0, 18.0}) {
                    if (d >= alt_l) break;
                    if (planes.covered(ref_cu, px + nx * d, py + ny * d) ||
                        planes.covered(ref_cu, px - nx * d, py - ny * d)) {
                        alt_l = d;
                        break;
                    }
                }
                detour_sum += std::min(alt_l, alt_v);
            }
            c.unreferenced_fraction = (double)uncovered / samples;
            if (uncovered > 0) {
                // area-weighted: the covered part keeps its tight loop, each
                // uncovered sample pays its own cheapest detour
                c.height_mm = c.height_mm * (1.0 - c.unreferenced_fraction) +
                              (detour_sum / uncovered) * c.unreferenced_fraction;
                c.over_void = true;
            }
        }

        // Layer-change hop: the nearest reference via SPANNING the pair. Flags
        // and the per-segment display value are set here; the AREA is charged
        // once per via at net level, below, so a via surrounded by a dozen
        // router micro-segments is not counted a dozen times.
        auto nv = net_vias.find(s.net);
        if (nv != net_vias.end() && ref_cu >= 0) {
            const double mx = 0.5 * (s.x1 + s.x2), my = 0.5 * (s.y1 + s.y2);
            for (const Via* v : nv->second) {
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
                double hv = 0, eps = 0;
                double sep = c.height_mm;
                for (size_t j = 0; j < layers.size(); ++j) {
                    if (!layers[j].is_plane || (int)j == ref_cu) continue;
                    board.stackup.dielectric_between(std::min((size_t)ref_cu, j),
                                                     std::max((size_t)ref_cu, j),
                                                     hv, eps);
                    if (hv > 0) { sep = std::max(sep, hv); break; }
                }
                c.return_area_mm2 = c.stitch_distance_mm * sep;
            }
        }

        c.area_mm2 = len_mm * c.height_mm;

        if (c.no_reference) ++r.no_reference_count;
        if (c.over_void) ++r.over_void_count;
        if (c.layer_change) ++r.layer_change_count;
        if (c.unstitched) ++r.unstitched_count;
        ++r.counted;

        NetWorst& w = by_net[s.net];
        w.net = s.net;
        w.area_mm2 += c.area_mm2;
        w.over_void = w.over_void || c.over_void;
        w.unstitched = w.unstitched || c.unstitched;
        w.no_reference = w.no_reference || c.no_reference;

        r.segments[i] = c;
    }
    if (r.counted == 0)
        throw std::invalid_argument(
            "rp: no routed copper with a known layer — nothing to map");

    // Charge each layer-change detour ONCE, per via, at net level; then hand
    // every segment of the net its share as extra effective height.
    std::map<int, double> net_len, net_detour;
    for (size_t i = 0; i < board.segments.size(); ++i)
        if (r.segments[i].len_mm > 0)
            net_len[board.segments[i].net] += r.segments[i].len_mm;
    for (const auto& [net, vlist] : net_vias) {
        if (!net_len.count(net)) continue;
        for (const Via* v : vlist) {
            const double d = vias.nearest_spanning(v->x, v->y, v->cu_from,
                                                   v->cu_to,
                                                   p.max_return_detour_mm);
            // separation between the plane pair the return hops across; the
            // dominant plane spacing is the honest scale for the detour loop
            double sep = p.no_reference_height_mm;
            for (size_t a = 0; a < layers.size(); ++a)
                for (size_t b2 = a + 1; b2 < layers.size(); ++b2) {
                    if (!layers[a].is_plane || !layers[b2].is_plane) continue;
                    double hv = 0, eps = 0;
                    board.stackup.dielectric_between(a, b2, hv, eps);
                    if (hv > 0) { sep = std::min(sep, hv); }
                }
            net_detour[net] += d * sep;
        }
    }
    for (size_t i = 0; i < board.segments.size(); ++i) {
        SegmentQuality& c = r.segments[i];
        if (!(c.len_mm > 0)) continue;
        const int net = board.segments[i].net;
        const double extra =
            net_detour.count(net) ? net_detour[net] / net_len[net] : 0.0;
        c.eff_height_mm = c.height_mm + extra;
        r.min_eff_mm = std::min(r.min_eff_mm, c.eff_height_mm);
        r.max_eff_mm = std::max(r.max_eff_mm, c.eff_height_mm);
        NetWorst& w = by_net[net];
        w.worst_eff_mm = std::max(w.worst_eff_mm, c.eff_height_mm);
    }
    for (auto& [net, d] : net_detour) by_net[net].area_mm2 += d;

    for (auto& [net, w] : by_net) r.worst.push_back(w);
    std::sort(r.worst.begin(), r.worst.end(),
              [](const NetWorst& a, const NetWorst& b) {
                  return a.area_mm2 > b.area_mm2;
              });
    if (r.worst.size() > 8) r.worst.resize(8);
    return r;
}

}  // namespace faraday::rp
