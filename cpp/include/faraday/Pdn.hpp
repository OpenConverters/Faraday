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
#include "Values.hpp"

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

// Component values and package ESL now live in Values.hpp — the screener
// needs the same parser (Y-capacitor resonance) and cannot include this
// header, since this one includes it. Re-exported here so every existing
// pdn::parse_capacitance call site keeps working.
using values::normalize_micro;
using values::parse_capacitance;
using values::parse_inductance;
using values::esl_from_footprint;

// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------

struct CapBranch {
    std::string ref;
    double c_f = 0;
    double esl_h = 0;          // package
    double l_mount_h = 0;      // measured off the board — the differentiator
    // 0.015 is an ASSUMPTION, and it used to be a silent one on every board. It
    // stays as the fallback because the impedance model needs a number, but a
    // branch now says which of its figures are measured, so a reader can tell a
    // datasheet ESR from this.
    double esr_ohm = 0.015;
    bool esr_measured = false;   // from the catalogue, not the constant above
    bool esl_measured = false;   // from the catalogue, not the package table
    bool c_measured = false;     // from the catalogue, not the value string
    std::string mpn;             // the part the catalogue matched, when it did
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
    // Name the reference explicitly. An isolated converter has two legitimate
    // returns and only the designer knows which side is being asked about; an
    // unknown name THROWS rather than falling back to the guess.
    std::string gnd_net;
};

struct Curve {
    std::vector<double> f_hz, z_ohm;
    double z_max_ohm = 0, z_max_hz = 0;     // in the 100 kHz - 100 MHz band
    std::vector<std::pair<double, double>> antires;   // f, z of local maxima
};

// Every net that could have been the reference, and the evidence for it. The
// choice used to be "the first net whose name contains gnd or vss", in net
// TABLE ORDER — which on an isolated board (a PoE front end with VSS_POE
// beside the real GND) picked whichever the exporter happened to list first,
// and then found no decoupling at all because the capacitors are on the other
// one. Carrying the alternatives means the refusal can say what it looked at.
struct GroundCandidate {
    int net = -1;
    std::string name;
    double pour_mm2 = 0;       // copper pour on this net — the physical reference
    int cap_terminals = 0;     // capacitor pads landing on it
    bool named = false;        // its name says ground
    bool plane = false;        // a layer references it as its plane
};

struct Result {
    std::vector<Rail> rails;
    int gnd_net = -1;
    std::string gnd_name;
    std::vector<GroundCandidate> gnd_candidates;   // best first
};

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

