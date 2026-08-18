#pragma once
// Whole-board EMC screening: pure computational geometry + the closed forms
// in Tline.hpp. No field solving here — this tier finds and RANKS risk, and
// selected pairs graduate to the FEA/SPICE tier.
//
// Rules implemented (P0):
//   coupled-run        parallel segments, same layer (edge) or adjacent signal
//                      layers (broadside), scored by saturated-NEXT estimate;
//                      switch-node aggressors get a severity boost
//   diff-pair          a coupled run whose two nets form a differential pair
//                      by name (P/N, +/-, H/L, DP/DM) — intentional coupling,
//                      reported as info, never as a defect
//   3w                 minimum edge separation < 2×width on the wider trace
//   plane-crossing     signal run over a spot where NO plane covers (hard
//                      return-path break)
//   sparse-reference   aggregated: runs whose NEAREST plane is void but a
//                      farther plane covers — the return current detours,
//                      one finding per (signal layer, nearest plane)
//   switch-node        converter dv/dt aggressor found by CONNECTIVITY: an
//                      inductor + FET net (buck/boost), or a half-bridge
//                      midpoint carrying power copper (inverter, where the
//                      motor is the inductance); copper extent is reported
//   commutation-loop   the input-cap -> switch pair -> return loop whose
//                      ENCLOSED AREA dominates converter radiated emissions
//   via-stub           via barrel longer than the layers the net uses; the
//                      unused length is a lambda/4 open-stub resonator
//   dangling-stub      a track end reaching no pad, via or track — an open
//                      stub that radiates and loads its driver
//   decoupling-distance  a rail<->pour capacitor too far from the IC pin it
//                      serves: the loop is then track/via inductance, not C
//   no-reference-plane copper layer carrying signals with no plane to return on
//   coverage notes     approximated arcs, missing outline, dropped findings,
//                      zone-only routed nets invisible to segment coupling
//                      (no silent caps: everything not analysed is reported)

#include "BoardIR.hpp"
#include "Values.hpp"
#include "CriticalMesh.hpp"
#include "Tline.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace faraday {

struct ScreenerParams {
    double min_run_mm = 1.0;         // ignore parallel overlaps shorter than this
    double angle_tol_deg = 15.0;     // "parallel" tolerance
    double radius_factor = 4.0;      // screen out to d = factor × h (k < −36 dB)
    double min_radius_mm = 1.0;      // floor for the screening radius
    double sample_step_mm = 1.0;     // plane-coverage sampling pitch
    double report_floor_db = -40.0;  // coupled-run findings below this are dropped (counted)
    double plane_coverage_min = 0.5; // zone area / board area to call a layer a plane
    // Above this many distinct COMPONENTS an L+Q net is a rail, not a switch
    // node. Components, not pads: a rail betrays itself by its crowd of
    // decoupling caps (each one a component), while a real 20 A switch node
    // stays compact in components even when its power FETs are multi-pad
    // packages — mppt-2420-lc's SW_NODE is 6 components but 22 pads, and the
    // old pad count silently rejected it (the worst class of false negative).
    int sw_max_components = 12;
    // A dv/dt node carries at most a snubber and a bootstrap cap; a crowd of
    // capacitor COMPONENTS is bulk/decoupling and marks a DC rail
    // (mppt-2420-hc's HV+ input rail: 5 caps; its SW_NODE: 1).
    int sw_max_caps = 3;
    // Isolated-converter (flyback/forward) switch node: FET drain + transformer
    // primary + RCD snubber and nothing else. Tighter than sw_max_components
    // because this shape has no inductor to anchor it — the PoE flyback's
    // NetD17_3 is {T7 FET, T2 transformer, D17+R29 snubber, T5 gate helper}.
    int sw_flyback_max_components = 6;
    // A power-package switching device: SO-8/PowerPAK and up. Excludes the
    // 3-pad SOT-23 small-signal transistors that share the prefix.
    int sw_flyback_min_switch_pads = 8;
    // A wound part (inductor or transformer), by pad count.
    int sw_magnetic_min_pads = 4;
    // Nets the USER declares to be switch nodes (by exact name). Promoted
    // candidates from the UI land here; provenance is recorded as
    // switchNodeSource "user" so an exported report never claims the
    // heuristic found them. An unknown name THROWS — no silent skip.
    std::vector<std::string> user_switch_nets;
    double min_via_stub_mm = 0.3;    // ignore stubs shorter than this
    double min_dangling_mm = 1.0;    // ignore dangling ends shorter than this
    double decoupling_far_mm = 6.0;  // beyond this a decoupling cap is "reaching"
    size_t max_individual_breaks = 8;  // more hard breaks on one plane -> roll up
    double return_pour_fraction = 0.2; // pour >= this share of the board = a return net
    size_t max_findings = 200;       // hard cap on emitted findings (dropped count reported)

    // ---- immunity (ABT #796) ----
    // IEC 61000-4-2 contact discharge: ~30 A first peak in under a nanosecond
    // at 8 kV. 30 A/ns is the figure the standard's waveform gives and the one
    // TVS application notes quote; it is a PARAMETER here because it is the
    // multiplier on every voltage this rule family reports.
    double esd_di_dt_a_per_ns = 30.0;
    double esd_nh_per_mm = 0.8;      // same escape-inductance rule of thumb as the PDN
    double esd_via_nh = 0.3;         // per barrel
    // Above this, the copper between the connector pin and its clamp is worth
    // reporting: at 30 A/ns, 4 mm of it is ~96 V the clamp never sees.
    double esd_clamp_far_mm = 4.0;
    // The clamp's OWN return: pad to the nearest return via.
    double esd_return_far_mm = 2.5;
    // How far a diode-class part may sit from a connector pin and still be
    // called that pin's clamp. Beyond this the association is fiction: the
    // MPPT has a power diode 66 mm from a connector pin on the same net, and
    // calling that a badly-placed TVS invents both a part role and a defect.
    // Past this distance the pin is simply not protected NEAR the connector,
    // which is what the coverage finding then says.
    double esd_clamp_assoc_max_mm = 25.0;
    size_t esd_max_pins_listed = 6;  // roll-up width for unprotected pins

    // ---- the filter block (ABT #795) ----
    // Copper within this radius of the choke's own body is the FOOTPRINT, not
    // a bypass path: the two sides necessarily meet at the part.
    double filter_local_mm = 3.0;
    // A coupling path across the filter worse than this caps what the filter
    // can achieve, and is worth saying out loud.
    double filter_couple_floor_db = -45.0;
    // Switching copper closer than this to the filter's CONNECTOR side walks
    // around the filter through the air rather than through it.
    double filter_bypass_mm = 10.0;
};

struct FindingGeom {
    // overlay primitives, board coords (mm)
    std::vector<Segment> lines;   // highlighted runs (net/cu meaningful)
    std::vector<Point> markers;   // point markers (plane-crossing samples, ...)
};

struct Finding {
    std::string id;          // "XTALK-0001"
    std::string rule;        // coupled-run | 3w | plane-crossing | no-reference-plane | note
    double severity = 0.0;   // rank key, 0..1
    std::string severity_label;  // high | medium | low | info
    std::string confidence;      // screening-estimate | geometric-only | exact
    std::string title;
    std::string detail;
    std::string remediation;
    std::optional<double> next_db;   // saturated NEXT estimate (coupled-run)
    double coupled_len_mm = 0.0;
    double min_sep_mm = 0.0;         // minimum EDGE separation seen
    int net_a = -1, net_b = -1;
    int cu_a = -1, cu_b = -1;
    FindingGeom geom;
    // Everything the field solver needs to rebuild this pair's cross-section.
    // Without it the deep tier would have to be re-parameterised by hand,
    // which is exactly the transcription step a tool like this exists to
    // remove. Absent when the layer has no reference plane, because then
    // there is no cross-section to solve.
    std::optional<nlohmann::json> solve;
    // Inputs for the radiated-emission estimate. Present on findings that
    // enclose a current loop, whose AREA is the one term the layout uniquely
    // determines and every other estimator makes you measure by hand.
    std::optional<nlohmann::json> emit;
};

// Per-copper-layer derived model: plane classification + reference geometry.
struct LayerModel {
    bool is_plane = false;
    int plane_net = -1;          // dominant pour net when is_plane
    double zone_coverage = 0.0;  // pour area / board area
    int ref_up = -1, ref_dn = -1;      // nearest plane layer ordinal above/below
    double h_up = 0.0, h_dn = 0.0;     // dielectric height to it, mm
    double eps_up = 1.0, eps_dn = 1.0; // effective eps_r of that dielectric
};

namespace detail {

// A conductor edge the coupling engine can see: either a routed track or one
// boundary edge of a copper pour. Pours carry the high-current paths on
// converter boards, so leaving them out made those paths invisible.
struct SegRef {
    int net, cu;
    double x1, y1, x2, y2, w, len;
    double ux, uy;                   // unit direction
    bool pour_edge = false;          // true => this is a zone outline edge
    size_t id = 0;                   // index within its layer, for dedup
};

// Uniform grid per layer. Cell size = screening radius; a segment is inserted
// into every cell its bbox touches, queries return candidate indices.
class Grid {
  public:
    Grid(double cell) : cell_(cell) {}
    void insert(const SegRef& s, size_t slot) {
        for_cells(s, 0.0, [&](long long key) { cells_[key].push_back(slot); });
    }
    template <typename F>
    void query(const SegRef& s, double radius, F&& f) const {
        std::set<size_t> seen;
        for_cells(s, radius, [&](long long key) {
            auto it = cells_.find(key);
            if (it == cells_.end()) return;
            for (size_t slot : it->second)
                if (seen.insert(slot).second) f(slot);
        });
    }

  private:
    template <typename F>
    void for_cells(const SegRef& s, double pad, F&& f) const {
        double x1 = std::min(s.x1, s.x2) - pad, x2 = std::max(s.x1, s.x2) + pad;
        double y1 = std::min(s.y1, s.y2) - pad, y2 = std::max(s.y1, s.y2) + pad;
        long long cx1 = (long long)std::floor(x1 / cell_), cx2 = (long long)std::floor(x2 / cell_);
        long long cy1 = (long long)std::floor(y1 / cell_), cy2 = (long long)std::floor(y2 / cell_);
        for (long long cx = cx1; cx <= cx2; ++cx)
            for (long long cy = cy1; cy <= cy2; ++cy)
                f(cx * 1000003LL + cy);
    }
    double cell_;
    std::map<long long, std::vector<size_t>> cells_;
};

struct Overlap {
    double length;    // parallel overlap length, mm
    double center_d;  // mean centerline separation over the overlap, mm
    Segment span_a, span_b;  // the overlapping portions (for the overlay)
};

// Parallel-overlap of two near-parallel segments; nullopt when the pair is
// not parallel within tol or the projected overlap is below min_run.
inline std::optional<Overlap> parallel_overlap(const SegRef& a, const SegRef& b,
                                               double min_run, double cos_tol) {
    double dot = a.ux * b.ux + a.uy * b.uy;
    if (std::abs(dot) < cos_tol) return std::nullopt;
    // project b's endpoints onto a's axis
    double t1 = (b.x1 - a.x1) * a.ux + (b.y1 - a.y1) * a.uy;
    double t2 = (b.x2 - a.x1) * a.ux + (b.y2 - a.y1) * a.uy;
    double lo = std::max(0.0, std::min(t1, t2));
    double hi = std::min(a.len, std::max(t1, t2));
    if (hi - lo < min_run) return std::nullopt;
    // perpendicular distances of b's endpoints from a's centerline
    double d1 = std::abs(a.ux * (b.y1 - a.y1) - a.uy * (b.x1 - a.x1));
    double d2 = std::abs(a.ux * (b.y2 - a.y1) - a.uy * (b.x2 - a.x1));
    Overlap ov;
    ov.length = hi - lo;
    ov.center_d = 0.5 * (d1 + d2);
    auto pt_a = [&](double t) { return Point{a.x1 + a.ux * t, a.y1 + a.uy * t}; };
    Point pa1 = pt_a(lo), pa2 = pt_a(hi);
    ov.span_a = {0, 0, pa1.x, pa1.y, pa2.x, pa2.y, a.w};
    // matching span on b: clamp the projection of a's overlap back onto b
    double s1 = (pa1.x - b.x1) * b.ux + (pa1.y - b.y1) * b.uy;
    double s2 = (pa2.x - b.x1) * b.ux + (pa2.y - b.y1) * b.uy;
    s1 = std::clamp(s1, 0.0, b.len);
    s2 = std::clamp(s2, 0.0, b.len);
    ov.span_b = {0, 0, b.x1 + b.ux * s1, b.y1 + b.uy * s1,
                 b.x1 + b.ux * s2, b.y1 + b.uy * s2, b.w};
    return ov;
}

}  // namespace detail

// Differential-pair recognition by net name: identical names except a final
// P/N, +/-, H/L or P/M (DP/DM) designator. Conservative on purpose — a wrong
// pairing silences a real finding, a missed pairing only leaves an info-grade
// false positive (e.g. PWM_HS/PWM_LS differ in the second-to-last char and are
// correctly NOT paired).
inline bool is_differential_pair_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    size_t i = a.size() - 1;
    if (a.compare(0, i, b, 0, i) != 0) return false;
    char x = std::toupper(static_cast<unsigned char>(a[i]));
    char y = std::toupper(static_cast<unsigned char>(b[i]));
    if (x == y) return false;
    if (x > y) std::swap(x, y);
    return (x == 'N' && y == 'P') || (x == 'H' && y == 'L') ||
           (x == 'M' && y == 'P') || (x == '+' && y == '-');
}

class Screener {
  public:
    Screener(const BoardIR& board, ScreenerParams params = {})
        : b_(board), p_(params) {
        build_layer_models();
        build_sw_nets();
        build_routed_lengths();
    }

    const std::vector<LayerModel>& layer_models() const { return layers_; }

    // Whether a net carries a substantial pour — i.e. is a return/reference
    // net. Public because the near-field map needs to know what a shield can
    // could be BONDED to, and "the return copper" is the screener's judgement,
    // not something a caller should re-derive.
    bool is_return_net(int net) const { return is_pour_net(net); }

    // Nets the switch-node rule identified. Exposed because the radiation map
    // has to give those nets the switched current rather than swing / Z0 —
    // they are the loudest copper on a converter by a wide margin.
    bool is_switch_node(int net) const { return sw_nets_.count(net) > 0; }

    // The nets the switch-node rule identified. The near-field map is built
    // around switching aggressors, so it needs the set, not just a predicate.
    const std::set<int>& switch_nets() const { return sw_nets_; }

    // The commutation loop of a switch net: enclosed area, the hull that
    // bounds it, and the nearest bulk capacitor. Public because the near-field
    // map reduces that loop to a magnetic dipole — the area is the one factor
    // in m = N*I*A that geometry supplies exactly.
    struct LoopResult {
        double area_mm2 = 0;
        double cap_dist_mm = 0;
        std::string cap_ref;
        // the loop cap's nets — the Stromumschaltanalyse on the derived
        // netlist showed the cap can be a DOMAIN STITCH (both sides pours),
        // which means the critical mesh closes across two ground domains
        std::vector<int> cap_nets;
        std::vector<Point> hull;
        // set when the mesh was DERIVED by the current-switching analysis
        // (CriticalMesh.hpp) rather than pattern-matched: the members are
        // the XOR branches, named in the finding
        std::vector<std::string> members;
        std::string shape;   // "two-device" | "magnetic-clamp" | "" (geometric)
    };
    std::optional<LoopResult> commutation_loop(int sw_net) const {
        return commutation_loop_impl(sw_net);
    }

