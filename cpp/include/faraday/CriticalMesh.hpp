#pragma once
// The critical mesh, derived — Franz §4.4 (Holst's Stromumschaltanalyse) run
// on the netlist that is already inside the layout.
//
// The method: draw the load current's closed circulation BEFORE the switch
// toggles (Umlauf 1) and AFTER (Umlauf 2); the closed loop through the
// branches traversed in only ONE of the two is the critical mesh — the only
// copper carrying the current STEP. Shared branches (the winding, the input
// chain) carry continuous current and cancel out of the XOR.
//
// What the layout provides: the full component x net incidence (the
// schematic's connectivity graph), and — with pad counts per net — the
// conduction path of power switches: the control terminal is the net where
// the device has exactly ONE pad, the conduction path joins its multi-pad
// nets. Validated on real boards (SI7852DP, SI4850EY, SI4848DY all correctly
// role-assigned; transformers and SOT-23 small-signals correctly rejected).
//
// What it refuses: a device whose roles cannot be inferred, or a circulation
// that does not close, produces NO derived mesh — the caller keeps its
// geometric fallback. A guessed critical mesh would be worse than none.
//
// Two trace shapes, tried in order:
//   A "two-device node" (buck, boost, synchronous anything, bridge legs):
//     two conducting elements share the switch node — a switch and its
//     freewheel diode, or both FETs of a half bridge. Umlauf 1 conducts
//     through one, Umlauf 2 through the other; the XOR is device 1 +
//     device 2 + the capacitor chain joining their far rails. For a BOOST
//     that chain lands on the OUTPUT capacitor — which is Franz's own
//     example of what pattern-matching gets wrong.
//   B "magnetic node" (flyback primary, any winding + clamp): Umlauf 1 =
//     winding + switch + input chain; Umlauf 2 = winding + the clamp path
//     the winding current takes when the switch opens; XOR.

#include "BoardIR.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace faraday::mesh {

struct DerivedMesh {
    std::vector<std::string> members;   // refs of the XOR branches
    std::vector<std::string> chain;     // caps of the Umlauf-1 closing chain
    std::string shape;                  // "two-device" | "magnetic-clamp"
    std::string sw_ref;                 // the switching device
};

namespace detail {

inline std::string prefix(const std::string& ref) {
    std::string p;
    for (char c : ref) {
        if (!std::isalpha(static_cast<unsigned char>(c))) break;
        p.push_back((char)std::toupper((unsigned char)c));
    }
    return p;
}

struct Graph {
    // component -> (net -> pads on that net)
    std::map<std::string, std::map<int, int>> comp;
    // component -> (net -> total pad AREA on that net, mm^2). Pad area is
    // the package-agnostic role signal: a power FET's conduction terminals
    // are large thermal pads, its gate is tiny — and a SOT-23 signal
    // transistor's equal pads correctly fail the ratio test.
    std::map<std::string, std::map<int, double>> area;
    // net -> components touching it
    std::map<int, std::set<std::string>> net;
    // centroid per component, for compactness tie-breaks
    std::map<std::string, Point> pos;
    // every pad position per component — loop-area scoring needs the pads'
    // EXTENT (centroids degenerate: a far-away cap collinear with the two
    // FETs scores a near-zero triangle while the real loop is huge)
    std::map<std::string, std::vector<Point>> padpts;

    // net owning the pad NAMED "1" — the JEDEC control terminal of 3-lead
    // transistor packages (TO-220/TO-247/DPAK/SOT-223: gate or base is
    // pin 1). Only meaningful when pin names survived the import.
    std::map<std::string, int> pin1;

    explicit Graph(const BoardIR& b) {
        std::map<std::string, Point> sum;
        std::map<std::string, int> cnt;
        for (const auto& p : b.pads) {
            if (p.component.empty()) continue;
            if (p.net > 0) {
                comp[p.component][p.net]++;
                area[p.component][p.net] += std::max(p.w * p.h, 0.01);
                net[p.net].insert(p.component);
                if (p.pin == "1") pin1[p.component] = p.net;
            }
            sum[p.component].x += p.x;
            sum[p.component].y += p.y;
            cnt[p.component]++;
            padpts[p.component].push_back({p.x, p.y});
        }
        for (auto& [r, s] : sum)
            pos[r] = {s.x / cnt[r], s.y / cnt[r]};
    }

