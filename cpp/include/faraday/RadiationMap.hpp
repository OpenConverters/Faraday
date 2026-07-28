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
    double area_m2 = 0;
    double current_a = 0;
    double e_v_per_m = 0;
    double height_mm = 0;
    bool no_reference = false;
    bool switch_node = false;
};

struct MapResult {
    std::vector<SegmentContribution> segments;   // parallel to board.segments
    double total_v_per_m = 0;      // incoherent (power) sum
    double total_dbuv_m = 0;
    double max_e_v_per_m = 0;
    size_t counted = 0, no_reference_count = 0;
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

        power += c.e_v_per_m * c.e_v_per_m;
        if (c.no_reference) {
            ++r.no_reference_count;
            power_no_ref += c.e_v_per_m * c.e_v_per_m;
        }
        r.max_e_v_per_m = std::max(r.max_e_v_per_m, c.e_v_per_m);
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
