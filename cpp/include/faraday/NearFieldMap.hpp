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
    // Inductors on switch nets are near-field sources in their own right. The
    // construction factor is the ONE attribute geometry cannot see, so it is a
    // stated input: measured anchors (Wurth ANP047c, Vishay) put a shielded
    // part ~9-10.5 dB below an unshielded drum and a moulded composite ~10.6 dB
    // below, conservatively carried to the ring frequency.
    double inductor_k = 1.0;           // unshielded drum
    std::string inductor_type = "unshielded";
    // Shield cans drawn by the user. A can attenuates coupling only when it
    // separates the pair — aggressor inside and victim outside, or the
    // reverse. Both inside or both outside, it changes nothing, which is the
    // behaviour that makes drawing a rectangle mean something.
    std::vector<shield::Rect> shields;
};

// One radiating source: a commutation loop (with its hull, for the exact
// integral) or an inductor (a dipole at its footprint).
struct Aggressor {
    std::string kind = "loop";
    std::string net;
    double x_mm = 0, y_mm = 0;     // centroid
    double area_mm2 = 0;           // enclosed loop area
    double moment_am2 = 0;         // m = N I A, with N = 1 for a board loop
    double a_eff_mm = 0;           // equivalent radius
    double valid_from_mm = 0;      // beyond this the point dipole is also valid
    std::vector<Point> hull;       // the loop itself, for the exact integral
    // The current that flows in THIS loop. A commutation loop carries the ring
    // current; an inductor carries it derated by its construction (a shielded
    // part leaks a fraction of a drum's field). Before this was per-aggressor
    // the map applied one current to everything, so an inductor's derating
    // existed only in its moment — and its moment was only ever used outside
    // the dipole-validity radius.
    double current_a = 0;
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
    // cos(theta) between the aggressor's field VECTOR at the pad and the
    // victim loop's own normal, taken from the victim net's routed direction.
    // This is real layout information — rotating a victim edge-on nulls the
    // coupling, and no distance rule can see that. When the victim net has no
    // routed segment near the pad, 1.0 worst case is used and flagged.
    double cos_theta = 1.0;
    bool oriented = false;
    // Attenuation from a drawn can separating this victim from its aggressor.
    // An UPPER BOUND: the can is five-sided and flux routes around through the
    // PCB, which the UI states rather than hides.
    double shield_db = 0;
    std::string aggressor;
};

// A victim trace or pad broadside over switch-node copper: the capacitive
// mechanism, with the frequency-independent divider ceiling as its bound.
struct CapHit {
    std::string component, net, victim_class;
    double x_mm = 0, y_mm = 0;
    double overlap_mm2 = 0, c12_f = 0, dv_v = 0, threshold_v = 0, ratio = 0;
};

// One drawn can, judged against the board it would be soldered to. The SE the
// can's datasheet earns is only delivered if the layout has a ground fence at
// that contact pitch — see shield::bond_check.
struct ShieldBond {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    double spec_seam_mm = 0, spec_se_db = 0;
    int contacts = 0;
    double perimeter_mm = 0, achievable_pitch_mm = 0;
    double delivered_se_db = 0;   // SE at the pitch the BOARD can actually give
    bool fence_present = false, limits_the_can = false;
};

