#pragma once
// The component near-field map: which copper is loud at component scale, and
// which parts are close enough to care.
//
// This answers a different question from RadiationMap.hpp. That one attributes
// the 3 m chamber measurement to the copper that causes it. This one asks
// "what is the field a few millimetres above this board, and does the part
// sitting there mind" — which is the question behind why some components get
// shielded and others do not.
//
// The two must never be blended, because the near field is not radiation: at
// component scale below a gigahertz k*r << 1 and the fields are the
// magnetostatic and electrostatic dipole fields, falling as 1/r^3 rather than
// 1/r. See NearField.hpp for the regime and the exact invariants.
//
// WHAT IT COMPUTES
//   * an H layer, from current-driven sources: commutation loops (area already
//     extracted by the screener) and the trace currents themselves;
//   * a per-victim verdict: the field at each sensitive component, the voltage
//     it induces in that component's own loop, and how that compares with the
//     threshold for its class.
//
// WHAT IT REFUSES
//   * any dBuV/m, limit line, margin or pass/fail. There is no reliable
//     near-field to far-field transform, and two standards-level sources say
//     so explicitly.
//   * any far-field claim. The field itself comes from an EXACT Biot-Savart
//     integral over the loop's own polygon, which is valid at every distance
//     outside the conductor, so no refusal is needed for proximity. The
//     point-dipole approximation is reported separately as context: it is
//     invalid within a few source dimensions, and a 267 mm^2 commutation loop
//     is invalid within 46 mm, so the reader is told how far inside the source
//     they are standing.
//
// THE CURRENT IS AN ASSUMPTION AND IT SCALES EVERYTHING LINEARLY. Geometry
// gives the area and the orientation exactly; the current does not come from a
// layout file. Both are surfaced rather than buried.

#include "NearField.hpp"
#include "Shielding.hpp"
#include "Screener.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace faraday::nfmap {

struct MapParams {
    // The excitation, all user-supplied and all stated in the UI.
    // TWO SEPARATE CURRENTS, and conflating them is the easiest way to produce
    // a number nobody believes. The loop carries the full switched current at
    // the fundamental, but only a fraction of it rings at the hot-loop
    // resonance — and it is the RING that dominates coupling, because induced
    // voltage goes as f. Driving the ring with the full DC current
    // over-predicts by more than an order of magnitude and yields victim
    // ratios in the millions, which is not a finding, it is a bug wearing a
    // percentage sign.
    double sw_current_a = 10.0;    // switched current, at the fundamental
    double ring_current_a = 2.0;   // HF amplitude at the ring frequency
    double ring_hz = 130e6;        // hot-loop resonance — the dominant coupling
                                   // case, since induced voltage goes as f
    double f_sw_hz = 500e3;        // switching fundamental
    double dv_dt_v_per_ns = 2.4;   // switch-node slew (silicon ~2-6, GaN 20-150)
    double swing_v = 12.0;
    // Where the map is evaluated. Every answer changes 60 dB per decade of
    // height, so this is a first-class parameter and is always reported.
    double probe_height_mm = 3.0;
    // A victim's loop area, when its own routing has not been measured. Stated
    // rather than silently assumed.
    double default_victim_area_mm2 = 4.0;
    // Shield cans drawn by the user. A can attenuates coupling only when it
    // separates the pair — aggressor inside and victim outside, or the
    // reverse. Both inside or both outside, it changes nothing, which is the
    // behaviour that makes drawing a rectangle mean something.
    std::vector<shield::Rect> shields;
};

// One radiating loop, reduced to a dipole at a location.
struct Aggressor {
    std::string net;
    double x_mm = 0, y_mm = 0;     // centroid
    double area_mm2 = 0;           // enclosed loop area
    double moment_am2 = 0;         // m = N I A, with N = 1 for a board loop
    double a_eff_mm = 0;           // equivalent radius
    double valid_from_mm = 0;      // beyond this the point dipole is also valid
    std::vector<Point> hull;       // the loop itself, for the exact integral
};

