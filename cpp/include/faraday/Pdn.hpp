#pragma once
// Power-distribution-network impedance, from the layout.
//
// Faraday's screening tier says "C14 -> U2, 24 mm" and cannot say whether that
// matters. This closes the sentence: every decoupling capacitor becomes a
// series R-L-C branch whose INDUCTANCE IS MEASURED OFF THE BOARD — pad-to-via
// distance on each terminal, via barrels, spreading — and the rail's impedance
// is the parallel combination across frequency, with its anti-resonance peaks,
// against a target the user derives from transient current and allowed ripple.
//
// The mounting loop is the differentiator. Datasheets give C and ESL; nobody's
// tool reads the ACTUAL distance from this cap's pads to its vias on this
// board, and that distance routinely triples the effective inductance. Rules
// of thumb used, stated: ~0.8 nH/mm of pad-to-via escape, ~0.3 nH per via
// barrel, order-of-magnitude figures adequate to RANK capacitors and expose
// the ones whose placement wastes them.
//
// The model is honest about what it is: lumped, linear, and analytic. Each
// branch is ESR + jwL + 1/jwC; the plane pair is a capacitance from the REAL
// overlap area of the two pours; the VRM is an R-L behind the low end. What it
// does not model, and says so: distributed plane resonance above ~1 GHz,
// interplane spreading beyond a lumped term, and load-die capacitance.

#include "Screener.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace faraday::pdn {

// ---------------------------------------------------------------------------
// Component value parsing — "100n", "4u7", "0.1uF", "2200p"
// ---------------------------------------------------------------------------

inline std::optional<double> parse_capacitance(const std::string& raw) {
    std::string v;
    for (char c : raw)
        if (!std::isspace((unsigned char)c)) v += (char)std::tolower((unsigned char)c);
    if (v.empty()) return std::nullopt;
    // strip a trailing farad marker
    if (v.back() == 'f') v.pop_back();
    if (v.empty()) return std::nullopt;
    auto mult = [](char c) -> double {
        switch (c) {
            case 'p': return 1e-12;
            case 'n': return 1e-9;
            case 'u': return 1e-6;
            case 'm': return 1e-3;
            default: return 0;
        }
    };
    // "4u7" style: digits, multiplier, digits
    for (size_t i = 0; i < v.size(); ++i) {
        const double m = mult(v[i]);
        if (m == 0) continue;
        const std::string a = v.substr(0, i), b = v.substr(i + 1);
        try {
            if (!a.empty() && b.empty()) return std::stod(a) * m;
            if (!a.empty() && !b.empty() &&
                b.find_first_not_of("0123456789") == std::string::npos)
                return std::stod(a + "." + b) * m;
        } catch (...) { return std::nullopt; }
        return std::nullopt;
    }
    // bare number: refuse rather than guess a unit — a "100" could be pF or nF
    // depending on the library's habits, and a wrong guess poisons the curve
    return std::nullopt;
}

// ESL by package size, read from the footprint name. Order matters: "1210"
// contains "121", so match longest first.
inline double esl_from_footprint(const std::string& fp) {
    static const std::vector<std::pair<const char*, double>> table = {
        {"01005", 0.25e-9}, {"0201", 0.3e-9}, {"0402", 0.4e-9},
        {"0603", 0.5e-9},   {"0805", 0.7e-9}, {"1206", 1.0e-9},
        {"1210", 1.2e-9},   {"1812", 1.6e-9}, {"2220", 2.0e-9},
    };
    for (const auto& [k, v] : table)
        if (fp.find(k) != std::string::npos) return v;
    return 1.0e-9;   // unknown package: mid-of-road, stated in the output
}

// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------

struct CapBranch {
    std::string ref;
    double c_f = 0;
    double esl_h = 0;          // package
    double l_mount_h = 0;      // measured off the board — the differentiator
    double esr_ohm = 0.015;
    double via_d1_mm = 0, via_d2_mm = 0;
    bool no_via = false;       // no same-net via within reach of a pad
    double f_res_hz = 0;
    std::string package;
};

struct Rail {
    int net = -1;
    std::string name;
    std::vector<CapBranch> caps;
    double plane_c_f = 0;      // from the REAL pour overlap
    double plane_overlap_mm2 = 0;
    int skipped_unparsed = 0;  // caps whose value string we refused to guess
};