struct MapResult {
    std::vector<Aggressor> aggressors;
    std::vector<VictimHit> victims;
    std::vector<CapHit> cap_hits;
    std::vector<ShieldBond> shield_bonds;
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
        a.current_a = p.ring_current_a;
        r.aggressors.push_back(std::move(a));
    }
    // ---- aggressors: inductors sitting on those switch nets ----
    {
        std::map<std::string, std::pair<Point, Point>> bbox;   // ref -> min,max
        std::map<std::string, bool> on_sw;
        for (const auto& pad : board.pads) {
            if (pad.component.empty() || pad.component[0] != 'L') continue;
            auto& [lo, hi] = bbox.try_emplace(pad.component,
                std::make_pair(Point{pad.x, pad.y}, Point{pad.x, pad.y})).first->second;
            lo.x = std::min(lo.x, pad.x - pad.w / 2); lo.y = std::min(lo.y, pad.y - pad.h / 2);
            hi.x = std::max(hi.x, pad.x + pad.w / 2); hi.y = std::max(hi.y, pad.y + pad.h / 2);
            if (screener.is_switch_node(pad.net)) on_sw[pad.component] = true;
        }
        for (const auto& [ref, mm] : bbox) {
            if (!on_sw.count(ref)) continue;
            const auto& [lo, hi] = mm;
            const double area_mm2 = std::max((hi.x - lo.x) * (hi.y - lo.y), 1.0);
            Aggressor a;
            a.kind = "inductor";
            a.net = ref + " (" + p.inductor_type + ")";
            a.x_mm = (lo.x + hi.x) / 2;
            a.y_mm = (lo.y + hi.y) / 2;
            a.area_mm2 = area_mm2;
            a.moment_am2 = nf::magnetic_moment(1.0, p.inductor_k * p.ring_current_a,
                                               area_mm2 * 1e-6);
            a.a_eff_mm = nf::effective_radius(area_mm2 * 1e-6) * 1e3;
            a.valid_from_mm = nf::dipole_valid_from_m(area_mm2 * 1e-6) * 1e3;
            a.current_a = p.inductor_k * p.ring_current_a;
            // An inductor is a loop of finite size, so give it the loop: the
            // rectangle its own footprint occupies, carrying the derated
            // current. Same moment as the dipole in the far field, and — the
            // point — a field that can be evaluated CLOSE UP, where a dipole
            // cannot. Without a hull this aggressor contributed nothing to the
            // rendered map and nothing to a victim inside its validity radius,
            // so raising the probe could make the reported field GO UP as the
            // source became computable (ABT #798).
            a.hull = {Point{lo.x, lo.y}, Point{hi.x, lo.y},
                      Point{hi.x, hi.y}, Point{lo.x, hi.y}};
            r.aggressors.push_back(std::move(a));
        }
    }
    if (r.aggressors.empty())
        throw std::invalid_argument(
            "nfmap: no commutation loop found on this board — the near-field "
            "map is built around switching aggressors, and this layout has none "
            "the screener could identify");

    // ---- can bonding: what the BOARD can deliver, not what the can claims ---
    // Done BEFORE the victims so the SE actually applied below is the delivered
    // one. A can whose contact pitch the layout cannot support is the single
    // most common way a shield underperforms its datasheet, and it is the one
    // shield caveat a layout file can settle.
    std::vector<shield::Rect> shields = p.shields;
    if (!shields.empty()) {
        std::vector<std::pair<double, double>> return_pts;
        for (const auto& v : board.vias)
            if (screener.is_return_net(v.net)) return_pts.push_back({v.x, v.y});
        for (const auto& pad : board.pads)
            if (screener.is_return_net(pad.net))
                return_pts.push_back({pad.x, pad.y});
        for (auto& sh : shields) {
            const shield::BondCheck bc =
                shield::bond_check(sh, return_pts, sh.spec_seam_mm);
            ShieldBond sb;
            sb.x1 = sh.x1; sb.y1 = sh.y1; sb.x2 = sh.x2; sb.y2 = sh.y2;
            sb.spec_seam_mm = sh.spec_seam_mm;
            sb.spec_se_db = sh.se_db;
            sb.contacts = bc.contacts;
            sb.perimeter_mm = bc.perimeter_mm;
            sb.achievable_pitch_mm = bc.achievable_pitch_mm;
            sb.fence_present = bc.fence_present;
            sb.limits_the_can = bc.limits_the_can;
            if (!bc.fence_present) {
                // NOT evidence of an unbonded can. The usual can fence is a
                // solder-mask opening onto the pour — bare copper, which is not
                // a pad or a via and so cannot be counted here. Absence of
                // countable contacts means "cannot verify", and a check that
                // cannot see the thing must not rule on it: the specified
                // figure stands, flagged, rather than being zeroed on evidence
                // the IR does not actually contain.
                sb.delivered_se_db = sh.se_db;
                sb.limits_the_can = false;
            } else if (bc.limits_the_can) {
                shield::Can c = sh.can;
                c.seam_pitch_mm = bc.achievable_pitch_mm;
                sb.delivered_se_db =
                    shield::evaluate(c, p.ring_hz,
                                     shield::FieldKind::MagneticNear).se_db;
                sh.se_db = std::min(sh.se_db, sb.delivered_se_db);
            } else {
                sb.delivered_se_db = sh.se_db;
            }
            r.shield_bonds.push_back(sb);
        }
    }

    // ---- victims: components on nets whose class we actually recognise ----
    // The net NAME is the primary evidence and the best one when it exists. It
    // often does not: a CAD tool names every unnamed net after a refdes and a
    // pin ("NetIC4_19"), and a board full of those matches nothing at all.
    //
    // The PART is the other evidence, and for one family it is unambiguous: a
    // crystal loop IS the high-Q, high-impedance node the "xtal" class
    // describes, whatever its net is called. That is the only family promoted
    // here. An analog IC is NOT assumed to be an ADC input — inventing victims
    // is worse than missing them, because a manufactured threshold looks
    // exactly like a measured one.
    const double area_m2 = p.default_victim_area_mm2 * 1e-6;
    for (const auto& pad : board.pads) {
        if (pad.net < 0) continue;
        const std::string& net = board.net_name(pad.net);
        std::string cls = victim_class_for(net);
        if (cls.empty() && !pad.component.empty()) {
            auto pd = board.part_data.find(pad.component);
            if (pd != board.part_data.end() && pd->second.family == "timing")
                cls = "xtal";
        }
        if (cls.empty()) continue;
        const nf::VictimClass& vc = nf::victim_by_id(cls);

        // STRONGEST aggressor at this pad, not the nearest: a big loop across
        // the board can out-field a small one next door, and the field is the
        // thing the victim experiences. Each candidate is evaluated by the
        // exact integral where it has a hull, and by the dipole only where the
        // dipole is valid.
        const Aggressor* best = nullptr;
        double best_d = 1e30, best_h = -1.0;
        nf::Vec3 best_vec;
        const nf::Vec3 probe{pad.x * 1e-3, pad.y * 1e-3, p.probe_height_mm * 1e-3};
        for (const auto& a : r.aggressors) {
            const double dx = pad.x - a.x_mm, dy = pad.y - a.y_mm;
            const double d = std::sqrt(dx * dx + dy * dy +
                                       p.probe_height_mm * p.probe_height_mm);
            double h = -1.0;
            nf::Vec3 vec;
            if (a.hull.size() >= 3) {
                std::vector<nf::Vec3> poly;
                poly.reserve(a.hull.size());
                for (const auto& pt : a.hull)
                    poly.push_back({pt.x * 1e-3, pt.y * 1e-3, 0.0});
                vec = nf::h_loop_vec(poly, probe, a.current_a);
                h = std::sqrt(vec.x * vec.x + vec.y * vec.y + vec.z * vec.z);
            } else if (d >= a.valid_from_mm) {
                h = nf::h_equatorial(a.moment_am2, d * 1e-3);
                vec = {0, 0, h};   // dipole axis vertical: field ~ axial at range
            }
            if (h > best_h) { best_h = h; best_d = d; best = &a; best_vec = vec; }
        }
        if (!best || !(best_h >= 0)) continue;

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
        v.h_a_per_m = best_h;
        // cos(theta): the victim loop (its trace over its return) has its
        // normal IN the board plane, perpendicular to the trace direction.
        // Take the direction from the victim net's own nearest routed segment.
        {
            double bd = 25.0, tx = 0, ty = 0;
            for (const auto& seg : board.segments) {
                if (seg.net != pad.net) continue;
                const double sdx = seg.x2 - seg.x1, sdy = seg.y2 - seg.y1;
                const double sl = std::hypot(sdx, sdy);
                if (!(sl > 0)) continue;
                double t = ((pad.x - seg.x1) * sdx + (pad.y - seg.y1) * sdy) / (sl * sl);
                t = std::clamp(t, 0.0, 1.0);
                const double d = std::hypot(pad.x - (seg.x1 + t * sdx),
                                            pad.y - (seg.y1 + t * sdy));
                if (d < bd) { bd = d; tx = sdx / sl; ty = sdy / sl; }
            }
            if (bd < 25.0) {
                const double hmag = std::hypot(best_vec.x, best_vec.y, best_vec.z);
                if (hmag > 0) {
                    // loop normal n = (-ty, tx, 0)
                    v.cos_theta = std::abs(-ty * best_vec.x + tx * best_vec.y) / hmag;
                    v.oriented = true;
                }
            }
        }
        // A can between the pair attenuates by its SE; around both or neither
        // it does nothing.
        for (const auto& sh : shields) {
            const bool agg_in = sh.contains(best->x_mm, best->y_mm);
            const bool vic_in = sh.contains(pad.x, pad.y);
            if (agg_in != vic_in) v.shield_db = std::max(v.shield_db, sh.se_db);
        }
        if (v.shield_db > 0) {
            v.h_a_per_m *= std::pow(10.0, -v.shield_db / 20.0);
            ++r.shielded_victims;
        }
        v.b_tesla = nf::b_from_h(v.h_a_per_m);
        v.induced_v = nf::induced_voltage(p.ring_hz, v.b_tesla, area_m2,
                                          v.oriented ? v.cos_theta : 1.0);
        v.ratio = v.induced_v / v.threshold_v;
        r.max_h = std::max(r.max_h, v.h_a_per_m);
        r.victims.push_back(std::move(v));
    }

    // ---- capacitive: victim pads broadside over switch-node copper ----
    // The E mechanism, bounded by the frequency-independent divider ceiling.
    // Only broadside overlap across an adjacent dielectric is claimed, because
    // that is the one capacitance a layout computes exactly.
    for (const auto& pad : board.pads) {
        if (pad.net < 0 || pad.cu < 0) continue;
        const std::string cls = victim_class_for(board.net_name(pad.net));
        if (cls.empty()) continue;
        for (const auto& seg : board.segments) {
            if (!screener.is_switch_node(seg.net) || seg.cu == pad.cu) continue;
            const double sdx = seg.x2 - seg.x1, sdy = seg.y2 - seg.y1;
            const double sl = std::hypot(sdx, sdy);
            if (!(sl > 0)) continue;
            double t = ((pad.x - seg.x1) * sdx + (pad.y - seg.y1) * sdy) / (sl * sl);
            t = std::clamp(t, 0.0, 1.0);
            const double d = std::hypot(pad.x - (seg.x1 + t * sdx),
                                        pad.y - (seg.y1 + t * sdy));
            if (d > seg.width / 2 + std::max(pad.w, pad.h) / 2) continue;
            double hmm = 0, eps = 4.3;
            board.stackup.dielectric_between(std::min(pad.cu, seg.cu),
                                             std::max(pad.cu, seg.cu), hmm, eps);
            if (!(hmm > 0)) continue;
            CapHit ch;
            ch.component = pad.component;
            ch.net = board.net_name(pad.net);
            ch.victim_class = cls;
            ch.x_mm = pad.x;
            ch.y_mm = pad.y;
            ch.overlap_mm2 = std::min(pad.w, seg.width) * std::min(pad.h, sl);
            ch.c12_f = nf::overlap_capacitance(ch.overlap_mm2 * 1e-6,
                                               hmm * 1e-3, eps);
            ch.dv_v = nf::capacitive_step(p.swing_v, ch.c12_f, 5e-12);
            ch.threshold_v = nf::victim_by_id(cls).threshold_v;
            ch.ratio = ch.dv_v / ch.threshold_v;
            r.cap_hits.push_back(std::move(ch));
            break;   // one hit per pad is the story; the worst is enough
        }
    }
    std::sort(r.cap_hits.begin(), r.cap_hits.end(),
              [](const CapHit& a, const CapHit& b) { return a.ratio > b.ratio; });
    if (r.cap_hits.size() > 12) r.cap_hits.resize(12);

    std::sort(r.victims.begin(), r.victims.end(),
              [](const VictimHit& a, const VictimHit& b) { return a.ratio > b.ratio; });
    if (r.victims.size() > 60) r.victims.resize(60);
    return r;
}

}  // namespace faraday::nfmap
