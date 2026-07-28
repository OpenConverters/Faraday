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
#include <numeric>
#include <vector>

namespace faraday::radmap {

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
};

struct SegmentContribution {
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

    MapResult r;
    r.segments.resize(board.segments.size());
    double power = 0, power_no_ref = 0;

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
        c.area_m2 = (len_mm * 1e-3) * (c.height_mm * 1e-3);

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

        power += c.e_v_per_m * c.e_v_per_m;
        if (c.no_reference) {
            ++r.no_reference_count;
            power_no_ref += c.e_v_per_m * c.e_v_per_m;
        }
        if (c.over_void) ++r.over_void_count;
        r.max_e_v_per_m = std::max(r.max_e_v_per_m, c.e_v_per_m);
        r.max_e_per_m = std::max(r.max_e_per_m, c.e_per_m);
        ++r.counted;
        r.segments[i] = c;
    }
    if (r.counted == 0)
        throw std::invalid_argument(
            "radmap: no routed copper with a known layer — nothing to attribute");

    r.total_v_per_m = std::sqrt(power);
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
