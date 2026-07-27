#pragma once
// Whole-board EMC screening: pure computational geometry + the closed forms
// in Tline.hpp. No field solving here — this tier finds and RANKS risk, and
// selected pairs graduate to the FEA/SPICE tier.
//
// Rules implemented (P0):
//   coupled-run        parallel segments, same layer (edge) or adjacent signal
//                      layers (broadside), scored by saturated-NEXT estimate
//   3w                 minimum edge separation < 2×width on the wider trace
//   plane-crossing     signal run over a void/split in its reference plane
//   no-reference-plane copper layer carrying signals with no plane to return on
//   coverage notes     approximated arcs, missing outline, dropped findings
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

struct SegRef {
    size_t idx;                      // index into BoardIR::segments
    double x1, y1, x2, y2, w, len;
    double ux, uy;                   // unit direction
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

class Screener {
  public:
    Screener(const BoardIR& board, ScreenerParams params = {})
        : b_(board), p_(params) {
        build_layer_models();
    }

    const std::vector<LayerModel>& layer_models() const { return layers_; }

    std::vector<Finding> run() {
        std::vector<Finding> out;
        find_no_reference_plane(out);
        find_coupled_runs(out);
        find_plane_crossings(out);
        // rank: severity desc, then coupled length desc
        std::sort(out.begin(), out.end(), [](const Finding& x, const Finding& y) {
            if (x.severity != y.severity) return x.severity > y.severity;
            return x.coupled_len_mm > y.coupled_len_mm;
        });
        dropped_by_cap_ = 0;
        if (out.size() > p_.max_findings) {
            dropped_by_cap_ = out.size() - p_.max_findings;
            out.resize(p_.max_findings);
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
                              {"zoneCoverage", layers_[i].zone_coverage}});
        return {{"planes", planes},
                {"approximatedArcs", b_.approximated_arcs},
                {"bboxFromOutline", b_.bbox_from_outline},
                {"stackupSource", b_.stackup.source},
                {"droppedBelowFloorDb", dropped_below_floor_},
                {"droppedByFindingCap", dropped_by_cap_},
                {"reportFloorDb", p_.report_floor_db}};
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
        for (size_t i = 0; i < n; ++i) {
            if (layers_[i].is_plane) continue;
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
    }

    double ref_height(int cu) const {  // nearest reference height, 0 when none
        const LayerModel& lm = layers_[cu];
        if (lm.ref_up >= 0 && lm.ref_dn >= 0) return std::min(lm.h_up, lm.h_dn);
        if (lm.ref_up >= 0) return lm.h_up;
        if (lm.ref_dn >= 0) return lm.h_dn;
        return 0.0;
    }

    bool is_pour_net(int net) const {
        for (const auto& lm : layers_)
            if (lm.is_plane && lm.plane_net == net) return true;
        return false;
    }

    // ---- rule: layers carrying signals with no reference plane ----
    void find_no_reference_plane(std::vector<Finding>& out) {
        std::vector<double> len_by_cu(layers_.size(), 0.0);
        for (const auto& s : b_.segments)
            len_by_cu[s.cu] += std::hypot(s.x2 - s.x1, s.y2 - s.y1);
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
        for (size_t i = 0; i < b_.segments.size(); ++i) {
            const Segment& s = b_.segments[i];
            if (s.net <= 0 || layers_[s.cu].is_plane || is_pour_net(s.net)) continue;
            double dx = s.x2 - s.x1, dy = s.y2 - s.y1;
            double len = std::hypot(dx, dy);
            if (len < 1e-6) continue;
            SegRef r{i, s.x1, s.y1, s.x2, s.y2, s.width, len, dx / len, dy / len};
            refs[s.cu].push_back(r);
        }
        for (size_t cu = 0; cu < n_cu; ++cu)
            for (size_t k = 0; k < refs[cu].size(); ++k) grids[cu].insert(refs[cu][k], k);

        std::map<PairKey, PairAccum> pairs;

        auto consider = [&](const SegRef& a, int cu_a, const SegRef& sb, int cu_b,
                            bool broadside) {
            const Segment& sa = b_.segments[a.idx];
            const Segment& sbg = b_.segments[sb.idx];
            if (sa.net == sbg.net) return;
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
            PairKey key{std::min(sa.net, sbg.net), std::max(sa.net, sbg.net),
                        std::min(cu_a, cu_b), std::max(cu_a, cu_b)};
            PairAccum& acc = pairs[key];
            acc.len += ov->length;
            acc.len_x_d += ov->length * ov->center_d;
            double edge_sep = ov->center_d - 0.5 * (sa.width + sbg.width);
            acc.min_edge_sep = std::min(acc.min_edge_sep, edge_sep);
            acc.max_w = std::max({acc.max_w, sa.width, sbg.width});
            acc.worst_k = std::max(acc.worst_k, k);
            acc.have_h = acc.have_h || have_h;
            Segment ga = ov->span_a; ga.net = sa.net; ga.cu = sa.cu;
            Segment gb = ov->span_b; gb.net = sbg.net; gb.cu = sbg.cu;
            acc.geom.lines.push_back(ga);
            acc.geom.lines.push_back(gb);
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
            std::string kind = broadside ? "broadside" : "edge";
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
            f.title = b_.net_name(f.net_a) + " <-> " + b_.net_name(f.net_b) +
                      " on " + where;
            f.detail = std::string(buf) +
                       ". Worst-case (length-saturated) near-end coupling; the "
                       "estimate carries roughly +/-6 dB — treat as a rank, "
                       "confirm with the field-solver tier. Mean centre "
                       "separation " + std::to_string(mean_d).substr(0, 5) + " mm.";
            f.remediation = broadside
                ? "Offset the runs laterally, route orthogonally on adjacent "
                  "layers, or move one net to a layer across a plane."
                : "Increase the gap (3W rule), shorten the parallel run, or "
                  "drop a grounded guard trace with stitching vias.";
            f.severity_label = f.severity > 0.66 ? "high"
                              : f.severity > 0.33 ? "medium" : "low";
            // 3W companion finding when violated
            if (acc.min_edge_sep < 2.0 * acc.max_w && !broadside) {
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

        // accumulate uncovered length per net (marker points capped per finding)
        struct Gap { double len = 0; std::vector<Point> pts; int cu = -1; int plane = -1; };
        std::map<int, Gap> gaps;
        for (const auto& s : b_.segments) {
            if (s.net <= 0 || layers_[s.cu].is_plane || is_pour_net(s.net)) continue;
            const LayerModel& lm = layers_[s.cu];
            int plane = lm.ref_up >= 0 ? lm.ref_up : lm.ref_dn;
            if (plane < 0) continue;  // handled by no-reference-plane
            double len = std::hypot(s.x2 - s.x1, s.y2 - s.y1);
            int steps = std::max(1, (int)(len / p_.sample_step_mm));
            for (int i = 0; i <= steps; ++i) {
                double t = (double)i / steps;
                double x = s.x1 + (s.x2 - s.x1) * t, y = s.y1 + (s.y2 - s.y1) * t;
                if (!covered(plane, x, y)) {
                    Gap& g = gaps[s.net];
                    g.len += len / steps;
                    g.cu = s.cu;
                    g.plane = plane;
                    if (g.pts.size() < 64) g.pts.push_back({x, y});
                }
            }
        }
        for (auto& [net, g] : gaps) {
            if (g.len < p_.sample_step_mm) continue;  // sub-pitch noise
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
