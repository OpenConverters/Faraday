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
    int sw_max_pads = 12;            // above this a L+Q net is a rail, not a switch node
    double min_via_stub_mm = 0.3;    // ignore stubs shorter than this
    double min_dangling_mm = 1.0;    // ignore dangling ends shorter than this
    double decoupling_far_mm = 6.0;  // beyond this a decoupling cap is "reaching"
    size_t max_individual_breaks = 8;  // more hard breaks on one plane -> roll up
    double return_pour_fraction = 0.2; // pour >= this share of the board = a return net
    size_t max_findings = 200;       // hard cap on emitted findings (dropped count reported)
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

    std::vector<Finding> run() {
        std::vector<Finding> out;
        find_no_reference_plane(out);
        find_switch_nodes(out);
        find_commutation_loops(out);
        find_coupled_runs(out);
        find_plane_crossings(out);
        find_via_stubs(out);
        find_dangling_stubs(out);
        find_decoupling(out);
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
                {"polygonOnlyNets", polyonly}};
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

        std::vector<std::map<int, double>> area_by_net(n);
        for (const auto& z : b_.zones)
            area_by_net[z.cu][z.net] += std::abs(z.signed_area());
        for (size_t i = 0; i < n; ++i) {
            double total = 0.0;
            int dominant = -1;
            double dom_area = 0.0;
            for (auto& [net, area] : area_by_net[i]) {
                total += area;
                if (area > dom_area) { dom_area = area; dominant = net; }
            }
            layers_[i].zone_coverage = total / board_area;
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
        // nets carrying a substantial pour anywhere (see is_pour_net)
        std::map<int, double> pour_area;
        for (const auto& z : b_.zones)
            if (z.net > 0) pour_area[z.net] += std::abs(z.signed_area());
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

    void build_sw_nets() {
        std::map<int, std::set<std::string>> prefixes;
        std::map<int, int> pad_count, q_pads;
        for (const auto& p : b_.pads) {
            if (p.net <= 0 || p.component.empty() || is_plane_net(p.net)) continue;
            std::string pre = ref_prefix(p.component);
            prefixes[p.net].insert(pre);
            ++pad_count[p.net];
            if (pre == "Q") ++q_pads[p.net];
        }
        for (auto& [net, pre] : prefixes) {
            if (pad_count[net] > p_.sw_max_pads) continue;   // a rail, not a node
            bool buck_like = pre.count("L") && pre.count("Q");
            // Inverter/half-bridge: several FETs meet with no inductor (the
            // motor IS the inductance — VESC found 0 switch nodes without
            // this). But GATE nets also gather many Q pads (VESC's H1_LOW has
            // 5), so require the net to sit in the POWER path: it must also
            // reach a capacitor, an inductor or a connector. VESC's phase
            // nodes carry {Q,C,P,R,U}; its gate nets carry only {Q,R,U}. This
            // is a topology test, not a trace-width guess — widths overlap too
            // much to separate them (gate nets run up to 2.29 mm there).
            bool power_path = pre.count("C") || pre.count("L") ||
                              pre.count("P") || pre.count("J");
            bool bridge_like = q_pads[net] >= 2 && power_path;
            if (buck_like || bridge_like) sw_nets_.insert(net);
        }
    }

    // ---- rule: commutation loop area (THE converter EMC metric) ----
    // The high-di/dt loop is input-cap(+) -> high-side switch -> switch node ->
    // low-side switch -> input-cap(-). Its ENCLOSED AREA sets radiated
    // emissions and ringing. We find it from connectivity: the FETs on a
    // switch node, and the nearest capacitor bridging one of their other rails
    // to a pour net. Reported as a heuristic with the geometry drawn, so the
    // user can see exactly which loop was measured.
    struct LoopResult {
        double area_mm2 = 0;
        double cap_dist_mm = 0;
        std::string cap_ref;
        std::vector<Point> hull;
    };

    std::optional<LoopResult> commutation_loop(int sw_net) const {
        // Switching devices on the node. A synchronous converter has two FETs;
        // an ASYNCHRONOUS one has a FET and a freewheel diode, and the diode
        // carries half the commutation current — omitting it left the
        // LibreSolar mppt-2420-lc (Q7 + D8) with no measurable loop.
        std::set<std::string> fets;
        for (const auto& p : b_.pads) {
            if (p.net != sw_net) continue;
            std::string pre = ref_prefix(p.component);
            if (pre == "Q" || pre == "D") fets.insert(p.component);
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
                if (d < best_d) { best_d = d; best.cap_ref = ref; }
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
            char buf[280];
            std::snprintf(buf, sizeof buf,
                          "Commutation loop around %s encloses about %.0f mm^2; "
                          "the nearest bulk/input capacitor (%s) sits %.1f mm "
                          "from the switching devices.",
                          b_.net_name(net).c_str(), loop->area_mm2,
                          loop->cap_ref.c_str(), loop->cap_dist_mm);
            f.title = "Commutation loop: " + b_.net_name(net) + " (" +
                      std::to_string((int)loop->area_mm2) + " mm^2)";
            f.detail = std::string(buf) +
                       " This loop carries the discontinuous switching current; "
                       "its enclosed area is the dominant radiated-emission and "
                       "ringing mechanism in a converter. The loop shown is the "
                       "hull of the switching devices and that capacitor — "
                       "verify it matches your intended commutation path.";
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
            out.push_back(std::move(f));
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
            f.confidence = "heuristic";
            f.net_a = net;
            f.coupled_len_mm = area;  // mm^2 — the extent metric for this rule
            char buf[200];
            std::snprintf(buf, sizeof buf,
                          "Net %s joins an inductor pad and a switch/diode pad — "
                          "identified as a converter switch node (dv/dt "
                          "aggressor). Copper extent %.0f mm^2.",
                          b_.net_name(net).c_str(), area);
            f.title = "Switch node: " + b_.net_name(net) + " (extent " +
                      std::to_string((int)area) + " mm^2)";
            f.detail = std::string(buf) +
                       " Verify the identification; every mm^2 of SW copper "
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
            acc.min_edge_sep = std::min(acc.min_edge_sep,
                                        ov.center_d - 0.5 * (w_a + w_b));
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
                if (p.net != rail || ref_prefix(p.component) != "U") continue;
                for (const auto& cp : ci.pts) {
                    double d = std::hypot(p.x - cp.x, p.y - cp.y);
                    if (d < best) { best = d; ic = p.component; ic_pt = {p.x, p.y};
                                    cap_pt = cp; }
                }
            }
            if (ic.empty() || best > p_.decoupling_far_mm * 4) continue;
            if (best <= p_.decoupling_far_mm) continue;   // well placed
            Finding f;
            f.rule = "decoupling-distance";
            f.severity = std::clamp(0.2 + (best - p_.decoupling_far_mm) / 25.0,
                                    0.2, 0.7);
            f.severity_label = f.severity > 0.33 ? "medium" : "low";
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
            f.remediation = "Move " + ref + " next to the " + ic +
                            " pin (ideally on the same side, with its own via "
                            "pair into the plane).";
            f.geom.lines.push_back({rail, 0, cap_pt.x, cap_pt.y, ic_pt.x, ic_pt.y, 0.2});
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
    std::set<int> sw_nets_;
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
inline nlohmann::json analyze_board(const BoardIR& board, ScreenerParams params = {}) {
    Screener sc(board, params);
    auto findings = sc.run();
    nlohmann::json fj = nlohmann::json::array();
    for (const auto& f : findings) fj.push_back(to_json(f));

    // Z0 tooltip table: one entry per (cu, width) actually routed
    std::set<std::pair<int, long long>> seen;
    nlohmann::json z0s = nlohmann::json::array();
    for (const auto& s : board.segments) {
        auto key = std::make_pair(s.cu, (long long)std::llround(s.width * 1000.0));
        if (!seen.insert(key).second) continue;
        auto z0 = sc.z0_estimate(s.cu, s.width);
        nlohmann::json e{{"cu", s.cu}, {"widthMm", s.width}};
        if (z0) e["z0Ohm"] = *z0;
        z0s.push_back(e);
    }

    return {{"faraday", "0.1.0"},
            {"board", to_json(board)},
            {"findings", fj},
            {"z0Table", z0s},
            {"meta", sc.meta()}};
}

}  // namespace faraday