    // extra: findings produced OUTSIDE the screener (the PDN anti-resonance
    // pass lives with the PDN model in Report.hpp) that must enter the same
    // ranking, cap and ID pipeline — appended before the sort, never after.
    std::vector<Finding> run(std::vector<Finding> extra = {}) {
        std::vector<Finding> out = std::move(extra);
        find_no_reference_plane(out);
        find_switch_nodes(out);
        find_commutation_loops(out);
        find_coupled_runs(out);
        find_plane_crossings(out);
        find_via_stubs(out);
        find_dangling_stubs(out);
        find_decoupling(out);
        find_connector_ground_spread(out);
        find_plane_cavity_modes(out);
        find_cap_via_stubs(out);
        find_edge_radiation(out);
        find_diff_skew(out);
        find_esd_clamp_paths(out);
        find_unprotected_connector_pins(out);
        find_filter_coupling(out);
        find_y_cap_return(out);
        find_filter_bypass(out);
        // rank: severity desc, then coupled length desc
        std::sort(out.begin(), out.end(), [](const Finding& x, const Finding& y) {
            if (x.severity != y.severity) return x.severity > y.severity;
            return x.coupled_len_mm > y.coupled_len_mm;
        });
        // Fair-share selection before the cap. A global sort + truncate lets
        // one chatty rule eat every slot (6 of 9 corpus boards capped; ulx3s
        // discarded 2137 findings, mostly to coupled-run). Round-robin one
        // finding per rule, highest severity first within each rule, so every
        // rule is represented; the SELECTED set is then ranked for display.
        dropped_by_cap_ = 0;
        if (out.size() > p_.max_findings) {
            std::map<std::string, std::vector<Finding>> by_rule;
            for (auto& f : out) by_rule[f.rule].push_back(std::move(f));
            for (auto& [rule, v] : by_rule)
                std::sort(v.begin(), v.end(), [](const Finding& a, const Finding& b) {
                    return a.severity > b.severity;
                });
            std::vector<Finding> picked;
            picked.reserve(p_.max_findings);
            for (size_t round = 0; picked.size() < p_.max_findings; ++round) {
                bool any = false;
                for (auto& [rule, v] : by_rule) {
                    if (round >= v.size()) continue;
                    any = true;
                    picked.push_back(std::move(v[round]));
                    if (picked.size() >= p_.max_findings) break;
                }
                if (!any) break;
            }
            dropped_by_cap_ = out.size() - picked.size();
            out = std::move(picked);
            std::sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) {
                if (a.severity != b.severity) return a.severity > b.severity;
                return a.coupled_len_mm > b.coupled_len_mm;
            });
        }
        // ids after ranking
        for (size_t i = 0; i < out.size(); ++i) {
            char idb[32];
            std::snprintf(idb, sizeof idb, "F-%04zu", i + 1);
            out[i].id = idb;
        }
        return out;
    }

    // ---- the dv/dt copper -------------------------------------------------
    // The plate that drives common-mode current into the chassis: every square
    // millimetre of copper that swings with the switching edge. It is measured
    // off the layout exactly the way the commutation loop is, which is what
    // lets the conducted estimate stop asking the user to invent a C_stray
    // (emc::chassis_stray_c_f takes this area and the one number the board
    // cannot carry — how far the metalwork is).
    //
    // Tracks, pads and pours on a switch net, summed. Where a track lands on
    // its own pad the overlap is counted twice; that is a few percent, and it
    // errs toward MORE capacitance, which is the pessimistic direction for an
    // emissions estimate. Via barrels are not counted: their plate area toward
    // a chassis under the board is the annular ring, which is already in the
    // pad sum, and the barrel itself faces the board's own layers.
    nlohmann::json dvdt_copper() const {
        std::map<int, double> per_net;
        for (const auto& s : b_.segments) {
            if (!sw_nets_.count(s.net)) continue;
            const double len = std::hypot(s.x2 - s.x1, s.y2 - s.y1);
            per_net[s.net] += len * s.width;
        }
        for (const auto& p : b_.pads) {
            if (!sw_nets_.count(p.net)) continue;
            per_net[p.net] += p.w * p.h;
        }
        for (const auto& z : b_.zones) {
            if (!sw_nets_.count(z.net)) continue;
            double a = std::abs(z.signed_area());
            for (const auto& h : z.holes) {
                ZonePoly hp;
                hp.pts = h;
                a -= std::abs(hp.signed_area());
            }
            per_net[z.net] += std::max(0.0, a);
        }
        double total = 0;
        nlohmann::json nets = nlohmann::json::array();
        for (const auto& [net, mm2] : per_net) {
            total += mm2;
            nets.push_back({{"net", b_.net_name(net)}, {"mm2", mm2}});
        }
        return {{"totalMm2", total}, {"perNet", nets}};
    }

    // The line filter(s) the screener recognised, by shape: the choke, its two
    // sides, and the X/Y capacitors that make it a filter rather than a
    // transformer. Stated in the meta strip so that a board where NO filter
    // was recognised says so — the negative of a heuristic has to be as
    // visible as its positive (ABT #410, again).
    nlohmann::json filter_blocks_json() const {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& fb : find_filter_blocks()) {
            nlohmann::json in = nlohmann::json::array(), o = nlohmann::json::array();
            for (int n : fb.in_nets) in.push_back(b_.net_name(n));
            for (int n : fb.out_nets) o.push_back(b_.net_name(n));
            out.push_back({{"choke", fb.choke_ref},
                           {"inNets", in}, {"outNets", o},
                           {"xCaps", fb.x_caps}, {"yCaps", fb.y_caps},
                           {"sideKnown", fb.side_known}});
        }
        return out;
    }

    // Coverage/meta for the report — everything not analysed, stated.
    nlohmann::json meta() const {
        nlohmann::json planes = nlohmann::json::array();
        for (size_t i = 0; i < layers_.size(); ++i)
            planes.push_back({{"layer", b_.copper_names[i]},
                              {"isPlane", layers_[i].is_plane},
                              {"planeNet", layers_[i].plane_net},
                              {"zoneCoverage", layers_[i].zone_coverage},
                              {"routedMm", routed_mm_[i]}});
        nlohmann::json unverifiable = nlohmann::json::array();
        for (const auto& n : unverifiable_planes_) unverifiable.push_back(n);
        nlohmann::json sw = nlohmann::json::array();
        for (int n : sw_nets_) sw.push_back(b_.net_name(n));
        // provenance per node ("heuristic" | "user") — an exported report
        // must never claim the heuristic found what the user declared
        nlohmann::json sw_src = nlohmann::json::object();
        for (int n : sw_nets_)
            sw_src[b_.net_name(n)] = sw_user_.count(n) ? "user" : "heuristic";
        // nets that LOOK like a converter (wound part + active silicon, no
        // shunt cap, two filtered rails) but are externally isomorphic to a
        // linear regulator's LC harness — offered for promotion, with the
        // evidence, never silently screened or silently dropped
        nlohmann::json sw_cand = nlohmann::json::array();
        for (const auto& c : sw_candidates_)
            sw_cand.push_back({{"net", b_.net_name(c.net)},
                               {"wound", c.wound},
                               {"active", c.active}});
        // nets routed ONLY as zones/polygons — now analysed at their pour
        // boundaries, but still listed so the reader knows which nets were
        // judged by outline rather than by track geometry
        nlohmann::json polyonly = nlohmann::json::array();
        {
            std::set<int> with_segments, with_zones;
            for (const auto& s : b_.segments) with_segments.insert(s.net);
            for (const auto& z : b_.zones)
                if (z.net > 0 && !is_pour_net(z.net)) with_zones.insert(z.net);
            for (int n : with_zones)
                if (!with_segments.count(n) && polyonly.size() < 20)
                    polyonly.push_back(b_.net_name(n));
        }
        return {{"planes", planes},
                {"approximatedArcs", b_.approximated_arcs},
                {"bboxFromOutline", b_.bbox_from_outline},
                {"stackupSource", b_.stackup.source},
                {"droppedBelowFloorDb", dropped_below_floor_},
                {"droppedByFindingCap", dropped_by_cap_},
                {"reportFloorDb", p_.report_floor_db},
                {"crossingCheckSkippedPlanes", unverifiable},
                {"diffPairsRecognized", diff_pairs_recognized_},
                {"switchNodes", sw},
                {"switchNodeSource", sw_src},
                {"switchNodeCandidates", sw_cand},
                {"dvdtCopper", dvdt_copper()},
                {"filterBlocks", filter_blocks_json()},
                {"polygonOnlyNets", polyonly}};
    }

    // The cross-section a coupled pair actually presents, in the form the
    // field solver takes. This is the bridge between the two tiers: the
    // screening pass already knows the widths, the separation, which layers
    // are involved and where their reference planes are, so the deep solve
    // should never have to be told any of it again.
    //
    // nullopt when the layer has no reference plane — there is then no
    // cross-section to solve, and inventing a height would be worse than
    // saying so.
    std::optional<nlohmann::json> cross_section_for(int cu_a, int cu_b,
                                                    double w_a, double w_b,
                                                    double sep_mm,
                                                    double len_mm) const {
        const LayerModel& lm = layers_[cu_a];
        const auto& cu_idx = b_.stackup.copper_indices();
        const double t = b_.stackup.layers[cu_idx[cu_a]].thickness_mm;
        if (w_a <= 0 || w_b <= 0) return std::nullopt;
        nlohmann::json j;
        j["w1Mm"] = w_a;
        j["w2Mm"] = w_b;
        j["tMm"] = t;
        j["lengthMm"] = len_mm;
        j["gapMm"] = std::max(sep_mm, 0.01);

        if (cu_a != cu_b) {                       // broadside: adjacent layers
            double hv = 0, eps = 0;
            b_.stackup.dielectric_between(std::min(cu_a, cu_b), std::max(cu_a, cu_b),
                                          hv, eps);
            const double h = ref_height(cu_a);
            if (!(h > 0) || !(hv > 0)) return std::nullopt;
            j["mode"] = "broadside";
            j["hMm"] = h;
            j["hvMm"] = hv;
            j["epsR"] = eps;
            j["lateralMm"] = 0.0;
            return j;
        }
        const bool up = lm.ref_up >= 0, dn = lm.ref_dn >= 0;
        if (up && dn) {                           // buried between two planes
            const double b = lm.h_up + lm.h_dn + t;
            j["mode"] = "stripline";
            j["bMm"] = b;
            j["epsR"] = (lm.eps_up * lm.h_up + lm.eps_dn * lm.h_dn) /
                        std::max(lm.h_up + lm.h_dn, 1e-9);
            return j;
        }
        if (!up && !dn) return std::nullopt;
        j["mode"] = "microstrip";
        j["hMm"] = up ? lm.h_up : lm.h_dn;
        j["epsR"] = up ? lm.eps_up : lm.eps_dn;
        return j;
    }

    // Z0 estimate for a trace of width w on copper layer cu — for tooltips.
    // nullopt when the layer has no reference plane (stated, never guessed).
    std::optional<double> z0_estimate(int cu, double w) const {
        const LayerModel& lm = layers_[cu];
        bool up = lm.ref_up >= 0, dn = lm.ref_dn >= 0;
        double t = b_.stackup.layers[b_.stackup.copper_indices()[cu]].thickness_mm;
        if (up && dn) {
            double b = lm.h_up + lm.h_dn;
            double eps = (lm.eps_up * lm.h_up + lm.eps_dn * lm.h_dn) / b;
            if (0.8 * w + t >= 1.9 * b) return std::nullopt;  // formula range exceeded
            return tline::stripline_z0(w, b, t, eps);
        }
        if (up) return tline::microstrip_z0(w, lm.h_up, lm.eps_up);
        if (dn) return tline::microstrip_z0(w, lm.h_dn, lm.eps_dn);
        return std::nullopt;
    }

  private:
    // ---- layer models: plane classification + reference geometry ----
    void build_layer_models() {
        size_t n = b_.copper_names.size();
        layers_.assign(n, {});
        double board_area = (b_.bbox_x2 - b_.bbox_x1) * (b_.bbox_y2 - b_.bbox_y1);
        if (board_area <= 0) throw BoardError("screener: degenerate board bbox");

        // Coverage is the UNION of a layer's fills, not the sum of their
        // areas: fills overlap (KiCad re-pours by priority, ODB++ stacks
        // surfaces over one region), and summing shoelace areas reported a
        // 175% "coverage" on BeagleBone's LYR2 — a number shown to the user
        // in the meta strip. The union is measured by scanline: per sample
        // row each zone yields even-odd x-crossings of its outer ring plus
        // holes — exactly the copper contains() sees — and intervals union
        // exactly in x; 192 rows keep the y-quantization error well under
        // anything the 50% plane threshold could notice.
        struct ZSpan { const ZonePoly* z; double y1, y2; };
        std::vector<std::vector<ZSpan>> zs(n);
        for (const auto& z : b_.zones) {
            ZSpan s{&z, 1e30, -1e30};
            for (const auto& p : z.pts) {
                s.y1 = std::min(s.y1, p.y);
                s.y2 = std::max(s.y2, p.y);
            }
            zs[z.cu].push_back(s);
        }
        auto crossings = [](const std::vector<Point>& ring, double y,
                            std::vector<double>& xs) {
            for (size_t i = 0, m = ring.size(), j = m - 1; i < m; j = i++) {
                const Point& a = ring[i];
                const Point& b = ring[j];
                if ((a.y > y) != (b.y > y))
                    xs.push_back((b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x);
            }
        };
        auto union_len = [](std::vector<std::pair<double, double>>& v) {
            std::sort(v.begin(), v.end());
            double total = 0.0, lo = 0.0, hi = 0.0;
            bool open = false;
            for (const auto& [a, b] : v) {
                if (!open || a > hi) {
                    if (open) total += hi - lo;
                    lo = a; hi = b; open = true;
                } else {
                    hi = std::max(hi, b);
                }
            }
            if (open) total += hi - lo;
            return total;
        };
        const int rows = 192;
        const double row_h = (b_.bbox_y2 - b_.bbox_y1) / rows;
        std::vector<std::map<int, double>> area_by_net(n);  // per-layer UNION
        for (size_t i = 0; i < n; ++i) {
            double covered = 0.0;
            std::vector<double> xs;
            for (int r = 0; r < rows; ++r) {
                double y = b_.bbox_y1 + row_h * (r + 0.5);
                std::map<int, std::vector<std::pair<double, double>>> by_net;
                for (const ZSpan& s : zs[i]) {
                    if (y < s.y1 || y > s.y2) continue;
                    xs.clear();
                    crossings(s.z->pts, y, xs);
                    for (const auto& h : s.z->holes) crossings(h, y, xs);
                    std::sort(xs.begin(), xs.end());
                    auto& iv = by_net[s.z->net];
                    for (size_t k = 0; k + 1 < xs.size(); k += 2)
                        iv.emplace_back(xs[k], xs[k + 1]);
                }
                std::vector<std::pair<double, double>> all;
                for (auto& [net, iv] : by_net) {
                    area_by_net[i][net] += union_len(iv) * row_h;
                    all.insert(all.end(), iv.begin(), iv.end());
                }
                covered += union_len(all) * row_h;
            }
            int dominant = -1;
            double dom_area = 0.0;
            for (const auto& [net, area] : area_by_net[i])
                if (area > dom_area) { dom_area = area; dominant = net; }
            layers_[i].zone_coverage = covered / board_area;
            // "power"-typed layers count as planes on the KiCad hint alone
            const auto& sl = b_.stackup.layers[b_.stackup.copper_indices()[i]];
            layers_[i].is_plane =
                layers_[i].zone_coverage >= p_.plane_coverage_min ||
                sl.copper_type == "power";
            layers_[i].plane_net = dominant;
        }
        // Reference layers are resolved for EVERY copper layer, including ones
        // that are themselves planes: a heavily-poured layer can serve as a
        // reference AND carry its own routing. (LibreSolar bms-c1 has 58-89%
        // pour on all four layers; treating "is a plane" as "carries no
        // signals" silently excluded 1097 segments and reported a clean board
        // — a false negative, far worse than a false positive.)
        for (size_t i = 0; i < n; ++i) {
            for (int j = (int)i - 1; j >= 0; --j)
                if (layers_[j].is_plane) {
                    layers_[i].ref_up = j;
                    b_.stackup.dielectric_between(j, i, layers_[i].h_up, layers_[i].eps_up);
                    break;
                }
            for (size_t j = i + 1; j < n; ++j)
                if (layers_[j].is_plane) {
                    layers_[i].ref_dn = (int)j;
                    b_.stackup.dielectric_between(i, j, layers_[i].h_dn, layers_[i].eps_dn);
                    break;
                }
        }
        // nets carrying a substantial pour anywhere (see is_pour_net) — the
        // same union areas, so stacked fills can't inflate a net over the
        // threshold
        std::map<int, double> pour_area;
        for (size_t i = 0; i < n; ++i)
            for (const auto& [net, a] : area_by_net[i])
                if (net > 0) pour_area[net] += a;
        for (auto& [net, a] : pour_area)
            if (a >= p_.return_pour_fraction * board_area) big_pour_nets_.insert(net);
    }

    // Called after build_sw_nets(), since is_pour_net() exempts switch nodes.
    void build_routed_lengths() {
        routed_mm_.assign(layers_.size(), 0.0);
        for (const auto& s : b_.segments)
            if (s.net > 0 && !is_pour_net(s.net))
                routed_mm_[s.cu] += std::hypot(s.x2 - s.x1, s.y2 - s.y1);
    }

    // ---- switch-node identification by CONNECTIVITY, not name-matching ----
    // A converter switch node joins the FET(s) to the inductor. Criteria, all
    // three required (each earned on real boards, 2026-07-27):
    //   * an EXACT "L" reference prefix — not "LED" (LED1 matched a naive
    //     first-char test on the MPPT board)
    //   * an EXACT "Q" reference prefix — "T" is far more often a test point
    //     than a European transistor (it produced 10 false hits on HackRF One)
    //   * few pads — a switch node is deliberately kept compact. HackRF's VAA
    //     rail has an inductor AND a load-switch FET but 75 pads; the MPPT
    //     switch node has 6. This is what separates a rail from a SW node.
    // Confidence stays "heuristic" and the finding asks the user to verify.
    static std::string ref_prefix(const std::string& ref) {
        std::string p;
        for (char c : ref) {
            if (!std::isalpha(static_cast<unsigned char>(c))) break;
            p.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
        return p;
    }

    // Which reference prefix marks a SWITCHING device on THIS board. The rule
    // above hardcoded ANSI "Q", which made every converter rule silent on an
    // IEC/DIN board: the PoE/USB-C flyback here names its FETs T1..T7, its
    // ICs IC1..IC6 and its test points TP1..TP21, and carries not one "Q".
    // "T" cannot simply be added to the set — on HackRF One T1..T4 ARE test
    // points (10 false hits when it was tried). So decide once per board, on
    // evidence: any "Q" at all means ANSI, and "T" keeps its test-point
    // reading; no "Q" plus IEC-style naming elsewhere ("IC" instead of "U",
    // or test points that already have their own "TP") means "T" is the
    // transistor prefix. A board with neither signal keeps the old "Q".
    std::string decide_sw_prefix() const {
        bool has_q = false, has_t = false, has_ic = false, has_tp = false;
        for (const auto& p : b_.pads) {
            if (p.component.empty()) continue;
            const std::string pre = ref_prefix(p.component);
            if (pre == "Q") has_q = true;
            else if (pre == "T") has_t = true;
            else if (pre == "IC") has_ic = true;
            else if (pre == "TP") has_tp = true;
        }
        if (!has_q && has_t && (has_ic || has_tp)) return "T";
        return "Q";
    }

    void build_sw_nets() {
        sw_prefix_ = decide_sw_prefix();
        // pads per component — the flyback test needs package size to tell a
        // power FET (SO-8/PowerPAK, 8+ pads) and a transformer (4+ pads) from
        // the 3-pad small-signal transistors that share their prefix
        std::map<std::string, int> pad_count;
        for (const auto& p : b_.pads)
            if (!p.component.empty()) ++pad_count[p.component];

        std::map<int, std::set<std::string>> prefixes;
        std::map<int, std::set<std::string>> comps, sw_comps, cap_comps,
            mag_comps;
        // every component's full net membership, INCLUDING plane nets — the
        // shunt-cap veto asks "does this cap land on the return?", and the
        // return is exactly what the per-net loop below filters out
        std::map<std::string, std::set<int>> comp_all_nets;
        for (const auto& p : b_.pads)
            if (p.net > 0 && !p.component.empty())
                comp_all_nets[p.component].insert(p.net);
        auto is_return_net = [&](int n) {
            std::string lo;
            for (char ch : b_.net_name(n))
                lo += (char)std::tolower((unsigned char)ch);
            return lo.find("gnd") != std::string::npos ||
                   lo.find("vss") != std::string::npos;
        };
        // a net is a FILTERED RAIL if a 2-pad capacitor ties it straight to a
        // return — and by the same physics, such a net can never be a switch
        // node (the cap would short the switch every cycle). mppt-2420-hc's
        // SUPPLY_INPUT carried L2+Q4 and screened as a converter for a day;
        // its two 1 uF caps to GND say it is a supply-ORing rail.
        std::set<int> capped_rails;
        for (const auto& [ref, ns] : comp_all_nets) {
            if (ref_prefix(ref) != "C" || pad_count[ref] != 2 || ns.size() != 2)
                continue;
            auto it = ns.begin();
            int a = *it++, c = *it;
            if (is_return_net(a) && !is_return_net(c)) capped_rails.insert(c);
            if (is_return_net(c) && !is_return_net(a)) capped_rails.insert(a);
        };
        auto has_shunt_cap = [&](int net) {
            return capped_rails.count(net) > 0;
        };
        for (const auto& p : b_.pads) {
            if (p.net <= 0 || p.component.empty() || is_plane_net(p.net)) continue;
            std::string pre = ref_prefix(p.component);
            prefixes[p.net].insert(pre);
            comps[p.net].insert(p.component);
            // switching devices only: counting diodes as bridge legs made
            // every power-button FET+D pair a "switch node" (ulx3s, bms-c1).
            // The async-buck case (one FET, one diode, one L) is buck_like's
            // job, not this one's.
            if (pre == sw_prefix_) sw_comps[p.net].insert(p.component);
            if (pre == "C") cap_comps[p.net].insert(p.component);
            // MAGNETICS, for the isolated case. A transformer is "T" in BOTH
            // conventions, so on an IEC board it shares the switching prefix
            // and only its pad count separates it from a FET; a wound part
            // has 4+ pads where even a power FET package tops out lower on
            // this board (T2 the flyback transformer: 10 pads).
            if ((pre == "L" || pre == "T" || pre == "TR") &&
                pad_count[p.component] >= p_.sw_magnetic_min_pads)
                mag_comps[p.net].insert(p.component);
        }
        for (auto& [net, pre] : prefixes) {
            if ((int)comps[net].size() > p_.sw_max_components)
                continue;   // a rail, not a node
            if ((int)cap_comps[net].size() > p_.sw_max_caps)
                continue;   // bulk/decoupling crowd: a DC rail, not a node
            bool buck_like = pre.count("L") && pre.count(sw_prefix_);
            // Inverter/half-bridge: several FETs meet with no inductor (the
            // motor IS the inductance — VESC found 0 switch nodes without
            // this). But GATE nets also gather Q pads (VESC's H1_LOW has 5),
            // so require (a) at least two distinct SWITCHING components — a
            // midpoint is bridged from both sides, where a rail behind one
            // FET sees only that FET (mppt-2420-lc's DCDC_IN), and (b) the
            // POWER path: the net must also reach a capacitor, an inductor or
            // a connector. VESC's phase nodes carry {Q,C,P,R,U}; its gate
            // nets carry only {Q,R,U}. Topology tests, not trace-width
            // guesses — widths overlap too much (gate nets run 2.29 mm).
            bool power_path = pre.count("C") || pre.count("L") ||
                              pre.count("P") || pre.count("J");
            bool bridge_like = sw_comps[net].size() >= 2 && power_path;
            // Isolated (flyback/forward): the drain node runs to a TRANSFORMER
            // primary, not to an inductor, and its only other company is the
            // RCD snubber. buck_like misses it (no "L" on the net) and
            // bridge_like misses it too (the snubber is D+R, so the C/L/P/J
            // power path is absent) — which is how a TPS23754 PoE flyback
            // scored zero switch nodes. So: a power-package switching device
            // AND a wound part that is not that same component, a clamp/
            // snubber leg (D or C), and a deliberately small component count.
            bool flyback_like = false;
            if ((int)comps[net].size() <= p_.sw_flyback_max_components &&
                (pre.count("D") || pre.count("C"))) {
                for (const auto& sw : sw_comps[net]) {
                    if (pad_count[sw] < p_.sw_flyback_min_switch_pads) continue;
                    for (const auto& mag : mag_comps[net])
                        if (mag != sw) { flyback_like = true; break; }
                    if (flyback_like) break;
                }
            }
            if ((buck_like || bridge_like || flyback_like) &&
                !has_shunt_cap(net))
                sw_nets_.insert(net);
        }

        // ---- MONOLITHIC converters: switcher IC + inductor, no discrete
        // FET (ABT #408/#409). Topology test, convention-free:
        //   a wound part AND a >=4-pad active device on a compact net,
        //   no shunt cap to the return (V1 — it would short the switch),
        //   >=2 distinct filtered rails reachable through the wound part, a
        //   2-pad diode, or the device's own pins (V2 — energy conversion
        //   moves charge between different filtered rails; a bias tee or a
        //   ferrite-filtered rail sees only one), and
        //   no wound part whose BOTH ends qualify (V3 — that is a signal
        //   choke; a converter inductor's far side is always shunt-capped).
        // What survives is still AMBIGUOUS: HackRF One's dual-LDO + ferrite
        // harness is externally ISOMORPHIC to a fixed-output buck — feedback
        // resistors, copper width and package size were each tested across
        // the corpus and none separates them. So survivors are reported as
        // CANDIDATES with their evidence, never silently screened: the UI
        // offers them for one-click promotion (switchNodeSource "user").
        std::map<int, SwCandidate> pass1;
        for (auto& [net, pre] : prefixes) {
            if (sw_nets_.count(net) || is_return_net(net)) continue;
            if ((int)comps[net].size() > p_.sw_max_components) continue;
            if ((int)cap_comps[net].size() > p_.sw_max_caps) continue;
            if (has_shunt_cap(net)) continue;                       // V1
            std::vector<std::string> wound, active;
            for (const auto& ref : comps[net]) {
                const std::string rp = ref_prefix(ref);
                if ((rp == "L" && pad_count[ref] >= 2) ||
                    ((rp == "T" || rp == "TR") &&
                     pad_count[ref] >= p_.sw_magnetic_min_pads))
                    wound.push_back(ref);
                else if ((rp == "U" || rp == "IC") && pad_count[ref] >= 4)
                    active.push_back(ref);
            }
            if (wound.empty() || active.empty()) continue;
            std::set<int> rails;                                    // V2
            auto add_rail = [&](int r) {
                if (r != net && !is_return_net(r) && capped_rails.count(r))
                    rails.insert(r);
            };
            for (const auto& w : wound)
                for (int r : comp_all_nets[w]) add_rail(r);
            for (const auto& ref : comps[net])
                if (ref_prefix(ref) == "D" && pad_count[ref] == 2)
                    for (int r : comp_all_nets[ref]) add_rail(r);
            for (const auto& a : active)
                for (int r : comp_all_nets[a]) add_rail(r);
            if (rails.size() < 2) continue;
            pass1[net] = {net, std::move(wound), std::move(active)};
        }
        for (auto& [net, cand] : pass1) {                           // V3
            bool choke = false;
            for (const auto& w : cand.wound)
                for (int r : comp_all_nets[w])
                    if (r != net && pass1.count(r)) choke = true;
            if (!choke) sw_candidates_.push_back(cand);
        }

        // user-declared switch nodes, by exact net name — the UI's promotion
        // path. Unknown name: THROW, never a silent skip.
        for (const auto& name : p_.user_switch_nets) {
            int id = -1;
            for (const auto& n : b_.nets)
                if (n.name == name) { id = n.id; break; }
            if (id < 0)
                throw BoardError("user switch net '" + name +
                                 "' does not exist on this board");
            sw_nets_.insert(id);
            sw_user_.insert(id);
            std::erase_if(sw_candidates_,
                          [&](const SwCandidate& c) { return c.net == id; });
        }
    }

    // ---- rule: commutation loop area (THE converter EMC metric) ----
    // The high-di/dt loop is input-cap(+) -> high-side switch -> switch node ->
    // low-side switch -> input-cap(-). Its ENCLOSED AREA sets radiated
    // emissions and ringing. We find it from connectivity: the FETs on a
    // switch node, and the nearest capacitor bridging one of their other rails
    // to a pour net. Reported as a heuristic with the geometry drawn, so the
    // user can see exactly which loop was measured.
    std::optional<LoopResult> commutation_loop_impl(int sw_net) const {
        // FIRST: the current-switching analysis (Franz §4.4) on the derived
        // netlist. When device roles are inferable and a circulation closes,
        // the mesh is the XOR of the before/after loops — exact branches,
        // not a pattern. When it does not close, the geometric fallback
        // below stands; a guessed mesh would be worse than none.
        if (auto dm = mesh::derive(b_, sw_net, sw_prefix_)) {
            LoopResult r;
            r.members = dm->members;
            r.shape = dm->shape;
            // geometry: hull of the member components' pads
            std::set<std::string> want(dm->members.begin(), dm->members.end());
            std::vector<Point> pts;
            double best_cap = 1e30;
            std::vector<Point> sw_pts;
            for (const auto& p : b_.pads) {
                if (want.count(p.component)) pts.push_back({p.x, p.y});
                if (p.component == dm->sw_ref) sw_pts.push_back({p.x, p.y});
            }
            if (pts.size() < 3) return std::nullopt;
            double cx = 0, cy = 0;
            for (const auto& p : sw_pts) { cx += p.x; cy += p.y; }
            if (!sw_pts.empty()) { cx /= sw_pts.size(); cy /= sw_pts.size(); }
            // the Umlauf-1 chain: its caps' nets feed the domain check, its
            // nearest cap the distance figure
            std::set<int> cn;
            for (const auto& cref : dm->chain)
                for (const auto& p : b_.pads)
                    if (p.component == cref) {
                        if (p.net > 0) cn.insert(p.net);
                        best_cap = std::min(
                            best_cap, std::hypot(p.x - cx, p.y - cy));
                        pts.push_back({p.x, p.y});
                    }
            if (!dm->chain.empty()) r.cap_ref = dm->chain.front();
            r.cap_nets.assign(cn.begin(), cn.end());
            r.cap_dist_mm = best_cap < 1e29 ? best_cap : 0.0;
            r.hull = convex_hull(pts);
            double a = 0;
            for (size_t i = 0, n = r.hull.size(); i < n; ++i) {
                const Point& p = r.hull[i];
                const Point& q = r.hull[(i + 1) % n];
                a += p.x * q.y - q.x * p.y;
            }
            r.area_mm2 = std::abs(a) / 2.0;
            if (r.area_mm2 > 0.5) return r;
            // degenerate hull (members stacked): fall through to geometric
        }
        // Switching devices on the node. A synchronous converter has two FETs;
        // an ASYNCHRONOUS one has a FET and a freewheel diode, and the diode
        // carries half the commutation current — omitting it left the
        // LibreSolar mppt-2420-lc (Q7 + D8) with no measurable loop.
        std::set<std::string> fets;
        for (const auto& p : b_.pads) {
            if (p.net != sw_net) continue;
            std::string pre = ref_prefix(p.component);
            if (pre == sw_prefix_ || pre == "D") fets.insert(p.component);
        }
        if (fets.empty()) return std::nullopt;

        // rails those FETs also touch (excluding the switch node and pours)
        std::set<int> rails;
        std::vector<Point> pts;
        for (const auto& p : b_.pads) {
            if (!fets.count(p.component)) continue;
            pts.push_back({p.x, p.y});
            // is_plane_net, NOT is_pour_net: the input rail is very often a
            // large pour on a power board (/DCDC_HV+ on the MPPT), and that
            // rail is precisely what the loop closes through. Only the return
            // plane itself is excluded here.
            if (p.net > 0 && p.net != sw_net && !is_plane_net(p.net))
                rails.insert(p.net);
        }
        if (pts.empty()) return std::nullopt;
        double cx = 0, cy = 0;
        for (const auto& p : pts) { cx += p.x; cy += p.y; }
        cx /= pts.size(); cy /= pts.size();

        // the input cap: a C bridging one of those rails to a pour, nearest
        // to the FET cluster
        std::map<std::string, std::set<int>> cap_nets;
        std::map<std::string, std::vector<Point>> cap_pts;
        for (const auto& p : b_.pads) {
            if (ref_prefix(p.component) != "C" || p.net <= 0) continue;
            cap_nets[p.component].insert(p.net);
            cap_pts[p.component].push_back({p.x, p.y});
        }
        LoopResult best;
        double best_d = 1e30;
        for (auto& [ref, nets] : cap_nets) {
            bool on_rail = false, on_pour = false;
            for (int n : nets) {
                if (rails.count(n)) on_rail = true;
                if (is_pour_net(n)) on_pour = true;
            }
            if (!on_rail || !on_pour) continue;
            for (const auto& cp : cap_pts[ref]) {
                double d = std::hypot(cp.x - cx, cp.y - cy);
                if (d < best_d) {
                    best_d = d;
                    best.cap_ref = ref;
                    best.cap_nets.assign(nets.begin(), nets.end());
                }
            }
        }
        if (best.cap_ref.empty()) return std::nullopt;

        // loop geometry = convex hull of the FET pads + that cap's pads
        std::vector<Point> all = pts;
        for (const auto& cp : cap_pts[best.cap_ref]) all.push_back(cp);
        best.hull = convex_hull(all);
        double a = 0;
        for (size_t i = 0, n = best.hull.size(); i < n; ++i) {
            const Point& p = best.hull[i];
            const Point& q = best.hull[(i + 1) % n];
            a += p.x * q.y - q.x * p.y;
        }
        best.area_mm2 = std::abs(a) / 2.0;
        best.cap_dist_mm = best_d;
        return best;
    }

    // Loop inductance of the commutation hull — the equivalent-rectangle
    // Grover closed form. The hull is reduced to the rectangle with its
    // perimeter and area (a+b = P/2, ab = A); conductor cross-section enters
    // through the GMD of a w x t strip, r_eq = 0.2235(w+t). A screening
    // estimate: validated against FastHenry on rectangles and real hulls
    // (see tools/validate_loop_l.py) — the band is stated where the number
    // is shown, per house discipline. Enables the ring-frequency check
    // f = 1/(2*pi*sqrt(L*Coss)) when the lab tier runs.
  public:   // reusable, and pinned against the FastHenry reference in tests
    static double hull_loop_inductance_nh(const std::vector<Point>& hull,
                                          double trace_w_mm) {
        if (hull.size() < 3 || !(trace_w_mm > 0)) return 0.0;
        double per = 0, area2 = 0;
        for (size_t i = 0, n = hull.size(); i < n; ++i) {
            const Point& p = hull[i];
            const Point& q = hull[(i + 1) % n];
            per += std::hypot(q.x - p.x, q.y - p.y);
            area2 += p.x * q.y - q.x * p.y;
        }
        const double A = std::abs(area2) / 2.0;
        if (!(per > 0) || !(A > 0)) return 0.0;
        // equivalent rectangle a x b (mm); disc = 0 means a square-ish hull
        const double s = per / 4.0;
        const double disc = std::max(0.0, s * s - A);
        const double a = s + std::sqrt(disc), b = std::max(A / a, 0.01);
        const double t = 0.035;                        // 1 oz copper
        const double r = 0.2235 * (trace_w_mm + t);    // GMD of the strip
        const double d = std::hypot(a, b);
        // Grover, rectangle of round wire radius r (result in nH, dims mm):
        // L = (mu0/pi) * [ a*ln(2ab/(r(a+d))) + b*ln(2ab/(r(b+d)))
        //                  + 2d - 2(a+b) ]  ... mu0/pi = 0.4 nH/mm
        const double L =
            0.4 * (a * std::log(2.0 * a * b / (r * (a + d))) +
                   b * std::log(2.0 * a * b / (r * (b + d))) + 2.0 * d -
                   2.0 * (a + b));
        return std::max(L, 0.0);
    }

  private:
    static std::vector<Point> convex_hull(std::vector<Point> p) {
        if (p.size() < 3) return p;
        std::sort(p.begin(), p.end(), [](const Point& a, const Point& b) {
            return a.x != b.x ? a.x < b.x : a.y < b.y;
        });
        auto cross = [](const Point& o, const Point& a, const Point& b) {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        };
        std::vector<Point> h(2 * p.size());
        size_t k = 0;
        for (size_t i = 0; i < p.size(); ++i) {
            while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
            h[k++] = p[i];
        }
        for (size_t i = p.size() - 1, t = k + 1; i > 0; --i) {
            while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0) --k;
            h[k++] = p[i - 1];
        }
        h.resize(k ? k - 1 : 0);
        return h;
    }

    void find_commutation_loops(std::vector<Finding>& out) {
        // several switch nets can share one loop capacitor (a flyback's
        // primary, SR and bias meshes all close near the same stitch) — the
        // domain-crossing is a property of the CAP, reported once
        std::set<std::string> domain_reported;
        for (int net : sw_nets_) {
            auto loop = commutation_loop(net);
            if (!loop) continue;
            Finding f;
            f.rule = "commutation-loop";
            // 20 mm^2 is tight, 200 mm^2 is poor — the span engineers work in
            f.severity = std::clamp(0.35 + (loop->area_mm2 - 20.0) / 300.0, 0.35, 0.95);
            f.severity_label = f.severity > 0.66 ? "high" : "medium";
            f.confidence = "heuristic";
            f.net_a = net;
            f.coupled_len_mm = loop->area_mm2;
            // loop inductance: conductor width = median routed width of the
            // switch net (the loop's own copper), fallback 1 mm power trace
            double wsum = 0;
            std::vector<double> ws;
            for (const auto& s : b_.segments)
                if (s.net == net) ws.push_back(s.width);
            double w_med = 1.0;
            if (!ws.empty()) {
                std::nth_element(ws.begin(), ws.begin() + ws.size() / 2,
                                 ws.end());
                w_med = ws[ws.size() / 2];
            }
            (void)wsum;
            const double l_nh =
                hull_loop_inductance_nh(loop->hull, w_med);
            f.emit = nlohmann::json{{"areaMm2", loop->area_mm2},
                                    {"loopNh", l_nh},
                                    {"net", b_.net_name(net)}};
            char buf[280];
            std::snprintf(buf, sizeof buf,
                          "Commutation loop around %s encloses about %.0f mm^2; "
                          "the nearest bulk/input capacitor (%s) sits %.1f mm "
                          "from the switching devices.",
                          b_.net_name(net).c_str(), loop->area_mm2,
                          loop->cap_ref.c_str(), loop->cap_dist_mm);
            f.title = "Commutation loop: " + b_.net_name(net) + " (" +
                      std::to_string((int)loop->area_mm2) + " mm^2)";
            if (!loop->members.empty()) {
                // DERIVED by the current-switching analysis: the members ARE
                // the XOR branches, so the finding can name them exactly
                std::string ms;
                for (const auto& m : loop->members)
                    ms += (ms.empty() ? "" : " + ") + m;
                f.detail =
                    "Critical mesh of " + b_.net_name(net) + ", derived by "
                    "current-switching analysis (Franz §4.4): " + ms +
                    " — the branches carried in only one of the two switch "
                    "states, enclosing about " +
                    std::to_string((int)loop->area_mm2) + " mm^2" +
                    (loop->shape == "magnetic-clamp"
                         ? " (winding + clamp shape)"
                         : " (two-device shape)") +
                    ". Loop inductance ~" +
                    std::to_string((int)std::lround(l_nh)) +
                    " nH (equivalent-rectangle estimate, within ~15% of a "
                    "FastHenry solve of the same hull) — with the switch "
                    "output capacitance this sets the ringing frequency. "
                    "This loop "
                    "carries the discontinuous switching current; its "
                    "enclosed area is the dominant radiated-emission and "
                    "ringing mechanism in a converter.";
            } else {
                f.detail =
                    std::string(buf) +
                    " This loop carries the discontinuous switching current; "
                    "its enclosed area is the dominant radiated-emission and "
                    "ringing mechanism in a converter. The loop shown is the "
                    "hull of the switching devices and that capacitor — "
                    "verify it matches your intended commutation path.";
            }
            f.remediation = "Move the input capacitor as close to the switch "
                            "pair as the footprints allow, and place the return "
                            "plane directly beneath the loop so the return "
                            "current cancels the forward path.";
            // draw the hull
            for (size_t i = 0; i < loop->hull.size(); ++i) {
                const Point& a = loop->hull[i];
                const Point& b = loop->hull[(i + 1) % loop->hull.size()];
                f.geom.lines.push_back({net, 0, a.x, a.y, b.x, b.y, 0.25});
            }
            FindingGeom hull_geom = f.geom;   // reuse for the domain finding
            out.push_back(std::move(f));

            // Franz §8.17.1, rule 1: the critical mesh must connect to the
            // general ground at ONE point. When the loop's capacitor bridges
            // TWO pour nets it is a domain STITCH, not a bulk cap — the
            // discontinuous switching current is crossing a ground-domain
            // boundary (e.g. GND<->AGND) through it, and every circuit
            // referenced to either domain sees that di/dt on its reference.
            // Found by running his Stromumschaltanalyse on the derived
            // netlist of a real PoE flyback, where the geometric rule's cap
            // choice (a GND<->AGND stitch) was correct but its MEANING —
            // the mesh spans two domains — was invisible.
            // BOTH sides must be RETURN domains. A power board's input rail
            // is very often a pour too (mppt's /DCDC_HV+), and a cap from
            // the rail pour to ground is the correct loop capacitor, not a
            // domain crossing. Net names are the established discriminator
            // for return nets in this codebase (pdn ground discovery).
            auto is_return_name = [&](int n) {
                std::string lo;
                for (char ch : b_.net_name(n))
                    lo += (char)std::tolower((unsigned char)ch);
                return lo.find("gnd") != std::string::npos ||
                       lo.find("vss") != std::string::npos;
            };
            int pour_a = -1, pour_b = -1;
            for (int n : loop->cap_nets) {
                if (!is_pour_net(n) || !is_return_name(n)) continue;
                if (pour_a < 0) pour_a = n;
                else if (n != pour_a) pour_b = n;
            }
            if (pour_a >= 0 && pour_b >= 0 &&
                domain_reported
                    .insert(loop->cap_ref + "|" + std::to_string(pour_a) +
                            "|" + std::to_string(pour_b))
                    .second) {
                Finding g;
                g.rule = "critical-mesh-ground";
                g.severity = 0.6;
                g.severity_label = "medium";
                g.confidence = "heuristic";
                g.net_a = pour_a;
                g.net_b = pour_b;
                char gb[380];
                std::snprintf(
                    gb, sizeof gb,
                    "The commutation loop of %s closes through %s — a "
                    "capacitor bridging the %s and %s pours. The switching "
                    "current's return is crossing a ground-domain boundary "
                    "through that stitch, so both domains carry the di/dt "
                    "and everything referenced to either sees it. The "
                    "critical mesh should connect to the general ground at "
                    "one point only.",
                    b_.net_name(net).c_str(), loop->cap_ref.c_str(),
                    b_.net_name(pour_a).c_str(), b_.net_name(pour_b).c_str());
                g.title = "Critical mesh crosses " + b_.net_name(pour_a) +
                          "<->" + b_.net_name(pour_b) + " through " +
                          loop->cap_ref;
                g.detail = gb;
                g.remediation =
                    "Keep the whole commutation loop inside ONE ground "
                    "domain: return the switch source and the input "
                    "capacitor to the same pour, joined to the other domain "
                    "at a single star point away from the loop. If the "
                    "stitch must carry the loop, it is part of the hot loop "
                    "and must be treated (placed, sized) as such.";
                g.geom = hull_geom;
                out.push_back(std::move(g));
            }
        }
    }

    void find_switch_nodes(std::vector<Finding>& out) {
        for (int net : sw_nets_) {
            double x1 = 1e30, y1 = 1e30, x2 = -1e30, y2 = -1e30;
            Finding f;
            for (const auto& s : b_.segments)
                if (s.net == net) {
                    x1 = std::min({x1, s.x1, s.x2}); y1 = std::min({y1, s.y1, s.y2});
                    x2 = std::max({x2, s.x1, s.x2}); y2 = std::max({y2, s.y1, s.y2});
                    if (f.geom.lines.size() < 200) f.geom.lines.push_back(s);
                }
            for (const auto& p : b_.pads)
                if (p.net == net) {
                    x1 = std::min(x1, p.x - p.w / 2); y1 = std::min(y1, p.y - p.h / 2);
                    x2 = std::max(x2, p.x + p.w / 2); y2 = std::max(y2, p.y + p.h / 2);
                }
            if (x1 > x2) continue;  // no geometry at all
            double area = (x2 - x1) * (y2 - y1);
            f.rule = "switch-node";
            f.severity = std::clamp(0.25 + area / 500.0, 0.25, 0.6);
            f.severity_label = f.severity > 0.33 ? "medium" : "low";
            // provenance must not lie: a promoted candidate was DECLARED by
            // the user, not identified by connectivity
            const bool user_declared = sw_user_.count(net) > 0;
            f.confidence = user_declared ? "user-declared" : "heuristic";
            f.net_a = net;
            f.coupled_len_mm = area;  // mm^2 — the extent metric for this rule
            char buf[200];
            std::snprintf(buf, sizeof buf,
                          user_declared
                              ? "Net %s is screened as a converter switch node "
                                "(dv/dt aggressor) because YOU declared it one. "
                                "Copper extent %.0f mm^2."
                              : "Net %s joins an inductor pad and a switch/diode "
                                "pad — identified as a converter switch node "
                                "(dv/dt aggressor). Copper extent %.0f mm^2.",
                          b_.net_name(net).c_str(), area);
            f.title = "Switch node: " + b_.net_name(net) + " (extent " +
                      std::to_string((int)area) + " mm^2)";
            f.detail = std::string(buf) +
                       (user_declared
                            ? " Every mm^2 of SW copper "
                            : " Verify the identification; every mm^2 of SW "
                              "copper ") +
                       "radiates dv/dt — coupled runs involving this net are "
                       "severity-boosted in this report.";
            f.remediation = "Minimize SW copper area, keep it away from sense/"
                            "feedback routing, and shield it with ground pour "
                            "on the adjacent layer.";
            out.push_back(std::move(f));
        }
    }

    double ref_height(int cu) const {  // nearest reference height, 0 when none
        const LayerModel& lm = layers_[cu];
        if (lm.ref_up >= 0 && lm.ref_dn >= 0) return std::min(lm.h_up, lm.h_dn);
        if (lm.ref_up >= 0) return lm.h_up;
        if (lm.ref_dn >= 0) return lm.h_dn;
        return 0.0;
    }

    // Nets that act as a RETURN/SUPPLY conductor rather than a victim or
    // aggressor. Two ways to qualify:
    //   * being the dominant pour of a layer classified as a plane, or
    //   * carrying a large pour anywhere, even on a layer that did not reach
    //     the plane threshold. Without the second test, a board with no
    //     classified plane (Fomu, ULX3S) reported "GND <-> SPI_IO3, -12 dB"
    //     as high-severity crosstalk once pour edges entered the engine —
    //     but coupling to the return conductor is not crosstalk.
    // A switch node is exempt: on a converter the SW pour is a genuine
    // aggressor no matter how much copper it occupies.
    // Narrow test: the dominant pour of a layer classified as a plane. Used by
    // build_sw_nets(), which runs BEFORE sw_nets_ exists and so cannot use the
    // full is_pour_net() without a circular dependency (that circularity ate
    // VESC's three phase nodes, whose pours are large). A switch node is never
    // the dominant pour of a plane layer, so this test is safe there.
    bool is_plane_net(int net) const {
        for (const auto& lm : layers_)
            if (lm.is_plane && lm.plane_net == net) return true;
        return false;
    }

    bool is_pour_net(int net) const {
        if (is_plane_net(net)) return true;
        if (big_pour_nets_.count(net) && !sw_nets_.count(net)) return true;
        return false;
    }

    // ---- rule: intra-pair skew on recognized differential pairs ----
    // The pair recognizer already exists for the coupled-run reclassification;
    // this closes the other half: a pair is only a pair if both halves arrive
    // at the same time. Skew is pure geometry — summed routed length per net —
    // so the finding carries no assumption beyond the name match.
    void find_diff_skew(std::vector<Finding>& out) {
        std::map<int, double> len_by_net;
        std::map<int, int> cu_of_net;
        for (const auto& seg : b_.segments) {
            const double l = std::hypot(seg.x2 - seg.x1, seg.y2 - seg.y1);
            len_by_net[seg.net] += l;
            cu_of_net[seg.net] = seg.cu;
        }
        std::set<int> done;
        for (const auto& [na_id, la] : len_by_net) {
            if (done.count(na_id)) continue;
            const std::string& na = b_.net_name(na_id);
            if (na.empty()) continue;
            for (const auto& [nb_id, lb] : len_by_net) {
                if (nb_id <= na_id || done.count(nb_id)) continue;
                const std::string& nb = b_.net_name(nb_id);
                if (!is_differential_pair_name(na, nb)) continue;
                done.insert(na_id);
                done.insert(nb_id);
                if (la < 5.0 || lb < 5.0) break;   // stubs, not a routed pair
                const double skew = std::abs(la - lb);
                if (skew < 0.5) break;             // matched well enough
                Finding f;
                f.rule = "diff-skew";
                f.severity = std::clamp(0.2 + skew / 20.0, 0.2, 0.6);
                f.severity_label = f.severity > 0.4 ? "medium" : "low";
                f.confidence = "exact";
                f.net_a = na_id;
                f.net_b = nb_id;
                f.coupled_len_mm = skew;
                // delay per mm from the layer the pair is routed on, when a
                // reference exists to define one; FR-4 microstrip otherwise
                double eps_eff = 3.0;
                const int cu = cu_of_net[na_id];
                if (cu >= 0 && (size_t)cu < layers_.size()) {
                    const LayerModel& lm = layers_[cu];
                    if (lm.ref_up >= 0)
                        eps_eff = tline::microstrip_eps_eff(0.2, lm.h_up, lm.eps_up);
                    else if (lm.ref_dn >= 0)
                        eps_eff = tline::microstrip_eps_eff(0.2, lm.h_dn, lm.eps_dn);
                }
                const double ps = skew * std::sqrt(eps_eff) / 0.2998;
                char buf[240];
                std::snprintf(buf, sizeof buf,
                              "Intra-pair skew %.1f mm (%s %.1f mm, %s %.1f mm) "
                              "— about %.0f ps at eps_eff %.1f.",
                              skew, na.c_str(), la, nb.c_str(), lb, ps, eps_eff);
                f.title = na + " / " + nb + " skew " +
                          std::to_string((int)std::lround(skew)) + " mm";
                f.detail = std::string(buf) +
                           " Skew converts differential signal into common mode "
                           "at every edge, and the common-mode current is what "
                           "reaches the cable. Both lengths are summed routed "
                           "copper, a geometric fact.";
                f.remediation = "Length-match the pair with serpentines AT the "
                                "mismatch point, not at the far end; keep the "
                                "correction on the same layer as the mismatch.";
                out.push_back(std::move(f));
                break;
            }
        }
    }

    // ---- rule: layers carrying signals with no reference plane ----
    void find_no_reference_plane(std::vector<Finding>& out) {
        const std::vector<double>& len_by_cu = routed_mm_;
        for (size_t i = 0; i < layers_.size(); ++i) {
            if (layers_[i].is_plane || len_by_cu[i] <= 0) continue;
            if (layers_[i].ref_up >= 0 || layers_[i].ref_dn >= 0) continue;
            Finding f;
            f.rule = "no-reference-plane";
            f.severity = 0.9;
            f.severity_label = "high";
            f.confidence = "exact";
            f.cu_a = (int)i;
            f.title = "No reference plane for signals on " + b_.copper_names[i];
            f.detail = "Layer " + b_.copper_names[i] + " carries " +
                       std::to_string((int)len_by_cu[i]) +
                       " mm of routed traces but no copper layer qualifies as a "
                       "reference plane (pour coverage >= 50% or power-typed). "
                       "Return currents have no defined path; crosstalk and "
                       "emission estimates on this layer are geometric-only.";
            f.remediation = "Add a ground pour / dedicate a plane layer, and stitch "
                            "it; re-run to get quantified coupling estimates.";
            out.push_back(std::move(f));
        }
    }

    // ---- rule: coupled parallel runs (edge + broadside) + 3W ----
    struct PairKey {
        int net_lo, net_hi, cu_a, cu_b;  // cu_a <= cu_b
        bool operator<(const PairKey& o) const {
            return std::tie(net_lo, net_hi, cu_a, cu_b) <
                   std::tie(o.net_lo, o.net_hi, o.cu_a, o.cu_b);
        }
    };
    struct PairAccum {
        double len = 0, len_x_d = 0;      // for length-weighted mean separation
        double min_edge_sep = 1e30;
        double max_w = 0;
        double w_a = 0, w_b = 0;      // widths at the closest approach
        double worst_k = 0;
        bool have_h = false;
        bool involves_pour = false;   // one side is a copper-pour boundary
        FindingGeom geom;
    };

    void find_coupled_runs(std::vector<Finding>& out) {
        using detail::SegRef;
        size_t n_cu = layers_.size();
        double cos_tol = std::cos(p_.angle_tol_deg * M_PI / 180.0);

        // screening radius per layer: factor × reference height (floored)
        std::vector<double> radius(n_cu, p_.min_radius_mm);
        for (size_t i = 0; i < n_cu; ++i) {
            double h = ref_height((int)i);
            radius[i] = std::max(p_.min_radius_mm, p_.radius_factor * (h > 0 ? h : 1.0));
        }
        double max_radius = *std::max_element(radius.begin(), radius.end());

        // per-layer seg refs + grids
        std::vector<std::vector<SegRef>> refs(n_cu);
        std::vector<detail::Grid> grids;
        grids.reserve(n_cu);
        for (size_t i = 0; i < n_cu; ++i) grids.emplace_back(std::max(1.0, max_radius));
        auto add_ref = [&](int net, int cu, double x1, double y1, double x2,
                           double y2, double w, bool pour_edge) {
            double dx = x2 - x1, dy = y2 - y1;
            double len = std::hypot(dx, dy);
            // A parallel overlap can never exceed min(len_a, len_b), so an edge
            // shorter than min_run_mm cannot contribute to ANY qualifying run.
            // Dropping it is exact, not an approximation — and it matters:
            // pour outlines are mostly tiny arc-approximation edges, which took
            // ulx3s from 0.33 s to 2.4 s before this filter.
            if (len < p_.min_run_mm) return;
            refs[cu].push_back({net, cu, x1, y1, x2, y2, w, len,
                                dx / len, dy / len, pour_edge, refs[cu].size()});
        };
        for (const Segment& s : b_.segments) {
            if (s.net <= 0 || is_pour_net(s.net)) continue;  // pour nets only
            add_ref(s.net, s.cu, s.x1, s.y1, s.x2, s.y2, s.width, false);
        }
        // A copper pour that is NOT the reference plane is a signal/power
        // conductor, and its BOUNDARY is what couples to a nearby track. On
        // converter boards the high-current paths ARE pours, so leaving them
        // out made exactly the interesting nets invisible. Zero nominal width:
        // the outline already sits at the conductor edge.
        for (const auto& z : b_.zones) {
            if (z.net <= 0 || is_pour_net(z.net)) continue;
            for (size_t i = 0, n = z.pts.size(); i < n; ++i) {
                const Point& a = z.pts[i];
                const Point& c = z.pts[(i + 1) % n];
                add_ref(z.net, z.cu, a.x, a.y, c.x, c.y, 0.0, true);
            }
        }
        for (size_t cu = 0; cu < n_cu; ++cu)
            for (size_t k = 0; k < refs[cu].size(); ++k) grids[cu].insert(refs[cu][k], k);

        std::map<PairKey, PairAccum> pairs;

        // A pour's outline has many edges, and a nearby track is usually
        // parallel to SEVERAL of them (both sides of a rectangle, for
        // instance). Summing all of them counts the same physical coupling
        // repeatedly — and the far edge is shadowed by the pour's own copper
        // anyway. So per (victim segment, pour net) keep only the CLOSEST
        // edge, and accumulate that once.
        struct PourHit {
            double center_d = 1e30;
            detail::Overlap ov;
            int pour_net = 0, victim_net = 0, cu_v = 0, cu_p = 0;
            double w_v = 0;
            bool broadside = false;
        };
        std::map<std::tuple<int, size_t, int>, PourHit> pour_best;

        auto accumulate = [&](int net_a, int net_b, int cu_a, int cu_b,
                              double w_a, double w_b, const detail::Overlap& ov,
                              double k, bool have_h, bool involves_pour) {
            PairKey key{std::min(net_a, net_b), std::max(net_a, net_b),
                        std::min(cu_a, cu_b), std::max(cu_a, cu_b)};
            PairAccum& acc = pairs[key];
            acc.len += ov.length;
            acc.len_x_d += ov.length * ov.center_d;
            const double sep = ov.center_d - 0.5 * (w_a + w_b);
            if (sep < acc.min_edge_sep) { acc.w_a = w_a; acc.w_b = w_b; }
            acc.min_edge_sep = std::min(acc.min_edge_sep, sep);
            acc.max_w = std::max({acc.max_w, w_a, w_b});
            acc.worst_k = std::max(acc.worst_k, k);
            acc.have_h = acc.have_h || have_h;
            acc.involves_pour = acc.involves_pour || involves_pour;
            Segment ga = ov.span_a; ga.net = net_a; ga.cu = cu_a;
            Segment gb = ov.span_b; gb.net = net_b; gb.cu = cu_b;
            acc.geom.lines.push_back(ga);
            acc.geom.lines.push_back(gb);
        };

        auto consider = [&](const SegRef& a, int cu_a, const SegRef& sb, int cu_b,
                            bool broadside) {
            if (a.net == sb.net) return;
            // two pour outlines meeting is a clearance question, not coupling
            if (a.pour_edge && sb.pour_edge) return;
            auto ov = detail::parallel_overlap(a, sb, p_.min_run_mm, cos_tol);
            if (!ov) return;
            double h = 0.0;
            bool have_h = false;
            double k = 0.0;
            if (!broadside) {
                if (ov->center_d > radius[cu_a]) return;
                h = ref_height(cu_a);
                if (h > 0) { have_h = true; k = tline::next_sat_edge(ov->center_d, h); }
            } else {
                // vertical dielectric between the two signal layers
                double eps;
                b_.stackup.dielectric_between(std::min(cu_a, cu_b), std::max(cu_a, cu_b), h, eps);
                if (ov->center_d > p_.radius_factor * h) return;
                have_h = true;
                k = tline::next_sat_broadside(ov->center_d, h);
            }
            if (a.pour_edge || sb.pour_edge) {
                const SegRef& victim = a.pour_edge ? sb : a;
                const SegRef& pour = a.pour_edge ? a : sb;
                auto key = std::make_tuple(victim.cu, victim.id, pour.net);
                PourHit& h = pour_best[key];
                if (ov->center_d >= h.center_d) return;   // a closer edge won
                h.center_d = ov->center_d;
                h.ov = a.pour_edge ? detail::Overlap{ov->length, ov->center_d,
                                                     ov->span_b, ov->span_a}
                                   : *ov;
                h.victim_net = victim.net;
                h.pour_net = pour.net;
                h.cu_v = victim.cu;
                h.cu_p = pour.cu;
                h.w_v = victim.w;
                h.broadside = broadside;
                return;
            }
            accumulate(a.net, sb.net, cu_a, cu_b, a.w, sb.w, *ov, k, have_h,
                       false);
        };

        for (size_t cu = 0; cu < n_cu; ++cu) {
            for (size_t ia = 0; ia < refs[cu].size(); ++ia) {
                const SegRef& a = refs[cu][ia];
                // same layer (edge coupling)
                grids[cu].query(a, radius[cu], [&](size_t ib) {
                    if (ib <= ia) return;  // each unordered pair once
                    consider(a, (int)cu, refs[cu][ib], (int)cu, false);
                });
                // adjacent signal layer above only (below handled when that
                // layer is 'cu' in the outer loop) — broadside pairs once
                if (cu + 1 < n_cu && !layers_[cu + 1].is_plane) {
                    grids[cu + 1].query(a, max_radius, [&](size_t ib) {
                        consider(a, (int)cu, refs[cu + 1][ib], (int)(cu + 1), true);
                    });
                }
            }
        }

        // fold the deduplicated pour hits in, recomputing k at the kept distance
        for (auto& [key, h] : pour_best) {
            if (h.center_d > 1e29) continue;
            double k = 0.0;
            bool have_h = false;
            if (!h.broadside) {
                double hh = ref_height(h.cu_v);
                if (hh > 0) { have_h = true; k = tline::next_sat_edge(h.center_d, hh); }
            } else {
                double hh, eps;
                b_.stackup.dielectric_between(std::min(h.cu_v, h.cu_p),
                                              std::max(h.cu_v, h.cu_p), hh, eps);
                have_h = true;
                k = tline::next_sat_broadside(h.center_d, hh);
            }
            accumulate(h.victim_net, h.pour_net, h.cu_v, h.cu_p, h.w_v, 0.0,
                       h.ov, k, have_h, true);
        }

        dropped_below_floor_ = 0;
        for (auto& [key, acc] : pairs) {
            double mean_d = acc.len_x_d / acc.len;
            bool broadside = key.cu_a != key.cu_b;
            Finding f;
            f.rule = "coupled-run";
            f.net_a = key.net_lo;
            f.net_b = key.net_hi;
            f.cu_a = key.cu_a;
            f.cu_b = key.cu_b;
            f.coupled_len_mm = acc.len;
            f.min_sep_mm = acc.min_edge_sep;
            f.geom = std::move(acc.geom);
            f.solve = cross_section_for(key.cu_a, key.cu_b, acc.w_a, acc.w_b,
                                        acc.min_edge_sep, acc.len);
            std::string kind = acc.involves_pour ? "pour-edge"
                             : broadside ? "broadside" : "edge";
            std::string where = broadside
                ? b_.copper_names[key.cu_a] + "/" + b_.copper_names[key.cu_b]
                : b_.copper_names[key.cu_a];
            char buf[160];
            if (acc.have_h) {
                double db = tline::to_db(acc.worst_k);
                if (db < p_.report_floor_db) { ++dropped_below_floor_; continue; }
                f.next_db = db;
                f.confidence = "screening-estimate";
                // severity: −40 dB → 0, −10 dB → 1
                f.severity = std::clamp((db + 40.0) / 30.0, 0.0, 1.0);
                std::snprintf(buf, sizeof buf,
                              "NEXT (saturated) ~ %.1f dB over %.1f mm %s-coupled run",
                              db, acc.len, kind.c_str());
            } else {
                f.confidence = "geometric-only";
                f.severity = std::clamp(acc.len / 100.0, 0.0, 0.5);
                std::snprintf(buf, sizeof buf,
                              "%.1f mm %s-coupled run (no reference plane — "
                              "geometric ranking only)",
                              acc.len, kind.c_str());
            }
            const std::string& na = b_.net_name(f.net_a);
            const std::string& nb = b_.net_name(f.net_b);
            f.title = na + " <-> " + nb + " on " + where;
            f.detail = std::string(buf) +
                       ". Length-saturated near-end coupling. Measured against a "
                       "2D field solve this closed form runs about 6.5 dB "
                       "OPTIMISTIC and fairly uniformly so, which means the "
                       "ranking is reliable but the absolute figure is not — "
                       "real coupling here is likelier near " +
                       std::to_string((int)std::lround(*f.next_db + 6.5)) +
                       " dB. Confirm with the field-solver tier. Mean centre "
                       "separation " + std::to_string(mean_d).substr(0, 5) + " mm.";
            f.remediation = broadside
                ? "Offset the runs laterally, route orthogonally on adjacent "
                  "layers, or move one net to a layer across a plane."
                : "Increase the gap (3W rule), shorten the parallel run, or "
                  "drop a grounded guard trace with stitching vias.";

            // differential pairs couple by DESIGN — reclassify to info, never
            // a defect (USB DP/DM on HackRF One taught this)
            bool is_diff = is_differential_pair_name(na, nb);
            if (is_diff) {
                ++diff_pairs_recognized_;
                f.rule = "diff-pair";
                f.severity = 0.05;
                f.confidence = "exact";
                f.title += " (differential pair)";
                f.detail = "Recognized as a differential pair by name — this "
                           "coupling is intentional. " + f.detail;
                f.remediation = "Nothing to fix; keep gap and lengths symmetric "
                                "along the whole run so the coupling stays "
                                "common-mode balanced.";
            } else if (sw_nets_.count(f.net_a) || sw_nets_.count(f.net_b)) {
                // a switch node is the board's dv/dt aggressor: same geometry
                // couples harder in practice than the quasi-static estimate
                f.severity = std::min(1.0, f.severity + 0.15);
                f.title += " [SW aggressor]";
                f.detail += " One net is an identified switch node (dv/dt "
                            "aggressor) — severity boosted.";
            }
            f.severity_label = f.severity > 0.66 ? "high"
                              : f.severity > 0.33 ? "medium"
                              : f.severity > 0.1  ? "low" : "info";
            if (acc.involves_pour)
                f.detail += " One side is a copper-pour boundary, so the "
                            "coupling is to the edge of that pour.";
            // 3W companion finding when violated (not for intentional pairs,
            // and not against a pour edge — "3x trace width" is meaningless
            // when one conductor has no width)
            if (acc.min_edge_sep < 2.0 * acc.max_w && !broadside && !is_diff &&
                !acc.involves_pour) {
                // companion finding: ranks just BELOW its coupled-run (a dense
                // board violates 3W everywhere — the quantified coupling must
                // stay on top of the ranking, learned on HackRF One)
                Finding w3;
                w3.rule = "3w";
                w3.severity = std::max(0.0, f.severity - 0.05);
                w3.severity_label = w3.severity > 0.66 ? "high"
                                   : w3.severity > 0.33 ? "medium" : "low";
                w3.confidence = "exact";
                w3.net_a = f.net_a; w3.net_b = f.net_b;
                w3.cu_a = key.cu_a; w3.cu_b = key.cu_b;
                w3.coupled_len_mm = acc.len;
                w3.min_sep_mm = acc.min_edge_sep;
                w3.title = "3W violation: " + b_.net_name(f.net_a) + " <-> " +
                           b_.net_name(f.net_b) + " on " + where;
                char b3[128];
                std::snprintf(b3, sizeof b3,
                              "Minimum edge separation %.3f mm < 2x trace width "
                              "(%.3f mm) over a %.1f mm run.",
                              acc.min_edge_sep, 2.0 * acc.max_w, acc.len);
                w3.detail = b3;
                w3.remediation = "Keep centre spacing >= 3x trace width for "
                                 "sensitive/aggressor nets (Johnson & Graham).";
                w3.geom = f.geom;  // same overlay
                out.push_back(std::move(w3));
            }
            out.push_back(std::move(f));
        }
    }

    // ---- rule: via stubs ----
    // A via spanning more layers than the net actually uses leaves an unused
    // barrel: an open stub that resonates at lambda/4 and dumps energy there.
    // "Used" layers are those where the same net has copper at the via site.
    void find_via_stubs(std::vector<Finding>& out) {
        auto cu_z = b_.stackup.copper_z();
        struct Agg {
            int count = 0;
            double min_stub = 1e30, max_stub = 0, worst_stub = 0;
            int worst_net = -1;
            std::vector<Point> pts;
        };
        std::map<std::pair<int, int>, Agg> agg;
        for (const auto& v : b_.vias) {
            if (v.net <= 0 || is_pour_net(v.net)) continue;
            if (v.cu_to <= v.cu_from) continue;
            double r = v.size * 0.5 + 0.05;  // touch tolerance
            int lo = 1 << 30, hi = -1;
            for (const auto& s : b_.segments) {
                if (s.net != v.net || s.cu < v.cu_from || s.cu > v.cu_to) continue;
                if (std::min(std::hypot(s.x1 - v.x, s.y1 - v.y),
                             std::hypot(s.x2 - v.x, s.y2 - v.y)) > r) continue;
                lo = std::min(lo, s.cu); hi = std::max(hi, s.cu);
            }
            for (const auto& p : b_.pads) {
                if (p.net != v.net) continue;
                if (std::hypot(p.x - v.x, p.y - v.y) > r) continue;
                int pc = p.through_hole ? v.cu_from : p.cu;
                if (pc < v.cu_from || pc > v.cu_to) continue;
                lo = std::min(lo, pc); hi = std::max(hi, pc);
                if (p.through_hole) hi = std::max(hi, v.cu_to);
            }
            if (hi < 0 || lo > hi) continue;                 // nothing lands here
            double stub = 0.0;
            for (int c = v.cu_from; c < lo; ++c) stub += cu_z[c + 1] - cu_z[c];
            for (int c = hi; c < v.cu_to; ++c) stub += cu_z[c + 1] - cu_z[c];
            if (stub < p_.min_via_stub_mm) continue;
            Agg& a = agg[{v.cu_from, v.cu_to}];
            ++a.count;
            a.min_stub = std::min(a.min_stub, stub);
            a.max_stub = std::max(a.max_stub, stub);
            if (stub > a.worst_stub) { a.worst_stub = stub; a.worst_net = v.net; }
            if (a.pts.size() < 200) a.pts.push_back({v.x, v.y});
        }

        // One finding per via SPAN, not per via. Every through-via used for a
        // shallow layer change on a 4-layer board leaves the same stub — 45
        // identical rows (seen on OrangeCrab/VESC/Glasgow) is noise, one line
        // naming the count and the resonance is a review comment.
        for (auto& [span, a] : agg) {
            auto [from, to] = span;
            double h, eps;
            b_.stackup.dielectric_between(from, to, h, eps);
            double f_lo = tline::quarter_wave_hz(a.max_stub, tline::via_eps_eff(eps)) / 1e9;
            double f_hi = tline::quarter_wave_hz(a.min_stub, tline::via_eps_eff(eps)) / 1e9;
            Finding f;
            f.rule = "via-stub";
            // geometry only: the fraction of the barrel left unused. Whether
            // that matters depends on the design's edge rate, which we do not
            // know — so the frequency is REPORTED and the user judges.
            double board_h = b_.stackup.copper_z().back();
            double frac = board_h > 0 ? a.max_stub / board_h : 0.0;
            f.severity = std::clamp(0.15 + 0.35 * frac, 0.15, 0.5);
            f.severity_label = f.severity > 0.33 ? "medium" : "low";
            f.confidence = "exact";
            f.net_a = a.worst_net;
            f.cu_a = from;
            f.cu_b = to;
            f.coupled_len_mm = a.max_stub;
            char buf[320];
            std::snprintf(buf, sizeof buf,
                          "%d via(s) spanning %s..%s leave %.2f-%.2f mm of unused "
                          "barrel because the net terminates earlier. Those open "
                          "stubs are quarter-wave resonators between %.1f and "
                          "%.1f GHz — a concern only if your edge rates reach "
                          "that far. Worst: %s.",
                          a.count, b_.copper_names[from].c_str(),
                          b_.copper_names[to].c_str(), a.min_stub, a.max_stub,
                          f_lo, f_hi, b_.net_name(a.worst_net).c_str());
            f.title = "Via stubs on " + b_.copper_names[from] + ".." +
                      b_.copper_names[to] + ": " + std::to_string(a.count) +
                      " via(s), " + std::to_string((int)std::lround(f_lo)) + " GHz";
            f.detail = buf;
            f.remediation = "If those frequencies matter for this design, use "
                            "blind/buried vias or back-drill; otherwise record "
                            "the decision and move on.";
            for (const auto& pt : a.pts) f.geom.markers.push_back(pt);
            out.push_back(std::move(f));
        }
    }

    // ---- rule: dangling trace ends (open stubs / accidental antennas) ----
    void find_dangling_stubs(std::vector<Finding>& out) {
        // endpoint -> how many pieces of copper of the same net meet there
        struct Key { int net, cu; long long x, y; };
        auto key = [](int net, int cu, double x, double y) {
            return std::make_tuple(net, cu, (long long)std::llround(x * 100),
                                   (long long)std::llround(y * 100));
        };
        std::map<std::tuple<int, int, long long, long long>, int> touch;
        for (const auto& s : b_.segments) {
            if (s.net <= 0 || is_pour_net(s.net)) continue;
            ++touch[key(s.net, s.cu, s.x1, s.y1)];
            ++touch[key(s.net, s.cu, s.x2, s.y2)];
        }
        for (const auto& s : b_.segments) {
            if (s.net <= 0 || is_pour_net(s.net)) continue;
            double len = std::hypot(s.x2 - s.x1, s.y2 - s.y1);
            if (len < p_.min_dangling_mm) continue;
            for (int end = 0; end < 2; ++end) {
                double ex = end ? s.x2 : s.x1, ey = end ? s.y2 : s.y1;
                if (touch[key(s.net, s.cu, ex, ey)] > 1) continue;  // continues
                bool anchored = false;
                for (const auto& p : b_.pads)
                    if (p.net == s.net &&
                        std::hypot(p.x - ex, p.y - ey) <= std::max(p.w, p.h)) {
                        anchored = true; break;
                    }
                if (!anchored)
                    for (const auto& v : b_.vias)
                        if (v.net == s.net && s.cu >= v.cu_from && s.cu <= v.cu_to &&
                            std::hypot(v.x - ex, v.y - ey) <= v.size) {
                            anchored = true; break;
                        }
                // T-junction: the endpoint lands on the BODY of another
                // same-net track. Endpoint counting cannot see it, and
                // Gerber-derived boards (and generated ones — caught by
                // Faraday reviewing Hertz's first emitted filter) join
                // mid-segment routinely; KiCad's router splits tracks at
                // junctions, which is why the corpus never showed it.
                if (!anchored)
                    for (const auto& t : b_.segments) {
                        if (t.net != s.net || t.cu != s.cu) continue;
                        if (&t == &s) continue;
                        const double tdx = t.x2 - t.x1, tdy = t.y2 - t.y1;
                        const double L2 = tdx * tdx + tdy * tdy;
                        double u = L2 > 0
                                       ? ((ex - t.x1) * tdx + (ey - t.y1) * tdy) / L2
                                       : 0.0;
                        u = std::clamp(u, 0.0, 1.0);
                        // interior contact only — endpoint-to-endpoint is
                        // already what `touch` counts
                        if (u <= 0.02 || u >= 0.98) continue;
                        if (std::hypot(ex - (t.x1 + u * tdx),
                                       ey - (t.y1 + u * tdy)) <=
                            t.width / 2.0 + 0.01) {
                            anchored = true;
                            break;
                        }
                    }
                if (anchored) continue;
                double h = ref_height(s.cu);
                Finding f;
                f.rule = "dangling-stub";
                f.severity = std::clamp(0.25 + len / 60.0, 0.25, 0.8);
                f.severity_label = f.severity > 0.66 ? "high"
                                  : f.severity > 0.33 ? "medium" : "low";
                f.confidence = "exact";
                f.net_a = s.net;
                f.cu_a = s.cu;
                f.coupled_len_mm = len;
                char buf[240];
                if (h > 0) {
                    const LayerModel& lm = layers_[s.cu];
                    double eps = lm.ref_up >= 0 ? lm.eps_up : lm.eps_dn;
                    double w = s.width;
                    double ee = tline::microstrip_eps_eff(w, h, eps);
                    double f_mhz = tline::quarter_wave_hz(len, ee) / 1e6;
                    std::snprintf(buf, sizeof buf,
                                  "A %.1f mm length of %s on %s ends without "
                                  "reaching a pad, via or another track. An open "
                                  "stub radiates and loads its driver; this one "
                                  "is a quarter-wave resonator near %.0f MHz.",
                                  len, b_.net_name(s.net).c_str(),
                                  b_.copper_names[s.cu].c_str(), f_mhz);
                    f.title = "Open stub: " + b_.net_name(s.net) + " (" +
                              std::to_string((int)std::lround(f_mhz)) + " MHz)";
                } else {
                    std::snprintf(buf, sizeof buf,
                                  "A %.1f mm length of %s on %s ends without "
                                  "reaching a pad, via or another track.",
                                  len, b_.net_name(s.net).c_str(),
                                  b_.copper_names[s.cu].c_str());
                    f.title = "Open stub: " + b_.net_name(s.net);
                }
                f.detail = buf;
                f.remediation = "Delete the leftover track, or terminate it "
                                "where it was meant to connect.";
                f.geom.lines.push_back(s);
                f.geom.markers.push_back({ex, ey});
                out.push_back(std::move(f));
            }
        }
    }

    // ---- rule: decoupling capacitor placement (loop inductance) ----
    // A decoupling cap bridges a rail to a pour. What matters is the LOOP its
    // current takes to the pin it serves: cap pad -> rail -> IC pad -> device
    // -> pour -> back. Distance is the proxy the layout controls.
    void find_decoupling(std::vector<Finding>& out) {
        struct CapInfo { std::vector<Point> pts; std::set<int> nets; };
        std::map<std::string, CapInfo> caps;
        for (const auto& p : b_.pads) {
            if (ref_prefix(p.component) != "C" || p.net <= 0) continue;
            caps[p.component].pts.push_back({p.x, p.y});
            caps[p.component].nets.insert(p.net);
        }
        for (auto& [ref, ci] : caps) {
            int rail = -1;
            bool on_pour = false;
            for (int n : ci.nets) {
                if (is_pour_net(n)) on_pour = true;
                else rail = n;
            }
            if (!on_pour || rail < 0) continue;   // not a decoupling cap
            // nearest IC pad on that rail
            double best = 1e30;
            std::string ic;
            Point ic_pt{0, 0}, cap_pt{0, 0};
            for (const auto& p : b_.pads) {
                // "U" (ANSI) or "IC" (IEC) — unlike Q/T these never collide
                // with another part class, so both are accepted on any board.
                // Only "U" was, which left every IEC board's decoupling rule
                // with no load to measure the distance TO.
                const std::string pre = ref_prefix(p.component);
                if (p.net != rail || (pre != "U" && pre != "IC")) continue;
                for (const auto& cp : ci.pts) {
                    double d = std::hypot(p.x - cp.x, p.y - cp.y);
                    if (d < best) { best = d; ic = p.component; ic_pt = {p.x, p.y};
                                    cap_pt = cp; }
                }
            }
            if (ic.empty() || best > p_.decoupling_far_mm * 4) continue;
            if (best <= p_.decoupling_far_mm) continue;   // well placed
            // Franz §5.9.5 (leiterplattenbezogene Abblockung): when the RAIL
            // itself is a continuous plane, capacitor position has little
            // effect on the decoupling impedance — his measured Bild 5.47
            // shows a cap in the far corner still working. The distance is
            // then context, not a defect. Trace-fed rails keep the full
            // severity: there, Einzelabblockung is mandatory.
            bool rail_is_plane = false;
            for (const auto& lm : layers_)
                rail_is_plane = rail_is_plane ||
                                (lm.is_plane && lm.plane_net == rail);
            Finding f;
            f.rule = "decoupling-distance";
            f.severity = std::clamp(0.2 + (best - p_.decoupling_far_mm) / 25.0,
                                    0.2, 0.7);
            f.severity_label = f.severity > 0.33 ? "medium" : "low";
            if (rail_is_plane) { f.severity = 0.12; f.severity_label = "info"; }
            f.confidence = "heuristic";
            f.net_a = rail;
            f.coupled_len_mm = best;
            char buf[240];
            std::snprintf(buf, sizeof buf,
                          "Decoupling capacitor %s on %s sits %.1f mm from the "
                          "nearest %s pin on that rail. The supply loop that far "
                          "out is dominated by track and via inductance, not by "
                          "the capacitor, so the part stops decoupling well "
                          "below its self-resonance.",
                          ref.c_str(), b_.net_name(rail).c_str(), best, ic.c_str());
            f.title = "Decoupling reach: " + ref + " -> " + ic + " (" +
                      std::to_string((int)std::lround(best)) + " mm)";
            f.detail = buf;
            if (rail_is_plane)
                f.detail += std::string(
                    " However: this rail is a continuous plane, and on a "
                    "plane pair the capacitor's position has little effect "
                    "on the decoupling impedance (Franz, EMV 5th ed., "
                    "§5.9.5 — board-referred decoupling). Reported as "
                    "context, not a defect.");
            f.remediation = "Move " + ref + " next to the " + ic +
                            " pin (ideally on the same side, with its own via "
                            "pair into the plane).";
            f.geom.lines.push_back({rail, 0, cap_pt.x, cap_pt.y, ic_pt.x, ic_pt.y, 0.2});
            out.push_back(std::move(f));
        }
    }

    // ---- the input filter, as a BLOCK (ABT #795) ----
    // Until now nothing in the screener knew that a filter IS a filter. Type
    // and values are Hertz's job; PLACEMENT is a layout question and therefore
    // this one. Three things decide whether a filter delivers the attenuation
    // it was designed for, and all three are geometry:
    //
    //   1. copper on the dirty side running beside copper on the clean side —
    //      a path AROUND the filter, which caps its insertion loss at whatever
    //      that coupling is, no matter what the parts do;
    //   2. the Y capacitors' return: above the resonance of the cap with its
    //      own mounting inductance it stops being a capacitor, and that
    //      frequency is set by the copper, not by the part;
    //   3. switching copper sitting next to the clean side, which re-injects
    //      after the filter through the near field.
    //
    // The block itself is found by SHAPE, not by refdes: a four-pad wound part
    // whose pads split two-and-two into four distinct non-return nets, with at
    // least one X or Y capacitor on those nets. The capacitor requirement is
    // what separates a common-mode choke from an ordinary transformer.
    struct FilterBlock {
        std::string choke_ref;
        std::set<int> side_a, side_b;      // the two winding sides
        std::set<int> in_nets, out_nets;   // in = the connector/outside side
        std::vector<std::string> x_caps, y_caps;
        std::vector<int> y_nets;
        double cx = 0, cy = 0, radius_mm = 0;
        bool side_known = false;           // was a connector actually found?
    };

    std::vector<FilterBlock> find_filter_blocks() const {
        std::map<std::string, std::vector<const Pad*>> by_comp;
        for (const auto& p : b_.pads) by_comp[p.component].push_back(&p);

        std::vector<FilterBlock> blocks;
        for (const auto& [ref, pads] : by_comp) {
            if (pads.size() != 4) continue;
            std::set<int> nets;
            bool bad = false;
            for (const Pad* p : pads) {
                if (p->net <= 0 || is_pour_net(p->net)) { bad = true; break; }
                nets.insert(p->net);
            }
            if (bad || nets.size() != 4) continue;

            // split the four pads two-and-two along the part's long axis
            double cx = 0, cy = 0;
            for (const Pad* p : pads) { cx += p->x; cy += p->y; }
            cx /= 4; cy /= 4;
            double sxx = 0, syy = 0;
            for (const Pad* p : pads) {
                sxx += (p->x - cx) * (p->x - cx);
                syy += (p->y - cy) * (p->y - cy);
            }
            const bool along_x = sxx >= syy;
            std::vector<const Pad*> sorted(pads.begin(), pads.end());
            std::sort(sorted.begin(), sorted.end(),
                      [&](const Pad* a, const Pad* b) {
                          return along_x ? a->x < b->x : a->y < b->y;
                      });
            FilterBlock fb;
            fb.choke_ref = ref;
            fb.cx = cx; fb.cy = cy;
            for (int i = 0; i < 2; ++i) fb.side_a.insert(sorted[i]->net);
            for (int i = 2; i < 4; ++i) fb.side_b.insert(sorted[i]->net);
            if (fb.side_a.size() != 2 || fb.side_b.size() != 2) continue;
            for (const Pad* p : pads)
                fb.radius_mm = std::max(fb.radius_mm,
                                        std::hypot(p->x - cx, p->y - cy) +
                                            0.5 * std::hypot(p->w, p->h));

            // X and Y capacitors on the block's own nets. The refdes of a
            // safety capacitor is very often CX1/CY2 rather than C1 — that IS
            // the convention on line filters — so any C-initial prefix counts,
            // minus the connector families that also start with a C.
            for (const auto& [cref, cpads] : by_comp) {
                const std::string cpre = ref_prefix(cref);
                if (cpre.empty() || cpre[0] != 'C' || cpre == "CN" ||
                    cpre == "CON" || cpads.size() != 2)
                    continue;
                const int n1 = cpads[0]->net, n2 = cpads[1]->net;
                if (n1 <= 0 || n2 <= 0) continue;
                auto in_side = [&](const std::set<int>& s) {
                    return s.count(n1) && s.count(n2);
                };
                if (in_side(fb.side_a) || in_side(fb.side_b)) {
                    fb.x_caps.push_back(cref);
                    continue;
                }
                const bool p1 = is_pour_net(n1), p2 = is_pour_net(n2);
                const int line = p1 ? n2 : n1;
                if (p1 == p2) continue;                       // both or neither
                if (!fb.side_a.count(line) && !fb.side_b.count(line)) continue;
                fb.y_caps.push_back(cref);
                fb.y_nets.push_back(line);
            }
            // No X and no Y capacitor: this is a transformer or a coupled
            // inductor, not a line filter. Refusing here is what keeps every
            // flyback on the corpus out of this rule family.
            if (fb.x_caps.empty() && fb.y_caps.empty()) continue;

            // Which side faces the outside world: a connector pad decides it;
            // otherwise the side whose copper sits nearer the board outline.
            const auto cpads = connector_pads();
            bool a_conn = false, b_conn = false;
            for (const Pad* cp : cpads) {
                if (fb.side_a.count(cp->net)) a_conn = true;
                if (fb.side_b.count(cp->net)) b_conn = true;
            }
            if (a_conn != b_conn) {
                fb.side_known = true;
                fb.in_nets = a_conn ? fb.side_a : fb.side_b;
                fb.out_nets = a_conn ? fb.side_b : fb.side_a;
            } else {
                fb.in_nets = fb.side_a;
                fb.out_nets = fb.side_b;
            }
            blocks.push_back(std::move(fb));
        }
        return blocks;
    }

    void find_filter_coupling(std::vector<Finding>& out) {
        const auto blocks = find_filter_blocks();
        if (blocks.empty()) return;
        const double cos_tol = std::cos(p_.angle_tol_deg * M_PI / 180.0);
        for (const auto& fb : blocks) {
            // segments of each side, excluding the choke's own footprint
            struct Seg { detail::SegRef r; int cu; };
            std::vector<Seg> in, outs;
            for (size_t i = 0; i < b_.segments.size(); ++i) {
                const Segment& sg = b_.segments[i];
                const double mx = 0.5 * (sg.x1 + sg.x2), my = 0.5 * (sg.y1 + sg.y2);
                if (std::hypot(mx - fb.cx, my - fb.cy) <
                    fb.radius_mm + p_.filter_local_mm)
                    continue;
                const double dx = sg.x2 - sg.x1, dy = sg.y2 - sg.y1;
                const double len = std::hypot(dx, dy);
                if (len < p_.min_run_mm) continue;
                detail::SegRef r{sg.net, sg.cu, sg.x1, sg.y1, sg.x2, sg.y2,
                                 sg.width, len, dx / len, dy / len, false, i};
                if (fb.in_nets.count(sg.net)) in.push_back({r, sg.cu});
                else if (fb.out_nets.count(sg.net)) outs.push_back({r, sg.cu});
            }
            if (in.empty() || outs.empty()) continue;

            double worst_k = 0, worst_len = 0, worst_d = 1e30;
            int cu_a = -1, cu_b = -1;
            Segment ga{}, gb{};
            for (const auto& a : in)
                for (const auto& b : outs) {
                    auto ov = detail::parallel_overlap(a.r, b.r, p_.min_run_mm,
                                                       cos_tol);
                    if (!ov) continue;
                    double k = 0, h = 0;
                    if (a.cu == b.cu) {
                        h = ref_height(a.cu);
                        if (!(h > 0)) continue;
                        k = tline::next_sat_edge(ov->center_d, h);
                    } else {
                        double eps;
                        b_.stackup.dielectric_between(std::min(a.cu, b.cu),
                                                      std::max(a.cu, b.cu), h, eps);
                        k = tline::next_sat_broadside(ov->center_d, h);
                    }
                    if (k > worst_k) {
                        worst_k = k;
                        worst_len = ov->length;
                        worst_d = ov->center_d;
                        cu_a = a.cu; cu_b = b.cu;
                        ga = ov->span_a; gb = ov->span_b;
                    }
                }
            if (!(worst_k > 0)) continue;
            const double db = tline::to_db(worst_k);
            if (db < p_.filter_couple_floor_db) continue;

            Finding f;
            f.rule = "filter-io-coupling";
            f.severity = std::clamp((db + 45.0) / 35.0, 0.25, 0.85);
            f.severity_label = f.severity > 0.55 ? "high"
                               : f.severity > 0.33 ? "medium" : "low";
            f.confidence = "screening-estimate";
            f.coupled_len_mm = worst_len;
            f.min_sep_mm = worst_d;
            f.next_db = db;
            f.cu_a = cu_a; f.cu_b = cu_b;
            char buf[560];
            std::snprintf(
                buf, sizeof buf,
                "The two sides of the filter around %s run beside each other "
                "for %.1f mm at %.2f mm separation, well away from the part "
                "itself: a coupling path of about %.1f dB straight ACROSS the "
                "filter. Insertion loss cannot beat the path that goes around "
                "it — a 60 dB choke behind a %.0f dB bypass delivers about "
                "%.0f dB. This is a layout ceiling, and no change of component "
                "moves it.",
                fb.choke_ref.c_str(), worst_len, worst_d, db, db, db);
            f.title = "Filter bypassed by its own routing: " + fb.choke_ref +
                      " in/out coupled at " +
                      std::to_string((int)std::lround(db)) + " dB";
            f.detail = buf;
            f.remediation =
                "Keep the dirty side and the clean side of the filter apart: "
                "no parallel runs across the block, no shared layer above or "
                "below, and a return-plane gap or a grounded fence between "
                "them. Route the two sides away from each other at the choke, "
                "not alongside it.";
            ga.net = *fb.in_nets.begin(); ga.cu = cu_a;
            gb.net = *fb.out_nets.begin(); gb.cu = cu_b;
            f.geom.lines.push_back(ga);
            f.geom.lines.push_back(gb);
            out.push_back(std::move(f));
        }
    }

    void find_y_cap_return(std::vector<Finding>& out) {
        const auto blocks = find_filter_blocks();
        if (blocks.empty()) return;
        std::map<std::string, const Component*> comps;
        for (const auto& c : b_.components) comps[c.reference] = &c;
        std::map<std::string, std::vector<const Pad*>> by_comp;
        for (const auto& p : b_.pads) by_comp[p.component].push_back(&p);

        struct Y { std::string ref; double d_mm = 0, l_nh = 0, f_res = 0;
                   double px = 0, py = 0, vx = 0, vy = 0; bool have_c = false; };
        std::vector<Y> ys;
        for (const auto& fb : blocks)
            for (const auto& ref : fb.y_caps) {
                auto it = by_comp.find(ref);
                if (it == by_comp.end()) continue;
                const Pad* gnd = nullptr;
                for (const Pad* p : it->second)
                    if (p->net > 0 && is_pour_net(p->net)) gnd = p;
                if (!gnd) continue;
                double vx = 0, vy = 0;
                double d = nearest_via_mm(gnd->net, gnd->x, gnd->y, &vx, &vy);
                if (d > 1e29) continue;      // pours on its own layer
                Y y;
                y.ref = ref;
                y.d_mm = d;
                y.l_nh = d * 0.8 + 0.3;      // escape + one barrel
                y.px = gnd->x; y.py = gnd->y; y.vx = vx; y.vy = vy;
                auto ci = comps.find(ref);
                if (ci != comps.end()) {
                    auto c = values::parse_capacitance(ci->second->value);
                    if (c && *c > 0) {
                        y.have_c = true;
                        y.f_res = 1.0 / (2.0 * M_PI *
                                         std::sqrt(y.l_nh * 1e-9 * *c));
                    }
                }
                ys.push_back(std::move(y));
            }
        if (ys.empty()) return;
        std::sort(ys.begin(), ys.end(),
                  [](const Y& a, const Y& b) { return a.d_mm > b.d_mm; });
        if (ys.front().d_mm < 2.0) return;   // all of them land on their via

        const Y& w = ys.front();
        Finding f;
        f.rule = "y-cap-return";
        f.severity = std::clamp(0.25 + w.d_mm / 25.0, 0.25, 0.7);
        f.severity_label = f.severity > 0.5 ? "high"
                           : f.severity > 0.33 ? "medium" : "low";
        f.confidence = "geometric-only";
        f.coupled_len_mm = w.d_mm;
        char buf[560];
        if (w.have_c)
            std::snprintf(
                buf, sizeof buf,
                "Y capacitor %s reaches the reference plane %.1f mm from its "
                "own pad — about %.1f nH in series with it. With its own value "
                "that branch self-resonates at %.1f MHz, and ABOVE that "
                "frequency the capacitor is an inductor: the common-mode path "
                "it was placed to provide simply stops existing, in the part "
                "of the band where common mode dominates.",
                w.ref.c_str(), w.d_mm, w.l_nh, w.f_res * 1e-6);
        else
            std::snprintf(
                buf, sizeof buf,
                "Y capacitor %s reaches the reference plane %.1f mm from its "
                "own pad — about %.1f nH in series with it, which sets the "
                "frequency where the capacitor stops acting as one. Its value "
                "field could not be parsed, so the resonance is not computed "
                "here rather than guessed.",
                w.ref.c_str(), w.d_mm, w.l_nh);
        f.title = "Y capacitor " + w.ref + " returns through " +
                  std::to_string((int)std::lround(w.d_mm)) + " mm of copper";
        f.detail = buf;
        f.remediation =
            "Via directly at the Y capacitor's ground pad, into the same "
            "reference the choke and the connector shell use. The Y "
            "capacitor's job is the common-mode return; a return through "
            "millimetres of track is the loop it was supposed to close.";
        for (size_t i = 0; i < ys.size() && i < 6; ++i)
            f.geom.lines.push_back(
                {-1, 0, ys[i].px, ys[i].py, ys[i].vx, ys[i].vy, 0.25});
        out.push_back(std::move(f));
    }

    void find_filter_bypass(std::vector<Finding>& out) {
        const auto blocks = find_filter_blocks();
        if (blocks.empty() || sw_nets_.empty()) return;
        for (const auto& fb : blocks) {
            if (!fb.side_known) continue;   // which side is clean is a guess here
            // True segment-to-segment distance, not centre-to-centre: two
            // long tracks running 2 mm apart have centres 30 mm apart, and a
            // centroid test would call that far.
            auto seg_dist = [](const Segment& a, const Segment& b,
                               double& px, double& py, double& qx, double& qy) {
                auto pt_seg = [](double x, double y, const Segment& s,
                                 double& cx, double& cy) {
                    const double dx = s.x2 - s.x1, dy = s.y2 - s.y1;
                    const double L2 = dx * dx + dy * dy;
                    double t = L2 > 0 ? ((x - s.x1) * dx + (y - s.y1) * dy) / L2 : 0.0;
                    t = std::clamp(t, 0.0, 1.0);
                    cx = s.x1 + t * dx;
                    cy = s.y1 + t * dy;
                    return std::hypot(x - cx, y - cy);
                };
                double best = 1e30, cx = 0, cy = 0;
                const std::pair<double, double> ends_a[] = {{a.x1, a.y1}, {a.x2, a.y2}};
                const std::pair<double, double> ends_b[] = {{b.x1, b.y1}, {b.x2, b.y2}};
                for (const auto& [x, y] : ends_a) {
                    const double d = pt_seg(x, y, b, cx, cy);
                    if (d < best) { best = d; px = x; py = y; qx = cx; qy = cy; }
                }
                for (const auto& [x, y] : ends_b) {
                    const double d = pt_seg(x, y, a, cx, cy);
                    if (d < best) { best = d; px = cx; py = cy; qx = x; qy = y; }
                }
                return best;
            };
            double best = 1e30, ax = 0, ay = 0, bx = 0, by = 0;
            std::string sw_name;
            for (const auto& sg : b_.segments) {
                if (!sw_nets_.count(sg.net)) continue;
                for (const auto& cl : b_.segments) {
                    if (!fb.in_nets.count(cl.net)) continue;
                    double px = 0, py = 0, qx = 0, qy = 0;
                    const double d = seg_dist(sg, cl, px, py, qx, qy);
                    if (d < best) {
                        best = d;
                        ax = px; ay = py; bx = qx; by = qy;
                        sw_name = b_.net_name(sg.net);
                    }
                }
            }
            if (best > p_.filter_bypass_mm) continue;
            Finding f;
            f.rule = "filter-bypass";
            f.severity = std::clamp(0.7 - best / 20.0, 0.3, 0.7);
            f.severity_label = f.severity > 0.5 ? "high" : "medium";
            f.confidence = "geometric-only";
            f.coupled_len_mm = best;
            char buf[520];
            std::snprintf(
                buf, sizeof buf,
                "Switching copper (%s) passes within %.1f mm of the CLEAN side "
                "of the filter around %s — the side that leaves the board. "
                "Noise that couples in there has already passed the filter: "
                "the choke and the capacitors act on what flows THROUGH them, "
                "not on what lands on the wire afterwards. This is the "
                "commonest reason a filter measures worse in the product than "
                "on the bench.",
                sw_name.c_str(), best, fb.choke_ref.c_str());
            f.title = "Filter bypassed through the air: switching copper " +
                      std::to_string((int)std::lround(best)) + " mm from " +
                      fb.choke_ref + "'s clean side";
            f.detail = buf;
            f.remediation =
                "Put distance, or a barrier, between the switching stage and "
                "the filter's connector side: keep the clean copper short and "
                "close to the connector, and never route it back past the "
                "converter. On a crowded board, a grounded fence or a can over "
                "the switching stage does what distance cannot.";
            f.geom.lines.push_back({-1, 0, ax, ay, bx, by, 0.3});
            out.push_back(std::move(f));
        }
    }

    // ---- immunity: the ESD clamp, and the copper it does not cover ----
    // ABT #796. Faraday is an emissions tool everywhere else; this is the one
    // immunity mechanism that is PURE LAYOUT, and it is the one a schematic
    // review cannot catch. A TVS is chosen for its clamping voltage, and then
    // the board decides what the protected silicon actually sees:
    //
    //     V_at_the_pin = V_clamp + L_path * di/dt
    //
    // with di/dt ~30 A/ns for an 8 kV IEC 61000-4-2 contact discharge. Four
    // millimetres of track between the connector pin and the clamp is ~3.2 nH
    // and ~96 V that the clamp never sees; the same again in the clamp's own
    // return to the reference plane doubles it. That is why a 5 V TVS on a
    // 3.3 V line still lets a strike kill the part it protects.
    //
    // What this does NOT do, and says so in the finding: judge the TVS itself
    // (that is a part question, and it belongs where parts live), or model
    // surge (61000-4-5) and burst (61000-4-4), whose energy paths leave the
    // board entirely.
    struct ClampPart {
        std::string ref;
        int signal_net = -1;
        double sx = 0, sy = 0;      // the signal-side pad
        double gx = 0, gy = 0;      // the return-side pad
        bool has_return_pad = false;
    };

    // A clamp: a diode-class part with one pad on a signal net and one on the
    // return copper. Refdes prefixes are not a contract (the D/TVS/ESD/VR/Z
    // families all appear in the wild), so the SHAPE decides — the prefix only
    // narrows the search.
    std::vector<ClampPart> find_clamp_parts() const {
        std::map<std::string, std::vector<const Pad*>> by_comp;
        for (const auto& p : b_.pads) by_comp[p.component].push_back(&p);
        std::vector<ClampPart> out;
        for (const auto& [ref, pads] : by_comp) {
            const std::string pre = ref_prefix(ref);
            if (pre != "D" && pre != "TVS" && pre != "ESD" && pre != "VR" &&
                pre != "Z" && pre != "SP")
                continue;
            if (pads.size() < 2) continue;
            const Pad* gnd = nullptr;
            const Pad* sig = nullptr;
            for (const Pad* p : pads) {
                if (p->net <= 0) continue;
                if (is_pour_net(p->net)) { if (!gnd) gnd = p; }
                else if (!sig) sig = p;
            }
            if (!gnd || !sig) continue;
            ClampPart c;
            c.ref = ref;
            c.signal_net = sig->net;
            c.sx = sig->x; c.sy = sig->y;
            c.gx = gnd->x; c.gy = gnd->y;
            c.has_return_pad = true;
            out.push_back(std::move(c));
        }
        return out;
    }

    // Connector pads near the board outline — where a cable, and therefore a
    // discharge, actually arrives. Same edge test the ground-spread rule uses:
    // a refdes prefix alone would sweep in mid-board headers and test points.
    std::vector<const Pad*> connector_pads() const {
        std::vector<const Pad*> out;
        const double bw = b_.bbox_x2 - b_.bbox_x1, bh = b_.bbox_y2 - b_.bbox_y1;
        if (!(bw > 0) || !(bh > 0)) return out;
        const double edge_band = 0.15 * std::min(bw, bh);
        for (const auto& p : b_.pads) {
            const std::string pre = ref_prefix(p.component);
            if (pre != "J" && pre != "X" && pre != "P" && pre != "CN" &&
                pre != "CON")
                continue;
            const double de = std::min(
                std::min(p.x - b_.bbox_x1, b_.bbox_x2 - p.x),
                std::min(p.y - b_.bbox_y1, b_.bbox_y2 - p.y));
            if (de <= edge_band) out.push_back(&p);
        }
        return out;
    }

    double nearest_via_mm(int net, double x, double y, double* vx = nullptr,
                          double* vy = nullptr) const {
        double best = 1e30;
        for (const auto& v : b_.vias) {
            if (v.net != net) continue;
            const double d = std::hypot(v.x - x, v.y - y);
            if (d < best) {
                best = d;
                if (vx) *vx = v.x;
                if (vy) *vy = v.y;
            }
        }
        return best;
    }

    void find_esd_clamp_paths(std::vector<Finding>& out) {
        const auto clamps = find_clamp_parts();
        if (clamps.empty()) return;
        const auto cpads = connector_pads();
        if (cpads.empty()) return;
        const double di_dt = p_.esd_di_dt_a_per_ns;   // A/ns == V per nH

        // --- the copper between the pin and its clamp ---
        struct Far {
            std::string pin_comp, clamp_ref, net;
            double d_mm = 0, v = 0, px = 0, py = 0, cx = 0, cy = 0;
        };
        std::vector<Far> far;
        std::set<std::string> protected_nets;
        for (const Pad* pin : cpads) {
            if (pin->net <= 0 || is_pour_net(pin->net)) continue;
            const ClampPart* best = nullptr;
            double bd = 1e30;
            for (const auto& c : clamps) {
                if (c.signal_net != pin->net) continue;
                const double d = std::hypot(c.sx - pin->x, c.sy - pin->y);
                if (d < bd) { bd = d; best = &c; }
            }
            if (!best || bd > p_.esd_clamp_assoc_max_mm) continue;
            protected_nets.insert(b_.net_name(pin->net));
            if (bd <= p_.esd_clamp_far_mm) continue;
            Far f;
            f.pin_comp = pin->component;
            f.clamp_ref = best->ref;
            f.net = b_.net_name(pin->net);
            f.d_mm = bd;
            f.v = bd * p_.esd_nh_per_mm * di_dt;   // nH * (A/ns) = volts
            f.px = pin->x; f.py = pin->y; f.cx = best->sx; f.cy = best->sy;
            far.push_back(std::move(f));
        }
        std::sort(far.begin(), far.end(),
                  [](const Far& a, const Far& b) { return a.v > b.v; });
        if (!far.empty()) {
            const Far& w = far.front();
            Finding f;
            f.rule = "esd-clamp-distance";
            f.severity = std::clamp(0.25 + w.v / 800.0, 0.25, 0.7);
            f.severity_label = f.severity > 0.5 ? "high"
                               : f.severity > 0.33 ? "medium" : "low";
            // The DISTANCE is geometry; the part's ROLE is inferred (a
            // diode-class part between this net and the return). Both readings
            // are a gap, which is why the finding states the inference instead
            // of hiding it: either the clamp is 22 mm from the pin, or there
            // is no clamp at the pin at all.
            f.confidence = "heuristic (clamp role inferred; distance exact)";
            f.coupled_len_mm = w.d_mm;
            f.net_a = -1;
            char buf[640];
            std::snprintf(
                buf, sizeof buf,
                "The nearest clamp-shaped part on %s (%s) is %s, %.1f mm of "
                "track from the connector pin. A contact discharge is ~%.0f "
                "A/ns and that copper is ~%.1f nH, so about %.0f V appears "
                "across it BEFORE the clamp acts — on top of the TVS's own "
                "clamping voltage, and everything tapped off in between sees "
                "all of it. If %s is not actually a clamp (a power or steering "
                "diode has the same two-terminal shape), then this pin has no "
                "protection near the connector at all — which is the same gap "
                "read the other way. %d protected pin(s) here are further than "
                "%.1f mm from their nearest clamp.",
                w.net.c_str(), w.pin_comp.c_str(), w.clamp_ref.c_str(), w.d_mm,
                di_dt, w.d_mm * p_.esd_nh_per_mm, w.v, w.clamp_ref.c_str(),
                (int)far.size(), p_.esd_clamp_far_mm);
            f.title = "ESD: nearest clamp to " + w.pin_comp + "'s " + w.net +
                      " pin is " + std::to_string((int)std::lround(w.d_mm)) +
                      " mm away (" + w.clamp_ref + ", ~" +
                      std::to_string((int)std::lround(w.v)) + " V of overshoot)";
            f.detail = buf;
            f.remediation =
                "Put the clamp AT the connector pin — the first thing the pin's "
                "copper meets, before any branch, stub or series part. Keep the "
                "track from pin to clamp as short and wide as the footprint "
                "allows; length is inductance and inductance is volts at these "
                "edge rates.";
            for (size_t i = 0; i < far.size() && i < 8; ++i)
                f.geom.lines.push_back(
                    {-1, 0, far[i].px, far[i].py, far[i].cx, far[i].cy, 0.3});
            out.push_back(std::move(f));
        }

        // --- the clamp's own return ---
        struct Ret { std::string ref, net; double d_mm = 0, v = 0,
                     gx = 0, gy = 0, vx = 0, vy = 0; };
        std::vector<Ret> rets;
        for (const auto& c : clamps) {
            bool at_connector = false;
            for (const Pad* pin : cpads)
                if (pin->net == c.signal_net) at_connector = true;
            if (!at_connector) continue;      // only the clamps on cable pins
            double vx = 0, vy = 0;
            int gnet = -1;
            for (const auto& pad : b_.pads)
                if (pad.component == c.ref && pad.net > 0 && is_pour_net(pad.net))
                    gnet = pad.net;
            if (gnet < 0) continue;
            const double d = nearest_via_mm(gnet, c.gx, c.gy, &vx, &vy);
            if (d > 1e29) continue;           // pours on its own layer: no via needed
            if (d <= p_.esd_return_far_mm) continue;
            Ret r;
            r.ref = c.ref;
            r.net = b_.net_name(c.signal_net);
            r.d_mm = d;
            r.v = (d * p_.esd_nh_per_mm + p_.esd_via_nh) * di_dt;
            r.gx = c.gx; r.gy = c.gy; r.vx = vx; r.vy = vy;
            rets.push_back(std::move(r));
        }
        std::sort(rets.begin(), rets.end(),
                  [](const Ret& a, const Ret& b) { return a.v > b.v; });
        if (!rets.empty()) {
            const Ret& w = rets.front();
            Finding f;
            f.rule = "esd-clamp-return";
            f.severity = std::clamp(0.25 + w.v / 800.0, 0.25, 0.65);
            f.severity_label = f.severity > 0.5 ? "high"
                               : f.severity > 0.33 ? "medium" : "low";
            f.confidence = "geometric-only";
            f.coupled_len_mm = w.d_mm;
            char buf[520];
            std::snprintf(
                buf, sizeof buf,
                "%s (on %s) reaches the return plane %.1f mm from its own pad — "
                "~%.1f nH including the barrel. The discharge current has to "
                "flow through that on its way home, so ~%.0f V rides on the "
                "clamp's reference and appears on top of everything it is "
                "supposed to hold down. A clamp is only as good as its return; "
                "%d clamp(s) here are further than %.1f mm from a return via.",
                w.ref.c_str(), w.net.c_str(), w.d_mm,
                w.d_mm * p_.esd_nh_per_mm + p_.esd_via_nh, w.v,
                (int)rets.size(), p_.esd_return_far_mm);
            f.title = "ESD clamp " + w.ref + " returns through " +
                      std::to_string((int)std::lround(w.d_mm)) +
                      " mm of copper (~" +
                      std::to_string((int)std::lround(w.v)) + " V)";
            f.detail = buf;
            f.remediation =
                "Via straight down from the clamp's ground pad into the return "
                "plane — beside the pad, not at the end of a trace. Two vias "
                "halve the barrel inductance, and the plane must be the SAME "
                "reference the protected part uses, or the strike simply moves "
                "the difference between them.";
            for (size_t i = 0; i < rets.size() && i < 8; ++i)
                f.geom.lines.push_back(
                    {-1, 0, rets[i].gx, rets[i].gy, rets[i].vx, rets[i].vy, 0.25});
            out.push_back(std::move(f));
        }
    }

    // Coverage, not a verdict: connector pins that reach silicon with nothing
    // to clamp them. Whether a pin NEEDS protection is a product decision
    // (an internal board-to-board header usually does not), so this is stated
    // at info grade and rolled up per connector — never one finding per pin.
    void find_unprotected_connector_pins(std::vector<Finding>& out) {
        const auto cpads = connector_pads();
        if (cpads.empty()) return;
        const auto clamps = find_clamp_parts();
        // "Clamped" means a clamp part NEAR this connector, not merely one
        // somewhere on the net — a diode at the other end of the board is not
        // protecting this pin, and pretending otherwise would hide the gap.
        std::set<int> clamped;
        for (const Pad* pin : cpads)
            for (const auto& c : clamps)
                if (c.signal_net == pin->net &&
                    std::hypot(c.sx - pin->x, c.sy - pin->y) <=
                        p_.esd_clamp_assoc_max_mm)
                    clamped.insert(pin->net);

        // nets that reach an active part: an IC or a transistor, by pad count
        // and prefix — the things a discharge destroys
        std::map<std::string, std::vector<const Pad*>> by_comp;
        for (const auto& p : b_.pads) by_comp[p.component].push_back(&p);
        std::set<int> active_nets;
        for (const auto& [ref, pads] : by_comp) {
            const std::string pre = ref_prefix(ref);
            const bool active = pre == "U" || pre == "IC" || pre == "Q" ||
                                pre == "T" || pre == "N";
            if (!active) continue;
            for (const Pad* p : pads)
                if (p->net > 0 && !is_pour_net(p->net)) active_nets.insert(p->net);
        }
        if (active_nets.empty()) return;

        std::map<std::string, std::set<std::string>> by_conn;   // conn -> nets
        size_t total = 0;
        for (const Pad* pin : cpads) {
            if (pin->net <= 0 || is_pour_net(pin->net)) continue;
            if (clamped.count(pin->net)) continue;
            if (!active_nets.count(pin->net)) continue;
            by_conn[pin->component].insert(b_.net_name(pin->net));
            ++total;
        }
        if (by_conn.empty()) return;
        std::string list;
        size_t shown = 0;
        for (const auto& [conn, nets] : by_conn) {
            for (const auto& n : nets) {
                if (shown >= p_.esd_max_pins_listed) break;
                list += (shown ? ", " : "") + conn + ":" + n;
                ++shown;
            }
            if (shown >= p_.esd_max_pins_listed) break;
        }
        Finding f;
        f.rule = "esd-unprotected-pin";
        f.severity = 0.2;
        f.severity_label = "info";
        f.confidence = "heuristic";
        char buf[520];
        std::snprintf(
            buf, sizeof buf,
            "%d pin(s) on %d edge connector(s) run from the outside world "
            "straight to an IC or transistor with no clamp part within %.0f mm "
            "of the pin: "
            "%s%s. Whether they need one is a product decision — an internal "
            "board-to-board header usually does not, a user-accessible port "
            "does — so this is a coverage statement, not a defect. What the "
            "layout can say is that nothing on these nets would limit a "
            "discharge.",
            (int)total, (int)by_conn.size(), p_.esd_clamp_assoc_max_mm,
            list.c_str(),
            total > shown ? ", ..." : "");
        f.title = "Unclamped connector pins reaching silicon: " +
                  std::to_string(total);
        f.detail = buf;
        f.remediation =
            "For the ports that are user-accessible, add a clamp at the pin "
            "(and check its return via). For the ones that are not, nothing — "
            "record the decision rather than the part.";
        out.push_back(std::move(f));
    }

    // ---- rule: connector grounds scattered around the board ----
    // Franz (EMV, 5th ed., §7.2): "Die Masseanschlüsse aller von einer
    // Baugruppe nach außen gehenden Leitungen sind möglichst nahe beieinander
    // zu platzieren und niederimpedant miteinander zu verbinden
    // (Sternstruktur)." — his single most emphasized layout rule. Ground
    // entries scattered around the perimeter make the board a
    // Reihenmassestruktur: the ground potential difference between two entry
    // points drives the attached CABLES as a dipole (measured on his GTEM
    // fixture: 23.5 dB between the scattered and the star layout, §7.3.1),
    // and every external ground loop couples through the copper between the
    // entries. Cable common-mode is what actually fails EMC tests, so this is
    // the one rule that sees it in pure layout geometry.
    //
    // What softens it, also from Franz: a continuous plane between the
    // entries is his "Vermaschung" decoupling method — the coupling impedance
    // gets small (not zero). So the severity keys on whether a return plane
    // exists: without one this is the loudest defect on the board; with one
    // it is a stated, reviewable risk.
    void find_connector_ground_spread(std::vector<Finding>& out) {
        // Connectors, by refdes AND position: connector-class prefixes only,
        // carrying the return net, sitting near the outline (cables leave at
        // the edge). Refdes alone is not a contract — the edge test is what
        // keeps mid-board test-point grids and headers out.
        const double bw = b_.bbox_x2 - b_.bbox_x1, bh = b_.bbox_y2 - b_.bbox_y1;
        if (!(bw > 0) || !(bh > 0)) return;
        const double edge_band = 0.15 * std::min(bw, bh);
        struct Conn { std::string ref; double gx = 0, gy = 0; int n = 0; };
        std::map<std::string, Conn> conns;
        for (const auto& p : b_.pads) {
            const std::string pre = ref_prefix(p.component);
            if (pre != "J" && pre != "X" && pre != "P" && pre != "CN" &&
                pre != "CON")
                continue;
            if (p.net <= 0 || !is_pour_net(p.net)) continue;
            const double de = std::min(
                std::min(p.x - b_.bbox_x1, b_.bbox_x2 - p.x),
                std::min(p.y - b_.bbox_y1, b_.bbox_y2 - p.y));
            if (de > edge_band) continue;
            Conn& c = conns[p.component];
            c.ref = p.component;
            c.gx += p.x; c.gy += p.y; ++c.n;
        }
        if (conns.size() < 2) return;
        const Conn *wa = nullptr, *wb = nullptr;
        double worst = 0;
        for (auto i = conns.begin(); i != conns.end(); ++i)
            for (auto j = std::next(i); j != conns.end(); ++j) {
                const double d = std::hypot(i->second.gx / i->second.n -
                                                j->second.gx / j->second.n,
                                            i->second.gy / i->second.n -
                                                j->second.gy / j->second.n);
                if (d > worst) { worst = d; wa = &i->second; wb = &j->second; }
            }
        const double diag = std::hypot(bw, bh);
        if (!wa || worst < 0.4 * diag) return;   // clustered enough — the star
        bool have_plane = false;
        for (const auto& lm : layers_) have_plane = have_plane || lm.is_plane;
        Finding f;
        f.rule = "connector-ground-spread";
        f.severity = have_plane ? 0.28 : 0.55;
        f.severity_label = have_plane ? "low" : "medium";
        f.confidence = "heuristic";
        char buf[420];
        std::snprintf(buf, sizeof buf,
                      "Ground entries of %s and %s sit %.0f mm apart (board "
                      "diagonal %.0f mm). Every cable attached between them is "
                      "driven by the ground potential difference across that "
                      "copper — a series-ground structure, and the mechanism "
                      "behind most cable common-mode failures. %s",
                      wa->ref.c_str(), wb->ref.c_str(), worst, diag,
                      have_plane
                          ? "A return plane covers the span, which keeps the "
                            "coupling impedance low — review, not necessarily "
                            "rework."
                          : "No continuous return plane connects them, so the "
                            "full ground impedance appears between the cable "
                            "roots.");
        f.title = "Connector grounds " + std::to_string((int)std::lround(worst)) +
                  " mm apart: " + wa->ref + " <-> " + wb->ref;
        f.detail = buf;
        f.remediation =
            "Cluster the off-board connections on one board edge so their "
            "grounds share one reference point (Franz's star structure, worth "
            "~24 dB of cable radiation in his measured comparison). Where the "
            "placement is fixed, make the copper between the entries as "
            "low-impedance as possible and consider a common-mode choke per "
            "cable.";
        f.geom.lines.push_back({-1, 0, wa->gx / wa->n, wa->gy / wa->n,
                                wb->gx / wb->n, wb->gy / wb->n, 0.3});
        out.push_back(std::move(f));
    }

    // ---- rule: power/ground plane cavity modes ----
    // Franz (EMV, 5th ed., §5.9.3): a VCC/GND plane pair is a 2-D
    // transmission line, open at the board edge; above the parallel resonance
    // the decoupling capacitors do NOTHING and standing waves (modes) set the
    // supply impedance. f_mn = c0/(2*sqrt(eps_r)) * sqrt((m/a)^2 + (n/b)^2)
    // (his Gl. 5.3). Every mode has an extremum in the CORNERS, so a switching
    // part in a corner can pump them all; the board centre is a null of the
    // 10, 01 and 11 modes — his measured difference is 17 dB on the 10-mode.
    void find_plane_cavity_modes(std::vector<Finding>& out) {
        // an adjacent plane PAIR on different nets (the unstitchable cavity)
        int top = -1;
        double h = 0, er = 4.5;
        for (size_t i = 0; i + 1 < layers_.size(); ++i)
            if (layers_[i].is_plane && layers_[i + 1].is_plane &&
                layers_[i].plane_net != layers_[i + 1].plane_net &&
                layers_[i + 1].ref_up == (int)i) {
                top = (int)i;
                h = layers_[i + 1].h_up;
                er = layers_[i + 1].eps_up;
                break;
            }
        if (top < 0 || sw_nets_.empty()) return;
        const double a = (b_.bbox_x2 - b_.bbox_x1) * 1e-3;
        const double bl = (b_.bbox_y2 - b_.bbox_y1) * 1e-3;
        if (!(a > 0) || !(bl > 0)) return;
        const double c0 = 299792458.0, k = c0 / (2.0 * std::sqrt(er));
        const double f10 = k / a, f01 = k / bl,
                     f11 = k * std::hypot(1.0 / a, 1.0 / bl);
        // where does the loudest aggressor sit relative to the cavity?
        double sx = 0, sy = 0; int sn = 0;
        for (const auto& p : b_.pads)
            if (sw_nets_.count(p.net)) { sx += p.x; sy += p.y; ++sn; }
        if (!sn) return;
        sx /= sn; sy /= sn;
        const double rx = (sx - b_.bbox_x1) / (b_.bbox_x2 - b_.bbox_x1);
        const double ry = (sy - b_.bbox_y1) / (b_.bbox_y2 - b_.bbox_y1);
        const bool corner = (rx < 0.25 || rx > 0.75) && (ry < 0.25 || ry > 0.75);
        Finding f;
        f.rule = "plane-cavity-mode";
        f.severity = corner ? 0.3 : 0.18;
        f.severity_label = corner ? "low" : "info";
        f.confidence = "heuristic";
        f.cu_a = top;
        char buf[460];
        std::snprintf(
            buf, sizeof buf,
            "The %s/%s plane pair (%.2f mm apart) is a resonant cavity: "
            "first modes at %.0f, %.0f and %.0f MHz. Above the "
            "capacitor/plane parallel resonance the decoupling capacitors no "
            "longer act — these modes set the supply impedance there. The "
            "switching aggressor's centroid sits at (%.0f%%, %.0f%%) of the "
            "board: %s",
            b_.copper_names[top].c_str(), b_.copper_names[top + 1].c_str(), h,
            f10 * 1e-6, f01 * 1e-6, f11 * 1e-6, rx * 100, ry * 100,
            corner ? "a corner region, where every mode has an extremum and "
                     "can be pumped (centre placement suppresses the first "
                     "three modes — 17 dB measured on the 10-mode)"
                   : "away from the corners, which limits how many modes it "
                     "can excite");
        f.title = "Plane cavity: first mode " +
                  std::to_string((int)std::lround(f10 * 1e-6)) + " MHz (" +
                  b_.copper_names[top] + "/" + b_.copper_names[top + 1] + ")";
        f.detail = buf;
        f.remediation =
            "If emissions cluster at these frequencies: thinner plane "
            "spacing lowers the cavity impedance everywhere; lossy "
            "termination (R+C to the plane edge, or ESR-controlled "
            "capacitors) damps the modes; and moving the switching cluster "
            "toward the board centre stops the first three modes being "
            "driven at all.";
        f.geom.markers.push_back({sx, sy});
        out.push_back(std::move(f));
    }

    // ---- rule: decoupling caps reaching their plane through a long stub ----
    // Franz (EMV, 5th ed., §5.6/§5.9.5): above its series resonance a
    // decoupling capacitor IS its inductance, and the connection stubs are
    // in series with it — "Verbindungsleitungen im Abblockzweig müssen so
    // kurz wie fertigungstechnisch möglich ausgeführt werden." His measured
    // via table (Tab. 5.2): a second via pair alone cuts the branch
    // inductance ~19%. The Würth checklist's version: plane vias within
    // 0.3 mm of the pad. A pad whose nearest same-net via is millimetres
    // away adds ~0.8 nH/mm of stub — often more than the capacitor itself.
    void find_cap_via_stubs(std::vector<Finding>& out) {
        // needs a plane to reach: skip boards without one
        std::set<int> plane_nets;
        for (const auto& lm : layers_)
            if (lm.is_plane && lm.plane_net > 0) plane_nets.insert(lm.plane_net);
        if (plane_nets.empty()) return;
        struct Worst { std::string ref; double d; double x, y, vx, vy; };
        std::vector<Worst> bad;
        std::map<std::string, double> cap_worst;   // ref -> worst pad stub
        std::map<std::string, Worst> cap_geom;
        for (const auto& p : b_.pads) {
            if (ref_prefix(p.component) != "C") continue;
            if (p.net <= 0 || !plane_nets.count(p.net)) continue;
            if (p.through_hole) continue;          // its own barrel IS the via
            // the plane carrying this net must be on ANOTHER layer — a pour
            // on the pad's own layer is a direct lateral connection
            bool remote = false;
            for (size_t i = 0; i < layers_.size(); ++i)
                if (layers_[i].is_plane && layers_[i].plane_net == p.net &&
                    (int)i != p.cu)
                    remote = true;
            if (!remote) continue;
            double best = 1e30, vx = 0, vy = 0;
            for (const auto& v : b_.vias)
                if (v.net == p.net) {
                    const double d = std::hypot(v.x - p.x, v.y - p.y);
                    if (d < best) { best = d; vx = v.x; vy = v.y; }
                }
            if (best > 1e29) continue;             // no via anywhere: zone-connected
            auto it = cap_worst.find(p.component);
            if (it == cap_worst.end() || best > it->second) {
                cap_worst[p.component] = best;
                cap_geom[p.component] = {p.component, best, p.x, p.y, vx, vy};
            }
        }
        for (auto& [ref, d] : cap_worst)
            if (d > 2.0) bad.push_back(cap_geom[ref]);
        if (bad.empty()) return;
        std::sort(bad.begin(), bad.end(),
                  [](const Worst& x, const Worst& y) { return x.d > y.d; });
        Finding f;
        f.rule = "cap-via-stub";
        f.severity = std::clamp(0.2 + bad[0].d / 30.0, 0.2, 0.6);
        f.severity_label = f.severity > 0.33 ? "medium" : "low";
        f.confidence = "geometric-only";
        f.coupled_len_mm = bad[0].d;
        std::string worst_list;
        for (size_t i = 0; i < bad.size() && i < 5; ++i)
            worst_list += (i ? ", " : "") + bad[i].ref + " (" +
                          std::to_string((int)std::lround(bad[i].d)) + " mm)";
        char buf[420];
        std::snprintf(
            buf, sizeof buf,
            "%d decoupling capacitor(s) reach their plane through a stub "
            "longer than 2 mm — worst %s. Above series resonance the "
            "capacitor is only its inductance, and every millimetre of stub "
            "adds ~0.8 nH in series: at %.0f mm the connection out-inducts "
            "the capacitor itself. A second via pair beside the pad alone is "
            "worth ~19%% of the branch inductance.",
            (int)bad.size(), worst_list.c_str(), bad[0].d);
        f.title = "Decoupling via stubs: " + std::to_string(bad.size()) +
                  " cap(s), worst " +
                  std::to_string((int)std::lround(bad[0].d)) + " mm";
        f.detail = buf;
        f.remediation =
            "Give each such pad its own via pair into the plane, placed "
            "beside the pad (not at the end of a trace run) — the checklist "
            "figure is vias within 0.3 mm of the pad.";
        for (size_t i = 0; i < bad.size() && i < 8; ++i)
            f.geom.lines.push_back(
                {-1, 0, bad[i].x, bad[i].y, bad[i].vx, bad[i].vy, 0.25});
        out.push_back(std::move(f));
    }

    // ---- rule: switch-node copper at the board edge ----
    // WE EMV-Checkliste V8 §10 [HZ] Edge-Coupling: copper near the board
    // edge couples into free radiation — the field is no longer confined
    // between trace and plane, and the plane's image current is truncated.
    // The fix the same checklist names: keep the plane to the edge and fence
    // it with return vias. Only the loudest copper is screened (switch
    // nodes); flagging every edge-routed net would bury the signal.
    void find_edge_radiation(std::vector<Finding>& out) {
        if (sw_nets_.empty() || !b_.bbox_from_outline) return;
        for (int net : sw_nets_) {
            double worst = 1e30, wx = 0, wy = 0;
            double len_near = 0, lim_used = 1.5;
            FindingGeom geom;
            // the clearance that matters is relative to the dielectric
            // height: at < 2h the edge field dominates the plane's
            double h = 0.3;
            for (const auto& s : b_.segments) {
                if (s.net != net) continue;
                const LayerModel& lm = layers_[s.cu];
                if (lm.ref_up >= 0 || lm.ref_dn >= 0)
                    h = std::max(
                        {0.1, lm.ref_up >= 0 ? lm.h_up : 0.0,
                         lm.ref_dn >= 0 ? lm.h_dn : 0.0});
                auto edge_d = [&](double x, double y) {
                    return std::min(
                        std::min(x - b_.bbox_x1, b_.bbox_x2 - x),
                        std::min(y - b_.bbox_y1, b_.bbox_y2 - y));
                };
                const double d =
                    std::min(edge_d(s.x1, s.y1), edge_d(s.x2, s.y2));
                const double lim = std::max(1.5, 2.0 * h);
                if (d < lim) {
                    lim_used = std::max(lim_used, lim);
                    len_near += std::hypot(s.x2 - s.x1, s.y2 - s.y1);
                    if (geom.lines.size() < 60) geom.lines.push_back(s);
                    if (d < worst) { worst = d; wx = s.x1; wy = s.y1; }
                }
            }
            if (len_near < 2.0) continue;   // a corner clip, not a run
            // softener: a return-via fence between the copper and the edge
            int fence = 0;
            for (const auto& v : b_.vias)
                if (is_pour_net(v.net) &&
                    std::hypot(v.x - wx, v.y - wy) < 5.0)
                    ++fence;
            Finding f;
            f.rule = "edge-radiation";
            f.severity = fence >= 3 ? 0.25 : 0.42;
            f.severity_label = fence >= 3 ? "low" : "medium";
            f.confidence = "geometric-only";
            f.net_a = net;
            f.coupled_len_mm = len_near;
            char buf[360];
            std::snprintf(
                buf, sizeof buf,
                "%.0f mm of switch node %s runs within %.1f mm of the board "
                "edge (closest %.2f mm). At the edge the field is no longer "
                "confined between trace and plane — the dv/dt couples into "
                "free space from the loudest copper on the board. %s",
                len_near, b_.net_name(net).c_str(), lim_used, worst,
                fence >= 3 ? "A return-via fence is present near the closest "
                             "approach, which contains most of it."
                           : "No return-via fence stands between it and the "
                             "edge.");
            f.title = "Switch node at board edge: " + b_.net_name(net) +
                      " (" + std::to_string((int)std::lround(len_near)) +
                      " mm)";
            f.detail = buf;
            f.remediation =
                "Pull the switch-node copper inboard, keep the return plane "
                "out to the edge beneath it, and fence the edge with "
                "stitching vias (checklist: pitch <= lambda/10 at the "
                "highest aggressor harmonic).";
            f.geom = std::move(geom);
            out.push_back(std::move(f));
        }
    }

    // ---- rule: signal runs crossing plane voids/splits ----
    void find_plane_crossings(std::vector<Finding>& out) {
        // zone bboxes per layer for cheap point tests
        struct ZBox { double x1, y1, x2, y2; const ZonePoly* z; };
        std::vector<std::vector<ZBox>> zb(layers_.size());
        for (const auto& z : b_.zones) {
            ZBox box{1e30, 1e30, -1e30, -1e30, &z};
            for (const auto& p : z.pts) {
                box.x1 = std::min(box.x1, p.x); box.y1 = std::min(box.y1, p.y);
                box.x2 = std::max(box.x2, p.x); box.y2 = std::max(box.y2, p.y);
            }
            zb[z.cu].push_back(box);
        }
        auto covered = [&](int plane_cu, double x, double y) {
            for (const auto& box : zb[plane_cu]) {
                if (x < box.x1 || x > box.x2 || y < box.y1 || y > box.y2) continue;
                if (box.z->contains(x, y)) return true;
            }
            return false;
        };

        // planes that actually have fill geometry to test against
        std::vector<int> testable;
        for (size_t i = 0; i < layers_.size(); ++i)
            if (layers_[i].is_plane && !zb[i].empty()) testable.push_back((int)i);

        // hard break: NO plane covers (per net, actionable individually).
        // detour: the NEAREST plane is void but a farther one covers — real
        // but systemic, so aggregated per (signal layer, nearest plane):
        // 135 individual rows on HackRF One taught this.
        struct Gap { double len = 0; std::vector<Point> pts; int cu = -1; int plane = -1; };
        std::map<int, Gap> gaps;
        struct Detour {
            double len = 0;
            std::map<int, double> len_by_net;
            std::vector<Point> pts;
        };
        std::map<std::pair<int, int>, Detour> detours;  // (signal cu, nearest plane)
        for (const auto& s : b_.segments) {
            if (s.net <= 0 || is_pour_net(s.net)) continue;  // pour nets only
            const LayerModel& lm = layers_[s.cu];
            int nearest = lm.ref_up >= 0 ? lm.ref_up : lm.ref_dn;
            if (nearest < 0) continue;  // handled by no-reference-plane
            // a plane known only from its 'power' layer-type hint has no fill
            // geometry to test against — skipping is stated in meta, never
            // silently flagged (a zoneless plane would fail EVERY sample)
            if (zb[nearest].empty()) {
                unverifiable_planes_.insert(b_.copper_names[nearest]);
                continue;
            }
            double len = std::hypot(s.x2 - s.x1, s.y2 - s.y1);
            int steps = std::max(1, (int)(len / p_.sample_step_mm));
            for (int i = 0; i <= steps; ++i) {
                double t = (double)i / steps;
                double x = s.x1 + (s.x2 - s.x1) * t, y = s.y1 + (s.y2 - s.y1) * t;
                if (covered(nearest, x, y)) continue;
                bool any = false;
                for (int p : testable)
                    if (p != nearest && covered(p, x, y)) { any = true; break; }
                if (any) {
                    Detour& d = detours[{s.cu, nearest}];
                    d.len += len / steps;
                    d.len_by_net[s.net] += len / steps;
                    if (d.pts.size() < 64) d.pts.push_back({x, y});
                } else {
                    Gap& g = gaps[s.net];
                    g.len += len / steps;
                    g.cu = s.cu;
                    g.plane = nearest;
                    if (g.pts.size() < 64) g.pts.push_back({x, y});
                }
            }
        }
        for (auto& [key, d] : detours) {
            if (d.len < p_.sample_step_mm) continue;
            auto [cu, plane] = key;
            Finding f;
            f.rule = "sparse-reference";
            f.severity = std::clamp(0.3 + d.len / 300.0, 0.3, 0.65);
            f.severity_label = "medium";
            f.confidence = "exact";
            f.cu_a = cu;
            f.cu_b = plane;
            f.coupled_len_mm = d.len;
            f.title = "Sparse reference: " + b_.copper_names[plane] + " under " +
                      b_.copper_names[cu] + " (" +
                      std::to_string(d.len_by_net.size()) + " nets)";
            // worst offenders by detour length
            std::vector<std::pair<double, int>> worst;
            for (auto& [net, l] : d.len_by_net) worst.push_back({l, net});
            std::sort(worst.rbegin(), worst.rend());
            std::string names;
            for (size_t i = 0; i < worst.size() && i < 5; ++i)
                names += (i ? ", " : "") + b_.net_name(worst[i].second);
            char buf[200];
            std::snprintf(buf, sizeof buf,
                          "%.0f mm of routing on %s runs where its nearest plane "
                          "(%s) is void; a farther plane covers, so the return "
                          "current detours through a larger loop. Worst nets: ",
                          d.len, b_.copper_names[cu].c_str(),
                          b_.copper_names[plane].c_str());
            f.detail = std::string(buf) + names + ".";
            f.remediation = "Densify the " + b_.copper_names[plane] +
                            " pour under these runs, or accept the longer return "
                            "path knowingly (state it in the EMC file).";
            for (const auto& pt : d.pts) f.geom.markers.push_back(pt);
            out.push_back(std::move(f));
        }
        // Many hard breaks against the same plane is one systemic problem, not
        // N independent ones (HackRF One produced 129 rows). Roll them up past
        // a threshold, naming the worst offenders; keep them individual while
        // the count is small enough to act on one by one.
        std::map<std::pair<int, int>, std::vector<std::pair<double, int>>> by_pair;
        for (auto& [net, g] : gaps) {
            if (g.len < p_.sample_step_mm) continue;
            by_pair[{g.cu, g.plane}].push_back({g.len, net});
        }
        std::set<int> rolled;
        for (auto& [key, v] : by_pair) {
            if (v.size() <= p_.max_individual_breaks) continue;
            auto [cu, plane] = key;
            std::sort(v.rbegin(), v.rend());
            double total = 0;
            for (auto& [l, n] : v) { total += l; rolled.insert(n); }
            std::string names;
            for (size_t i = 0; i < v.size() && i < 5; ++i)
                names += (i ? ", " : "") + b_.net_name(v[i].second);
            Finding f;
            f.rule = "plane-crossing";
            f.severity = std::clamp(0.6 + total / 400.0, 0.6, 1.0);
            f.severity_label = "high";
            f.confidence = "exact";
            f.cu_a = cu;
            f.cu_b = plane;
            f.coupled_len_mm = total;
            f.title = "Return-path breaks on " + b_.copper_names[cu] + ": " +
                      std::to_string(v.size()) + " nets";
            char buf[260];
            std::snprintf(buf, sizeof buf,
                          "%zu nets routed on %s cross places where NO plane "
                          "covers, %.0f mm in total. %s is the nearest plane "
                          "and it does not reach there. Worst: ",
                          v.size(), b_.copper_names[cu].c_str(), total,
                          b_.copper_names[plane].c_str());
            f.detail = std::string(buf) + names +
                       ". Each of these return currents must detour around the "
                       "gap; at this count it is a plane-coverage problem "
                       "rather than N routing mistakes.";
            f.remediation = "Extend the pour to cover this region, or move the "
                            "affected routing over solid plane.";
            for (auto& [l, n] : v)
                for (const auto& pt : gaps[n].pts) {
                    if (f.geom.markers.size() >= 200) break;
                    f.geom.markers.push_back(pt);
                }
            out.push_back(std::move(f));
        }
        for (auto& [net, g] : gaps) {
            if (g.len < p_.sample_step_mm) continue;  // sub-pitch noise
            if (rolled.count(net)) continue;          // covered by the roll-up
            Finding f;
            f.rule = "plane-crossing";
            f.severity = std::clamp(0.5 + g.len / 40.0, 0.5, 1.0);
            f.severity_label = f.severity > 0.66 ? "high" : "medium";
            f.confidence = "exact";
            f.net_a = net;
            f.cu_a = g.cu;
            f.cu_b = g.plane;
            f.coupled_len_mm = g.len;
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "%.1f mm of net %s on %s runs over a void/split in its "
                          "reference plane (%s).",
                          g.len, b_.net_name(net).c_str(),
                          b_.copper_names[g.cu].c_str(),
                          b_.copper_names[g.plane].c_str());
            f.title = "Return-path break: " + b_.net_name(net);
            f.detail = std::string(buf) +
                       " The return current must detour, forming a loop — a "
                       "primary radiated-emission and coupling mechanism.";
            f.remediation = "Reroute around the split, or place stitching "
                            "capacitors/vias across it next to the crossing.";
            for (const auto& pt : g.pts) f.geom.markers.push_back(pt);
            out.push_back(std::move(f));
        }
    }

    BoardIR b_;  // by value: screener owns a stable copy (stackup mutated: type hints)
    ScreenerParams p_;
    std::vector<LayerModel> layers_;
    size_t dropped_below_floor_ = 0;
    size_t dropped_by_cap_ = 0;
    size_t diff_pairs_recognized_ = 0;
    std::set<std::string> unverifiable_planes_;
    struct SwCandidate {
        int net;
        std::vector<std::string> wound, active;
    };
    std::set<int> sw_nets_;
    std::set<int> sw_user_;                    // subset of sw_nets_: user-declared
    std::vector<SwCandidate> sw_candidates_;   // ambiguous, offered not screened
    std::string sw_prefix_ = "Q";   // see decide_sw_prefix()
    std::set<int> big_pour_nets_;
    std::vector<double> routed_mm_;
};