inline Result discover(const BoardIR& board, const Screener& screener,
                       const Params& p) {
    if (!(p.f_lo_hz > 0) || !(p.f_hi_hz > p.f_lo_hz) || p.points < 8)
        throw std::invalid_argument("pdn: bad frequency grid");

    // THE REFERENCE IS DECIDED BY THE COPPER, not by the net table's order.
    // A name that says ground makes a net a CANDIDATE; what makes it the
    // reference is the pour it carries and the capacitors that return to it.
    Result r;
    {
        std::set<int> plane_nets;
        for (const auto& lm : screener.layer_models())
            if (lm.is_plane && lm.plane_net >= 0) plane_nets.insert(lm.plane_net);

        std::map<int, double> pour;
        for (const auto& z : board.zones) {
            if (z.net <= 0) continue;
            double a = std::abs(z.signed_area());
            for (const auto& h : z.holes) {
                ZonePoly hp;
                hp.pts = h;
                a -= std::abs(hp.signed_area());
            }
            pour[z.net] += std::max(0.0, a);
        }
        std::map<std::string, std::vector<const Pad*>> by_comp;
        for (const auto& pad : board.pads) by_comp[pad.component].push_back(&pad);
        std::map<int, int> cap_terminals;
        for (const auto& [ref, pads] : by_comp) {
            if (ref.empty() || ref[0] != 'C') continue;
            for (const Pad* pad : pads)
                if (pad->net > 0) ++cap_terminals[pad->net];
        }

        for (const auto& n : board.nets) {
            std::string lo;
            for (char c : n.name) lo += (char)std::tolower((unsigned char)c);
            const bool named = lo.find("gnd") != std::string::npos ||
                               lo.find("vss") != std::string::npos;
            const bool plane = plane_nets.count(n.id) > 0;
            if (!named && !plane) continue;
            GroundCandidate c;
            c.net = n.id;
            c.name = n.name;
            c.named = named;
            c.plane = plane;
            c.pour_mm2 = pour.count(n.id) ? pour[n.id] : 0.0;
            c.cap_terminals = cap_terminals.count(n.id) ? cap_terminals[n.id] : 0;
            r.gnd_candidates.push_back(std::move(c));
        }
        // Most copper first; then the most capacitors returning to it; a name
        // that says ground breaks a genuine tie. Order in the net table never
        // decides anything.
        std::sort(r.gnd_candidates.begin(), r.gnd_candidates.end(),
                  [](const GroundCandidate& a, const GroundCandidate& b) {
                      if (a.pour_mm2 != b.pour_mm2) return a.pour_mm2 > b.pour_mm2;
                      if (a.cap_terminals != b.cap_terminals)
                          return a.cap_terminals > b.cap_terminals;
                      if (a.named != b.named) return a.named;
                      return a.net < b.net;
                  });
        if (!p.gnd_net.empty()) {
            for (const auto& n : board.nets)
                if (n.name == p.gnd_net) { r.gnd_net = n.id; r.gnd_name = n.name; }
            if (r.gnd_net < 0)
                throw std::invalid_argument(
                    "pdn: no net named '" + p.gnd_net +
                    "' on this board — the reference was named explicitly and "
                    "does not exist, which is a typo rather than a fallback");
        } else if (!r.gnd_candidates.empty()) {
            r.gnd_net = r.gnd_candidates.front().net;
            r.gnd_name = r.gnd_candidates.front().name;
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
        // The catalogue is consulted BEFORE the gate below, because it can open
        // it. An Altium export carries part numbers and no values, so every one
        // of a board's capacitors used to be dropped here as "unparseable" —
        // and stayed dropped even after Kelvin had identified all 73 of them
        // and knew their capacitance. The identification was worth nothing to
        // the PDN, which is the opposite of the point.
        const values::PartData* pd = nullptr;
        if (auto it = board.part_data.find(ref); it != board.part_data.end())
            pd = &it->second;

        // The BOARD's own value wins where it has one: it is the designer's
        // stated intent, and it is the same rule apply_values follows ("a value
        // the board itself carries always beats the side file"). The catalogue
        // fills the silence rather than overriding speech.
        auto c = parse_capacitance(value);
        double c_f = (c && *c > 0) ? *c : 0.0;
        bool c_from_catalogue = false;
        if (!(c_f > 0) && pd && values::has(pd->c_f)) {
            c_f = pd->c_f;
            c_from_catalogue = true;
        }
        if (!(c_f > 0)) { ++rail.skipped_unparsed; continue; }

        CapBranch b;
        b.ref = ref;
        b.c_f = c_f;
        b.c_measured = c_from_catalogue;
        b.esl_h = esl_from_footprint(fp);
        b.package = fp;
        // The catalogue's measured figures REPLACE the guesses, field by field:
        // a part may publish an ESR and no ESL, and taking the pair together
        // would throw away the half that is real. Nothing here defaults — an
        // absent figure leaves the estimate in place and says so.
        if (pd) {
            b.mpn = pd->mpn;
            if (values::has(pd->esr_ohm)) { b.esr_ohm = pd->esr_ohm; b.esr_measured = true; }
            if (values::has(pd->esl_h))   { b.esl_h = pd->esl_h;     b.esl_measured = true; }
        }
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
    if (r.rails.empty()) {
        // Say what was chosen, on what evidence, and what else was available.
        // On an isolated board the answer is usually "you asked about the other
        // ground", and the user can only see that if the alternatives are named.
        std::string why = "pdn: no decoupling capacitor with a parseable value "
                          "found between a power net and " + r.gnd_name;
        for (const auto& c : r.gnd_candidates)
            if (c.net == r.gnd_net) {
                char b[220];
                std::snprintf(b, sizeof b,
                              " (chosen as the reference: %.0f mm2 of pour, %d "
                              "capacitor terminal(s))",
                              c.pour_mm2, c.cap_terminals);
                why += b;
            }
        if (r.gnd_candidates.size() > 1) {
            why += ". Other returns on this board: ";
            size_t shown = 0;
            for (const auto& c : r.gnd_candidates) {
                if (c.net == r.gnd_net || shown >= 3) continue;
                char b[200];
                std::snprintf(b, sizeof b, "%s%s (%.0f mm2, %d cap terminals)",
                              shown ? ", " : "", c.name.c_str(), c.pour_mm2,
                              c.cap_terminals);
                why += b;
                ++shown;
            }
            why += ". If this board is isolated, name the side you mean.";
        }
        // "The board has none" and "the values are unreadable" are different
        // problems with different fixes, and the caller cannot tell them apart
        // from the outside. Count them.
        int caps_total = 0, caps_unparsed = 0, caps_empty = 0;
        std::string examples, part_example;
        for (const auto& [ref, pads] : by_comp) {
            if (ref.empty() || ref[0] != 'C' || pads.size() < 2) continue;
            ++caps_total;
            auto ci = comps.find(ref);
            const std::string value = ci != comps.end() ? ci->second->value : "";
            if (parse_capacitance(value)) continue;
            ++caps_unparsed;
            if (value.empty()) {
                ++caps_empty;
                // The PART NUMBER, which is a field of its own. This used to
                // read the footprint, because an Altium ODB++ import put the
                // ordering code there — it no longer does, so the message was
                // offering a package name ("CC3225-1210") as the part number to
                // go and resolve. A board with no part number either is a
                // different problem and must not be described as this one.
                if (part_example.empty() && ci != comps.end()) {
                    // Same rule as parts_without_values, and for the same
                    // reason: ODB++ has a part-number field, KiCad does not and
                    // people type the ordering code into the footprint slot. A
                    // footprint that is merely a PACKAGE is not an answer —
                    // offering "CC3225-1210" as the number to go and resolve
                    // sends someone looking up a case size.
                    const std::string& pn = ci->second->part_number;
                    const std::string& fp2 = ci->second->footprint;
                    if (!pn.empty())
                        part_example = ref + " -> '" + pn + "'";
                    else if (!fp2.empty() && !looks_like_package(fp2))
                        part_example = ref + " -> '" + fp2 + "'";
                }
            }
            if (caps_unparsed <= 3)
                examples += (examples.empty() ? "" : ", ") + ref + "='" + value + "'";
        }
        char cb[320];
        std::snprintf(cb, sizeof cb,
                      " This board carries %d two-terminal capacitor(s); %d of "
                      "them have a value this refuses to guess at%s%s.",
                      caps_total, caps_unparsed,
                      examples.empty() ? "" : " (e.g. ",
                      examples.empty() ? "" : (examples + ")").c_str());
        why += cb;
        // The commonest cause by far, and it is not the user's fault: some CAD
        // exports carry the manufacturer PART NUMBER and no value at all
        // (Altium's ODB++ does). The values are not lost, they are in the
        // catalogue that knows those part numbers — so say how to get them
        // back rather than leaving "no capacitors" as the last word.
        if (caps_empty > 0 && caps_empty * 2 >= caps_total && !part_example.empty()) {
            char pb[420];
            std::snprintf(pb, sizeof pb,
                          " %d of them carry NO value at all — this export "
                          "wrote part numbers instead (%s). Supply a "
                          "refdes,value table and every model here works: in "
                          "the app, 'load component values'; on the command "
                          "line, --parts-out parts.csv to ask, kelvin-values "
                          "to resolve the part numbers, --values values.csv to "
                          "hand them back.",
                          caps_empty, part_example.c_str());
            why += pb;
        }
        throw std::invalid_argument(why);
    }
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