struct VictimHit {
    std::string component;         // refdes
    std::string net;
    std::string victim_class;
    double x_mm = 0, y_mm = 0;
    double distance_mm = 0;        // 3-D, including the probe height
    double h_a_per_m = 0;
    double b_tesla = 0;
    double induced_v = 0;
    double threshold_v = 0;
    double ratio = 0;              // induced / threshold
    // Whether the POINT-DIPOLE approximation would also have been valid here.
    // The reported field does not depend on it — that comes from the exact
    // integral — but it tells the reader how far inside the source they are.
    bool dipole_valid = true;
    // Attenuation from a drawn can separating this victim from its aggressor.
    // An UPPER BOUND: the can is five-sided and flux routes around through the
    // PCB, which the UI states rather than hides.
    double shield_db = 0;
    std::string aggressor;
};

struct MapResult {
    std::vector<Aggressor> aggressors;
    std::vector<VictimHit> victims;
    double max_h = 0;
    size_t too_close_count = 0, shielded_victims = 0;   // how many are inside the dipole radius
    // context the reader needs to interpret any of it
    double lambda_over_2pi_mm = 0;
    double probe_height_mm = 0;
    double ring_hz = 0;
};

// Classify a net by name into a victim class. Deliberately conservative: an
// unrecognised net is NOT a victim, because inventing a threshold for a net we
// do not understand would produce confident nonsense.
inline std::string victim_class_for(const std::string& net_name) {
    std::string n;
    for (char c : net_name) n += (char)std::tolower((unsigned char)c);
    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };

    if (has("xtal") || has("osc") || has("crystal")) return "xtal";
    if (has("isense") || has("csense") || has("i_sense") || has("ishunt") ||
        has("cs+") || has("cs-") || has("shunt"))
        return "csa";
    if (has("comp") || has("/fb") || has("feedback") || has("ith") || has("vfb"))
        return "comp";
    if (has("adc") || has("ain") || has("vref") || has("aref")) return "adc12";
    return "";
}