// ---- findings → JSON (report payload for CLI/web) ----

inline nlohmann::json to_json(const Finding& f) {
    nlohmann::json j{{"id", f.id},
                     {"rule", f.rule},
                     {"severity", f.severity},
                     {"severityLabel", f.severity_label},
                     {"confidence", f.confidence},
                     {"title", f.title},
                     {"detail", f.detail},
                     {"remediation", f.remediation},
                     {"coupledLenMm", f.coupled_len_mm},
                     {"minSepMm", f.min_sep_mm},
                     {"netA", f.net_a},
                     {"netB", f.net_b},
                     {"cuA", f.cu_a},
                     {"cuB", f.cu_b}};
    if (f.next_db) j["nextDb"] = *f.next_db;
    if (f.solve) j["solve"] = *f.solve;
    if (f.emit) j["emit"] = *f.emit;
    nlohmann::json lines = nlohmann::json::array();
    for (const auto& s : f.geom.lines)
        lines.push_back({{"cu", s.cu}, {"x1", s.x1}, {"y1", s.y1},
                         {"x2", s.x2}, {"y2", s.y2}, {"w", s.width}});
    nlohmann::json markers = nlohmann::json::array();
    for (const auto& p : f.geom.markers) markers.push_back({p.x, p.y});
    j["geom"] = {{"lines", lines}, {"markers", markers}};
    return j;
}

// Full analysis: board JSON + ranked findings + meta + per-width Z0 table.
// analyze_board lives in Report.hpp: the full report includes the PDN
// anti-resonance pass, which stands on Pdn.hpp and therefore cannot be
// defined here (Pdn includes Screener, never the reverse).

}  // namespace faraday
