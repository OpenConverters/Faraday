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
    // net -> components touching it
    std::map<int, std::set<std::string>> net;
    // centroid per component, for compactness tie-breaks
    std::map<std::string, Point> pos;

    explicit Graph(const BoardIR& b) {
        std::map<std::string, Point> sum;
        std::map<std::string, int> cnt;
        for (const auto& p : b.pads) {
            if (p.component.empty()) continue;
            if (p.net > 0) {
                comp[p.component][p.net]++;
                net[p.net].insert(p.component);
            }
            sum[p.component].x += p.x;
            sum[p.component].y += p.y;
            cnt[p.component]++;
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

    // Conduction path of a switching device: exactly one 1-pad net (the
    // control terminal) and at least two multi-pad nets (the path). Fails —
    // deliberately — on SOT-23 small-signals (three 1-pad nets) and on
    // transformers (many multi-pad nets are windings, not a path).
    std::optional<std::pair<int, int>> conduction(const std::string& ref) const {
        auto it = comp.find(ref);
        if (it == comp.end()) return std::nullopt;
        std::vector<int> one, multi;
        for (const auto& [n, k] : it->second)
            (k == 1 ? one : multi).push_back(n);
        if (one.size() != 1 || multi.size() != 2) return std::nullopt;
        return std::make_pair(multi[0], multi[1]);
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
    const Dev* sw = nullptr;
    for (const auto& d : devs)
        if (d.is_switch && (!sw || g.comp.at(d.ref).size() <
                                       g.comp.at(sw->ref).size()))
            sw = &d;
    if (!sw) return std::nullopt;
    const Point at = g.pos.count(sw->ref) ? g.pos.at(sw->ref) : Point{0, 0};

    // ---- shape A: a second conducting device + a chain between far rails --
    for (const auto& d : devs) {
        if (d.ref == sw->ref || d.far == sw->far) continue;
        auto chain = detail::cap_chain(g, sw->far, d.far, at);
        if (!chain || chain->empty()) continue;
        DerivedMesh m;
        m.shape = "two-device";
        m.sw_ref = sw->ref;
        m.members = {sw->ref, d.ref};
        m.members.insert(m.members.end(), chain->begin(), chain->end());
        m.chain = *chain;
        return m;
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