inline MapResult compute(const BoardIR& board, const Screener& screener,
                         const MapParams& p) {
    if (!(p.ring_hz > 0) || !(p.f_sw_hz > 0))
        throw std::invalid_argument("nfmap: frequencies must be > 0");
    if (!(p.probe_height_mm > 0))
        throw std::invalid_argument(
            "nfmap: probe height must be > 0 — a map with no stated height is "
            "meaningless, since the answer changes 60 dB per decade of height");
    if (p.sw_current_a < 0 || p.ring_current_a < 0)
        throw std::invalid_argument("nfmap: currents must be >= 0");
    if (!(p.default_victim_area_mm2 > 0))
        throw std::invalid_argument("nfmap: victim loop area must be > 0");

    MapResult r;
    r.probe_height_mm = p.probe_height_mm;
    r.ring_hz = p.ring_hz;
    r.lambda_over_2pi_mm = nf::near_far_boundary(p.ring_hz) * 1e3;

    // ---- aggressors: the commutation loops the screener already found ----
    for (int net : screener.switch_nets()) {
        auto loop = screener.commutation_loop(net);
        if (!loop || !(loop->area_mm2 > 0)) continue;
        Aggressor a;
        a.net = board.net_name(net);
        a.area_mm2 = loop->area_mm2;
        double cx = 0, cy = 0;
        for (const auto& pt : loop->hull) { cx += pt.x; cy += pt.y; }
        if (!loop->hull.empty()) { cx /= loop->hull.size(); cy /= loop->hull.size(); }
        a.x_mm = cx;
        a.y_mm = cy;
        const double area_m2 = a.area_mm2 * 1e-6;
        // quoted at the RING current: that is the excitation every victim
        // number below is computed against
        a.moment_am2 = nf::magnetic_moment(1.0, p.ring_current_a, area_m2);
        a.a_eff_mm = nf::effective_radius(area_m2) * 1e3;
        a.valid_from_mm = nf::dipole_valid_from_m(area_m2) * 1e3;
        a.hull = loop->hull;
        r.aggressors.push_back(std::move(a));
    }
    if (r.aggressors.empty())
        throw std::invalid_argument(
            "nfmap: no commutation loop found on this board — the near-field "
            "map is built around switching aggressors, and this layout has none "
            "the screener could identify");

    // ---- victims: components on nets whose class we actually recognise ----
    const double area_m2 = p.default_victim_area_mm2 * 1e-6;
    for (const auto& pad : board.pads) {
        if (pad.net < 0) continue;
        const std::string& net = board.net_name(pad.net);
        const std::string cls = victim_class_for(net);
        if (cls.empty()) continue;
        const nf::VictimClass& vc = nf::victim_by_id(cls);

        // nearest aggressor, in three dimensions: the probe height is a real
        // separation, not a label
        const Aggressor* best = nullptr;
        double best_d = 1e30;
        for (const auto& a : r.aggressors) {
            const double dx = pad.x - a.x_mm, dy = pad.y - a.y_mm;
            const double d = std::sqrt(dx * dx + dy * dy +
                                       p.probe_height_mm * p.probe_height_mm);
            if (d < best_d) { best_d = d; best = &a; }
        }
        if (!best) continue;

        VictimHit v;
        v.component = pad.component;
        v.net = net;
        v.victim_class = cls;
        v.x_mm = pad.x;
        v.y_mm = pad.y;
        v.distance_mm = best_d;
        v.threshold_v = vc.threshold_v;
        v.aggressor = best->net;
        // Exact Biot-Savart over the loop's own polygon. The point-dipole law
        // would have to refuse here: a 267 mm^2 commutation loop is invalid
        // within 46 mm, which on a 100 mm board excludes half the parts —
        // including every one sitting beside the switcher, which is precisely
        // what anyone would want to ask about. The integral has no such
        // restriction and agrees with the dipole to 2% once far enough out
        // (asserted in test_nearfield.cpp), so nothing is given up by using it
        // everywhere.
        v.dipole_valid = best_d >= best->valid_from_mm;
        if (!v.dipole_valid) ++r.too_close_count;
        if (best->hull.size() >= 3) {
            std::vector<nf::Vec3> poly;
            poly.reserve(best->hull.size());
            for (const auto& pt : best->hull)
                poly.push_back({pt.x * 1e-3, pt.y * 1e-3, 0.0});
            const nf::Vec3 probe{pad.x * 1e-3, pad.y * 1e-3,
                                 p.probe_height_mm * 1e-3};
            v.h_a_per_m = nf::h_loop(poly, probe, p.ring_current_a);
        } else {
            // no hull to integrate: fall back to the dipole, and only where it
            // is actually valid rather than extrapolating it
            if (!v.dipole_valid) { r.victims.push_back(std::move(v)); continue; }
            v.h_a_per_m = nf::h_equatorial(best->moment_am2, best_d * 1e-3);
        }
        // A can between the pair attenuates by its SE; around both or neither
        // it does nothing.
        for (const auto& sh : p.shields) {
            const bool agg_in = sh.contains(best->x_mm, best->y_mm);
            const bool vic_in = sh.contains(pad.x, pad.y);
            if (agg_in != vic_in) v.shield_db = std::max(v.shield_db, sh.se_db);
        }
        if (v.shield_db > 0) {
            v.h_a_per_m *= std::pow(10.0, -v.shield_db / 20.0);
            ++r.shielded_victims;
        }
        v.b_tesla = nf::b_from_h(v.h_a_per_m);
        // cos(theta) = 1: worst case, because a layout tool cannot know the
        // victim loop's orientation without its routing. Stated, not hidden.
        v.induced_v = nf::induced_voltage(p.ring_hz, v.b_tesla, area_m2, 1.0);
        v.ratio = v.induced_v / v.threshold_v;
        r.max_h = std::max(r.max_h, v.h_a_per_m);
        r.victims.push_back(std::move(v));
    }

    std::sort(r.victims.begin(), r.victims.end(),
              [](const VictimHit& a, const VictimHit& b) { return a.ratio > b.ratio; });
    if (r.victims.size() > 60) r.victims.resize(60);
    return r;
}

}  // namespace faraday::nfmap