struct Params {
    double vrm_r_ohm = 0.01;
    double vrm_l_h = 20e-9;
    double f_lo_hz = 1e4, f_hi_hz = 1e9;
    int points = 220;
    double via_search_mm = 4.0;
    // rules of thumb, stated: escape trace and via barrel inductance
    double nh_per_mm = 0.8;
    double via_nh = 0.3;
};

struct Curve {
    std::vector<double> f_hz, z_ohm;
    double z_max_ohm = 0, z_max_hz = 0;     // in the 100 kHz - 100 MHz band
    std::vector<std::pair<double, double>> antires;   // f, z of local maxima
};

struct Result {
    std::vector<Rail> rails;
    int gnd_net = -1;
    std::string gnd_name;
};

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

inline Result discover(const BoardIR& board, const Screener& screener,
                       const Params& p) {
    if (!(p.f_lo_hz > 0) || !(p.f_hi_hz > p.f_lo_hz) || p.points < 8)
        throw std::invalid_argument("pdn: bad frequency grid");

    // The reference: a plane net whose name says ground, else the largest
    // plane. Refusing when there is none — a PDN without a return is not a
    // network.
    Result r;
    for (const auto& n : board.nets) {
        std::string lo;
        for (char c : n.name) lo += (char)std::tolower((unsigned char)c);
        if (lo.find("gnd") != std::string::npos ||
            lo.find("vss") != std::string::npos) {
            r.gnd_net = n.id;
            r.gnd_name = n.name;
            break;
        }
    }
    if (r.gnd_net < 0) {
        for (const auto& lm : screener.layer_models())
            if (lm.is_plane && lm.plane_net >= 0) {
                r.gnd_net = lm.plane_net;
                r.gnd_name = board.net_name(lm.plane_net);
                break;
            }
    }
    if (r.gnd_net < 0)
        throw std::invalid_argument(
            "pdn: no ground net found — nothing named GND/VSS and no plane to "
            "fall back on, so there is no return to measure against");

    // pads by component
    std::map<std::string, std::vector<const Pad*>> by_comp;
    for (const auto& pad : board.pads) by_comp[pad.component].push_back(&pad);

    // component values
    std::map<std::string, const Component*> comps;
    for (const auto& c : board.components) comps[c.reference] = &c;

    // vias by net, for the mounting measurement
    std::map<int, std::vector<const Via*>> vias_by_net;
    for (const auto& v : board.vias) vias_by_net[v.net].push_back(&v);
    auto nearest_via_mm = [&](int net, double x, double y) -> double {
        auto it = vias_by_net.find(net);
        double best = 1e30;
        if (it != vias_by_net.end())
            for (const Via* v : it->second)
                best = std::min(best, std::hypot(v->x - x, v->y - y));
        return best;
    };

    std::map<int, Rail> rails;
    for (const auto& [ref, pads] : by_comp) {
        if (ref.empty() || ref[0] != 'C' || pads.size() < 2) continue;
        // which two nets does it sit between?
        int rail_net = -1;
        const Pad* rail_pad = nullptr;
        const Pad* gnd_pad = nullptr;
        for (const Pad* pad : pads) {
            if (pad->net == r.gnd_net) gnd_pad = pad;
            else if (pad->net > 0) { rail_net = pad->net; rail_pad = pad; }
        }
        if (rail_net < 0 || !gnd_pad || !rail_pad) continue;

        Rail& rail = rails[rail_net];
        rail.net = rail_net;
        rail.name = board.net_name(rail_net);

        auto ci = comps.find(ref);
        const std::string value = ci != comps.end() ? ci->second->value : "";
        const std::string fp = ci != comps.end() ? ci->second->footprint : "";
        auto c = parse_capacitance(value);
        if (!c || !(*c > 0)) { ++rail.skipped_unparsed; continue; }

        CapBranch b;
        b.ref = ref;
        b.c_f = *c;
        b.esl_h = esl_from_footprint(fp);
        b.package = fp;
        // the mounting loop, measured: each terminal's escape to its via
        b.via_d1_mm = nearest_via_mm(rail_net, rail_pad->x, rail_pad->y);
        b.via_d2_mm = nearest_via_mm(r.gnd_net, gnd_pad->x, gnd_pad->y);
        double l = 0;
        for (double d : {b.via_d1_mm, b.via_d2_mm}) {
            if (d > p.via_search_mm * 4) { b.no_via = true; d = p.via_search_mm * 4; }
            l += std::min(d, p.via_search_mm * 4) * p.nh_per_mm * 1e-9 +
                 p.via_nh * 1e-9;
        }
        b.l_mount_h = l;
        b.f_res_hz = 1.0 / (2.0 * 3.14159265358979323846 *
                            std::sqrt((b.esl_h + b.l_mount_h) * b.c_f));
        rail.caps.push_back(std::move(b));
    }

    // plane capacitance: the REAL overlap of the rail pour with the ground
    // pour, grid-sampled at 1 mm — coarse, but it is the actual copper
    for (auto& [net, rail] : rails) {
        const ZonePoly* zr = nullptr;
        const ZonePoly* zg = nullptr;
        for (const auto& z : board.zones) {
            if (z.net == net && !zr) zr = &z;
            if (z.net == r.gnd_net && !zg) zg = &z;
        }
        if (!zr || !zg || zr->cu == zg->cu) continue;
        double x1 = 1e30, y1 = 1e30, x2 = -1e30, y2 = -1e30;
        for (const auto& q : zr->pts) {
            x1 = std::min(x1, q.x); y1 = std::min(y1, q.y);
            x2 = std::max(x2, q.x); y2 = std::max(y2, q.y);
        }
        double area = 0;
        for (double x = x1 + 0.5; x < x2; x += 1.0)
            for (double y = y1 + 0.5; y < y2; y += 1.0)
                if (zr->contains(x, y) && zg->contains(x, y)) area += 1.0;
        rail.plane_overlap_mm2 = area;
        double h = 0, eps = 4.3;
        board.stackup.dielectric_between(std::min(zr->cu, zg->cu),
                                         std::max(zr->cu, zg->cu), h, eps);
        if (h > 0)
            rail.plane_c_f = 8.8541878128e-12 * eps * (area * 1e-6) / (h * 1e-3);
    }

    for (auto& [net, rail] : rails)
        if (!rail.caps.empty() || rail.plane_c_f > 0)
            r.rails.push_back(std::move(rail));
    std::sort(r.rails.begin(), r.rails.end(),
              [](const Rail& a, const Rail& b) { return a.caps.size() > b.caps.size(); });
    if (r.rails.empty())
        throw std::invalid_argument(
            "pdn: no decoupling capacitor with a parseable value found between "
            "a power net and " + r.gnd_name +
            " — either the board has none, or the value fields are not "
            "capacitances");
    return r;
}