    // 2-terminal element of one of the given prefixes: its two nets
    std::optional<std::pair<int, int>> two_terminal(
        const std::string& ref, const char* prefixes) const {
        const std::string pre = prefix(ref);
        if (std::string(prefixes).find("|" + pre + "|") == std::string::npos)
            return std::nullopt;
        auto it = comp.find(ref);
        if (it == comp.end() || it->second.size() != 2) return std::nullopt;
        auto a = it->second.begin();
        auto b = std::next(a);
        return std::make_pair(a->first, b->first);
    }

    // Conduction path of a switching device. Three package signatures, all
    // decided by pad COUNT and pad AREA — never by name:
    //   a) gate = the one 1-pad net, path = the two multi-pad nets
    //      (multi-pad packages with both terminals split);
    //   b) one terminal is a single LARGE pad (PowerPAK drain tab as one
    //      pad): two 1-pad nets, one multi-pad — the big 1-pad net joins
    //      the path, the small one is the gate;
    //   c) all three nets single pads (D2PAK/SOT-223: gate, source, tab):
    //      the two largest are the path, the smallest the gate.
    // b) and c) demand a clear 3x area separation from the gate — a SOT-23
    // signal transistor's equal pads fail it, which is exactly right.
    std::optional<std::pair<int, int>> conduction(const std::string& ref) const {
        auto it = comp.find(ref);
        if (it == comp.end()) return std::nullopt;
        const auto& ar = area.at(ref);
        std::vector<int> one, multi;
        for (const auto& [n, k] : it->second)
            (k == 1 ? one : multi).push_back(n);
        if (one.size() == 1 && multi.size() == 2)
            return std::make_pair(multi[0], multi[1]);
        // d) area tie-break failed but the pads carry PIN NAMES and the
        //    device has exactly three nets: pin 1 is the control terminal
        //    of every JEDEC 3-lead transistor package (TO-220/TO-247/
        //    DPAK/SOT-223 — gate or base), so the other two nets are the
        //    path. This is what derives through-hole TO-220 converters
        //    (mppt-2420-hc), whose three equal pads defeat the area test.
        auto by_pin1 = [&]() -> std::optional<std::pair<int, int>> {
            if (it->second.size() != 3) return std::nullopt;
            auto p1 = pin1.find(ref);
            if (p1 == pin1.end()) return std::nullopt;
            std::vector<int> path;
            for (const auto& [n, k] : it->second)
                if (n != p1->second) path.push_back(n);
            if (path.size() != 2) return std::nullopt;
            // power packages only: a TO-220/SOT-223 lead pad is >= 2 mm^2,
            // a SOT-23 small-signal's ~0.6 mm^2 — without this floor every
            // 3-pin transistor with numbered pins would "infer", and the
            // PoE board's gate-drive SOT-23s hijacked the mesh
            if (ar.at(path[0]) < 2.0 || ar.at(path[1]) < 2.0)
                return std::nullopt;
            return std::make_pair(path[0], path[1]);
        };
        if (one.size() == 2 && multi.size() == 1) {
            const double a0 = ar.at(one[0]), a1 = ar.at(one[1]);
            const int big = a0 > a1 ? one[0] : one[1];
            if (std::max(a0, a1) < 3.0 * std::min(a0, a1))
                return by_pin1();
            return std::make_pair(multi[0], big);
        }
        if (one.size() == 3 && multi.empty()) {
            std::vector<std::pair<double, int>> byarea;
            for (int n : one) byarea.push_back({ar.at(n), n});
            std::sort(byarea.begin(), byarea.end());
            if (byarea[1].first < 3.0 * byarea[0].first) return by_pin1();
            return std::make_pair(byarea[1].second, byarea[2].second);
        }
        return std::nullopt;
    }
};

// capacitor chain u <-> v of at most two caps; returns refs, smallest first
// by total distance to `near` (the loop wants to be compact)
inline std::optional<std::vector<std::string>> cap_chain(
    const Graph& g, int u, int v, const Point& near) {
    if (u == v) return std::vector<std::string>{};
    std::optional<std::vector<std::string>> best;
    double best_d = 1e30;
    auto dist = [&](const std::string& r) {
        auto it = g.pos.find(r);
        return it == g.pos.end()
                   ? 1e6
                   : std::hypot(it->second.x - near.x, it->second.y - near.y);
    };
    auto it = g.net.find(u);
    if (it == g.net.end()) return std::nullopt;
    for (const auto& c1 : it->second) {
        auto e1 = g.two_terminal(c1, "|C|");
        if (!e1) continue;
        const int w = e1->first == u ? e1->second : e1->first;
        if (w == v) {
            const double d = dist(c1);
            if (d < best_d) { best_d = d; best = {{c1}}; }
            continue;
        }
        // second hop
        auto jt = g.net.find(w);
        if (jt == g.net.end()) continue;
        for (const auto& c2 : jt->second) {
            if (c2 == c1) continue;
            auto e2 = g.two_terminal(c2, "|C|");
            if (!e2) continue;
            const int x = e2->first == w ? e2->second : e2->first;
            if (x != v) continue;
            const double d = dist(c1) + dist(c2) + 10.0;  // prefer 1 hop
            if (d < best_d) { best_d = d; best = {{c1, c2}}; }
        }
    }
    return best;
}

// EVERY capacitor chain u <-> v (single caps and 2-cap chains), so a caller
// can score the LOOPS they close instead of trusting one distance metric
inline std::vector<std::vector<std::string>> cap_chains(const Graph& g, int u,
                                                        int v) {
    std::vector<std::vector<std::string>> out;
    if (u == v) return out;
    auto it = g.net.find(u);
    if (it == g.net.end()) return out;
    for (const auto& c1 : it->second) {
        auto e1 = g.two_terminal(c1, "|C|");
        if (!e1) continue;
        const int w = e1->first == u ? e1->second : e1->first;
        if (w == v) { out.push_back({c1}); continue; }
        auto jt = g.net.find(w);
        if (jt == g.net.end()) continue;
        for (const auto& c2 : jt->second) {
            if (c2 == c1) continue;
            auto e2 = g.two_terminal(c2, "|C|");
            if (!e2) continue;
            const int x = e2->first == w ? e2->second : e2->first;
            if (x == v) out.push_back({c1, c2});
        }
    }
    return out;
}

// area of the convex hull of the members' PADS — a format-independent proxy
// for the loop the members enclose (pure geometry, never iteration order).
// Pads, not centroids: centroids degenerate when a far cap sits collinear
// with the two switches (mppt-2420-lc's C2, 14 mm out but 0.25 mm off the
// Q1-Q4 line, scored ~1 mm^2 while the real loop is the largest on the net).
inline double member_hull_area(const Graph& g,
                               const std::vector<std::string>& refs) {
    std::vector<Point> pts;
    for (const auto& r : refs) {
        auto it = g.padpts.find(r);
        if (it != g.padpts.end())
            pts.insert(pts.end(), it->second.begin(), it->second.end());
    }
    if (pts.size() < 3) return 0.0;
    std::sort(pts.begin(), pts.end(), [](const Point& a, const Point& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    auto cross = [](const Point& o, const Point& a, const Point& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    const int n = (int)pts.size();
    std::vector<Point> h(2 * n);
    int k = 0;
    for (int i = 0; i < n; ++i) {                       // lower hull
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; --i) {       // upper hull
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    h.resize(k);
    double a = 0;
    for (int i = 0; i + 1 < (int)h.size(); ++i)
        a += h[i].x * h[i + 1].y - h[i + 1].x * h[i].y;
    return std::abs(a) / 2.0;
}

}  // namespace detail

// Derive the critical mesh around sw_net. sw_prefix is the board's switching
// designator convention (decided by the screener); std::nullopt when no
// circulation closes with inferable roles.
inline std::optional<DerivedMesh> derive(const BoardIR& b, int sw_net,
                                         const std::string& sw_prefix) {
    detail::Graph g(b);
    auto nit = g.net.find(sw_net);
    if (nit == g.net.end()) return std::nullopt;

    // conducting elements ON the node: switches with inferable paths, and
    // 2-terminal power diodes (the async freewheel / boost rectifier)
    struct Dev { std::string ref; int far; bool is_switch; };
    std::vector<Dev> devs;
    for (const auto& r : nit->second) {
        const std::string pre = detail::prefix(r);
        if (pre == sw_prefix) {
            auto c = g.conduction(r);
            if (!c) continue;
            if (c->first == sw_net) devs.push_back({r, c->second, true});
            else if (c->second == sw_net) devs.push_back({r, c->first, true});
        } else if (pre == "D") {
            auto e = g.two_terminal(r, "|D|");
            if (!e) continue;
            devs.push_back({r, e->first == sw_net ? e->second : e->first, false});
        }
    }
    // SWITCHES before diodes, and sorted BEFORE any pointer is taken into
    // the vector: a sort after &devs[i] silently re-aims the pointer — on
    // the PoE board it turned the chosen switch into the clamp diode and
    // killed every derivation on the board.
    auto total_area = [&](const std::string& ref) {
        double a = 0;
        for (const auto& [n, v] : g.area.at(ref)) a += v;
        return a;
    };
    // FULLY deterministic order: area ties break on the ref name, never on
    // pad iteration order — VESC's H1 half-bridge has two equal-area FETs,
    // and the KiCad and ODB++ exports of the SAME board delivered them in
    // different pad order, anchoring the mesh on different devices.
    // areas QUANTIZED to 0.1 mm^2 before comparing: different exporters of
    // the same board render pad sizes with sub-0.1 mm^2 differences (ODB++
    // surface vs KiCad rect), and those decimals must never out-vote the
    // ref-name tie-break — "Q1 + Q2" and "Q2 + Q1" are spurious diffs to
    // the revision-diff gate.
    auto qarea = [&](const std::string& ref) {
        return (long long)std::llround(total_area(ref) * 10.0);
    };
    std::stable_sort(devs.begin(), devs.end(),
                     [&](const Dev& a, const Dev& b) {
                         if (a.is_switch != b.is_switch)
                             return a.is_switch > b.is_switch;
                         const long long aa = qarea(a.ref), ab = qarea(b.ref);
                         if (aa != ab) return aa > ab;
                         return a.ref < b.ref;
                     });
    // the switch = the LARGEST switching device on the node (total pad
    // area): the power FET, never a gate-drive small-signal that happened
    // to infer. Map/set ordering must never decide a mesh.
    const Dev* sw = nullptr;
    for (const auto& d : devs)
        if (d.is_switch) { sw = &d; break; }
    if (!sw) return std::nullopt;
    const Point at = g.pos.count(sw->ref) ? g.pos.at(sw->ref) : Point{0, 0};

    // ---- shape A: a second conducting device + a chain between far rails --
    // A far rail may reach the chain through a CURRENT-SENSE SHUNT: on a
    // three-phase drive the low FET's source often lands on a small Kelvin
    // net, then one (or several parallel) shunt resistors to ground — and
    // those shunts are INSIDE the hot loop, which is exactly why Kelvin
    // sensing exists. Guarded: the bridged net must be SMALL (a sense net,
    // <= 6 members), and a direct capacitor chain always wins first.
    auto shunt_bridges = [&](int far)
        -> std::vector<std::pair<std::vector<std::string>, int>> {
        std::vector<std::pair<std::vector<std::string>, int>> out;
        auto it = g.net.find(far);
        if (it == g.net.end() || it->second.size() > 6) return out;
        std::map<int, std::vector<std::string>> by_dest;
        for (const auto& rr : it->second) {
            if (detail::prefix(rr) != "R") continue;
            const auto& nets = g.comp.at(rr);
            if (nets.size() < 2 || nets.size() > 4) continue;
            // the shunt's current path = its LARGEST-pad other net. A
            // 4-terminal Kelvin shunt has two big current pads and two
            // small sense pads; a plain 2-terminal R has one choice.
            int dest = -1;
            double best_a = 0;
            for (const auto& [n, k] : nets) {
                if (n == far) continue;
                const double a = g.area.at(rr).at(n);
                if (a > best_a) { best_a = a; dest = n; }
            }
            if (dest >= 0 && dest != sw_net) by_dest[dest].push_back(rr);
        }
        for (auto& [dest, rs] : by_dest) out.push_back({rs, dest});
        return out;
    };
    // The commutation current takes the LEAST-INDUCTANCE loop, so among
    // every closable candidate the SMALLEST enclosed area wins — scored on
    // the hull of the member centroids, which is pure geometry. Scoring by
    // distance-to-the-switch let pad ITERATION order decide instead: the
    // KiCad and ODB++ exports of the SAME VESC picked different input caps
    // (C8, 277 mm^2 vs C40, 221 mm^2) for the identical half-bridge.
    for (int pass = 0; pass < 2; ++pass) {
        std::optional<DerivedMesh> best;
        double best_area = 1e30;
        std::string best_key;
        for (const auto& d : devs) {
            if (d.ref == sw->ref || d.far == sw->far) continue;
            const int far_a = sw->far, far_b = d.far;
            // candidate (extra-members, far_a, far_b) triples: the direct
            // pair on pass 0; every shunt-bridged variant on pass 1
            std::vector<std::tuple<std::vector<std::string>, int, int>> cands;
            if (pass == 0) {
                cands.push_back({{}, far_a, far_b});
            } else {
                for (auto& [rs, dest] : shunt_bridges(far_b))
                    cands.push_back({rs, far_a, dest});
                for (auto& [rs, dest] : shunt_bridges(far_a))
                    cands.push_back({rs, dest, far_b});
            }
            for (auto& [ex, fa, fb] : cands) {
                for (const auto& chain : detail::cap_chains(g, fa, fb)) {
                    if (chain.empty()) continue;
                    DerivedMesh m;
                    m.shape = "two-device";
                    m.sw_ref = sw->ref;
                    m.members = {sw->ref, d.ref};
                    m.members.insert(m.members.end(), ex.begin(), ex.end());
                    m.members.insert(m.members.end(), chain.begin(),
                                     chain.end());
                    m.chain = chain;
                    const double area =
                        detail::member_hull_area(g, m.members);
                    std::string key;
                    for (const auto& r : m.members) key += r + "|";
                    if (area < best_area - 1e-9 ||
                        (std::abs(area - best_area) <= 1e-9 &&
                         key < best_key)) {
                        best_area = area;
                        best_key = key;
                        best = std::move(m);
                    }
                }
            }
        }
        if (best) return best;
    }

    // ---- shape B: winding on the node + clamp path -----------------------
    // magnetics: L (2-terminal) or a transformer winding (T/TR with >= 4
    // pads); every other net of the part is a winding-partner candidate
    for (const auto& r : nit->second) {
        const std::string pre = detail::prefix(r);
        const bool is_mag =
            (pre == "L" && g.comp.at(r).size() == 2) ||
            ((pre == "T" || pre == "TR") &&
             [&] {
                 int pads = 0;
                 for (const auto& [n, k] : g.comp.at(r)) pads += k;
                 return pads >= 4;
             }());
        if (!is_mag) continue;
        for (const auto& [p_net, k] : g.comp.at(r)) {
            if (p_net == sw_net) continue;
            // Umlauf 1 must close: winding top -> chain -> switch return
            auto chain1 = detail::cap_chain(g, p_net, sw->far, at);
            if (!chain1) continue;
            // Umlauf 2: clamp path sw_net -> X (<= 2 D/R edges, not the
            // switch), then X closes to the winding top through caps
            auto jt = g.net.find(sw_net);
            for (const auto& e1 : jt->second) {
                if (e1 == sw->ref || e1 == r) continue;
                auto ed1 = g.two_terminal(e1, "|D|R|");
                if (!ed1) continue;
                const int x1 = ed1->first == sw_net ? ed1->second : ed1->first;
                // one-edge clamp landing on a closable net?
                std::vector<std::pair<std::vector<std::string>, int>> lands;
                auto c1 = detail::cap_chain(g, x1, p_net, at);
                if (c1) lands.push_back({{e1}, x1});
                // two-edge clamp (R then D of an RCD)
                auto kt = g.net.find(x1);
                if (kt != g.net.end())
                    for (const auto& e2 : kt->second) {
                        if (e2 == e1 || e2 == sw->ref) continue;
                        auto ed2 = g.two_terminal(e2, "|D|R|");
                        if (!ed2) continue;
                        // a two-edge clamp must contain a DIODE: R+R is a
                        // divider, not a commutation path (a lone R stays
                        // legal above — that is an RC snubber)
                        if (detail::prefix(e1) == "R" &&
                            detail::prefix(e2) == "R")
                            continue;
                        const int x2 =
                            ed2->first == x1 ? ed2->second : ed2->first;
                        if (x2 == sw_net) continue;
                        auto c2 = detail::cap_chain(g, x2, p_net, at);
                        if (c2) lands.push_back({{e1, e2}, x2});
                    }
                for (auto& [fw, x] : lands) {
                    auto closure = detail::cap_chain(g, x, p_net, at);
                    if (!closure) continue;
                    // XOR of the two circulations: winding cancels; the
                    // chains cancel where identical
                    std::set<std::string> u1(chain1->begin(), chain1->end());
                    u1.insert(sw->ref);
                    std::set<std::string> u2(closure->begin(), closure->end());
                    for (const auto& f : fw) u2.insert(f);
                    std::vector<std::string> xr;
                    for (const auto& s : u1)
                        if (!u2.count(s)) xr.push_back(s);
                    for (const auto& s : u2)
                        if (!u1.count(s)) xr.push_back(s);
                    if (xr.size() < 2) continue;
                    DerivedMesh m;
                    m.shape = "magnetic-clamp";
                    m.sw_ref = sw->ref;
                    m.members = xr;
                    m.chain = *chain1;
                    return m;
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace faraday::mesh