// ---------------------------------------------------------------------------
// Impedance
// ---------------------------------------------------------------------------

inline std::complex<double> impedance_at(const Rail& rail, const Params& p,
                                         double f_hz) {
    const std::complex<double> jw(0.0, 2.0 * 3.14159265358979323846 * f_hz);
    std::complex<double> y = 0.0;
    // VRM: an R-L that owns the low end
    y += 1.0 / (p.vrm_r_ohm + jw * p.vrm_l_h);
    for (const auto& c : rail.caps)
        y += 1.0 / (c.esr_ohm + jw * (c.esl_h + c.l_mount_h) +
                    1.0 / (jw * c.c_f));
    if (rail.plane_c_f > 0) y += jw * rail.plane_c_f;
    return 1.0 / y;
}

inline Curve curve(const Rail& rail, const Params& p) {
    Curve c;
    c.f_hz.reserve(p.points);
    c.z_ohm.reserve(p.points);
    const double lr = std::log(p.f_hi_hz / p.f_lo_hz);
    for (int i = 0; i < p.points; ++i) {
        const double f = p.f_lo_hz * std::exp(lr * i / (p.points - 1));
        const double z = std::abs(impedance_at(rail, p, f));
        c.f_hz.push_back(f);
        c.z_ohm.push_back(z);
        if (f >= 1e5 && f <= 1e8 && z > c.z_max_ohm) {
            c.z_max_ohm = z;
            c.z_max_hz = f;
        }
    }
    // anti-resonances: local maxima — where paralleled capacitors fight
    for (int i = 1; i + 1 < p.points; ++i)
        if (c.z_ohm[i] > c.z_ohm[i - 1] && c.z_ohm[i] > c.z_ohm[i + 1])
            c.antires.push_back({c.f_hz[i], c.z_ohm[i]});
    return c;
}

}  // namespace faraday::pdn
