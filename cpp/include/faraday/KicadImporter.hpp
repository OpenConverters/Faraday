#pragma once
// .kicad_pcb (KiCad 6+) → BoardIR.
//
// Stackup policy (no silent defaults): the board file's stackup is used when
// present; a caller-supplied stackup overrides it; if neither exists the
// import THROWS — the CLI/GUI must obtain an explicit stackup from the user.
//
// Robustness stance: unknown s-expr children are ignored (KiCad adds fields
// per version); *malformed* known fields throw. Arcs are collapsed to chords
// and counted in BoardIR::approximated_arcs (reported, never silent).

#include "BoardIR.hpp"
#include "SExpr.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace faraday {

namespace detail {

// Canonical copper order: F.Cu = 0, InN.Cu = N, B.Cu = last.
// (KiCad 9 renumbered internal layer IDs, so file-order/ID-order is not
// trustworthy across versions — the names are.)
inline int copper_sort_key(const std::string& name) {
    if (name == "F.Cu") return 0;
    if (name == "B.Cu") return 1000000;
    if (name.size() > 5 && name.compare(0, 2, "In") == 0 &&
        name.compare(name.size() - 3, 3, ".Cu") == 0)
        return std::stoi(name.substr(2, name.size() - 5));
    return -1;  // not a copper layer
}

inline Stackup parse_file_stackup(const SExpr& stackup_node) {
    Stackup s;
    s.source = "board-file";
    for (const SExpr* l : stackup_node.find_all("layer")) {
        const std::string& lname = l->atom_at(1);
        const SExpr* type = l->find("type");
        if (!type) continue;  // e.g. color-only entries
        const std::string& t = type->atom_at(1);
        if (t == "copper") {
            StackLayer sl{LayerKind::Copper, lname, l->number_of("thickness"),
                          std::nullopt, ""};
            s.layers.push_back(sl);
        } else if (t == "core" || t == "prepreg") {
            bool has_sub = false;
            for (const auto& c : l->children())
                if (c.is_atom() && c.atom() == "addsublayer") has_sub = true;
            if (has_sub)
                throw BoardError("stackup: multi-sublayer dielectric '" + lname +
                                 "' is not supported yet — supply --stackup explicitly");
            const SExpr* eps = l->find("epsilon_r");
            if (!eps)
                throw BoardError("stackup: dielectric '" + lname +
                                 "' has no epsilon_r in the board file — supply "
                                 "--stackup explicitly");
            StackLayer sl{LayerKind::Dielectric, lname, l->number_of("thickness"),
                          eps->number_at(1), ""};
            s.layers.push_back(sl);
        }
        // solder mask / silkscreen / paste entries: irrelevant to Z0 at P0 tier
    }
    return s;
}

struct XY {
    double x, y;
};

inline XY get_xy(const SExpr& parent, std::string_view child) {
    const SExpr* c = parent.find(child);
    if (!c)
        throw BoardError("kicad: missing (" + std::string(child) + " ...) in (" +
                         std::string(parent.name()) + " ...)");
    return {c->number_at(1), c->number_at(2)};
}

}  // namespace detail

inline BoardIR import_kicad(const std::string& text,
                            std::optional<Stackup> user_stackup = std::nullopt) {
    using detail::XY;
    SExpr root = SExpr::parse(text);
    if (root.name() != "kicad_pcb")
        throw BoardError("kicad: not a kicad_pcb file (root is '" +
                         std::string(root.name()) + "')");

    BoardIR b;

    // ---- copper layer table ----
    // Copper layers are the entries TYPED signal/power/mixed/jumper (non-copper
    // layers are 'user'). Names can be canonical (F.Cu/InN.Cu/B.Cu) or custom —
    // KiCad 5 renames the primary name itself ("Top", "GND", "3V3", "Bottom" on
    // power boards), while KiCad 6+ keeps the canonical name and appends the
    // user name as a 4th field. Ordering: canonical names sort by name (KiCad 9
    // renumbered ids); custom names sort by numeric id (v5 ids are 0..31 in
    // stack order).
    const SExpr* layers = root.find("layers");
    if (!layers) throw BoardError("kicad: no (layers ...) section");
    struct CuEntry { int id; std::string name, type; };
    std::vector<CuEntry> coppers;
    bool all_canonical = true;
    for (const auto& entry : layers->children()) {
        if (!entry.is_list() || entry.children().size() < 3) continue;
        const std::string& type = entry.atom_at(2);
        if (type != "signal" && type != "power" && type != "mixed" && type != "jumper")
            continue;
        const std::string& lname = entry.atom_at(1);
        coppers.push_back({static_cast<int>(entry.number_at(0)), lname, type});
        if (detail::copper_sort_key(lname) < 0) all_canonical = false;
    }
    if (coppers.empty()) throw BoardError("kicad: no copper layers in (layers ...)");
    std::sort(coppers.begin(), coppers.end(), [&](const CuEntry& a, const CuEntry& c) {
        return all_canonical ? detail::copper_sort_key(a.name) < detail::copper_sort_key(c.name)
                             : a.id < c.id;
    });
    std::vector<std::pair<std::string, std::string>> copper_types;
    for (auto& e : coppers) {
        b.copper_names.push_back(e.name);
        copper_types.emplace_back(e.name, e.type);
    }

    // ---- stackup ----
    const SExpr* setup = root.find("setup");
    const SExpr* file_stackup = setup ? setup->find("stackup") : nullptr;
    if (user_stackup) {
        b.stackup = std::move(*user_stackup);
    } else if (file_stackup) {
        b.stackup = detail::parse_file_stackup(*file_stackup);
    } else {
        throw StackupNeeded(
            "kicad: the board file carries no stackup. Faraday does not assume "
            "one — this board has " + std::to_string(b.copper_names.size()) +
            " copper layers; choose default-" +
            std::to_string(b.copper_names.size()) + "layer or supply one.",
            (int)b.copper_names.size());
    }
    size_t n_cu_stack = b.stackup.copper_indices().size();
    if (n_cu_stack != b.copper_names.size())
        throw BoardError("stackup has " + std::to_string(n_cu_stack) +
                         " copper layers but the board has " +
                         std::to_string(b.copper_names.size()));
    // copy layer-type hints onto stackup copper entries (order matches)
    {
        auto cu = b.stackup.copper_indices();
        for (size_t i = 0; i < cu.size(); ++i)
            for (auto& [lname, t] : copper_types)
                if (lname == b.copper_names[i]) b.stackup.layers[cu[i]].copper_type = t;
    }

    // ---- nets ----
    for (const SExpr* n : root.find_all("net"))
        b.nets.push_back({static_cast<int>(n->number_at(1)),
                          n->children().size() > 2 ? n->atom_at(2) : ""});

    auto cu_of = [&](const SExpr& node) -> int {
        int cu = b.copper_ordinal(node.value_of("layer"));
        if (cu < 0)
            throw BoardError("kicad: geometry on unknown copper layer '" +
                             node.value_of("layer") + "'");
        return cu;
    };
    auto net_of = [](const SExpr& node) -> int {
        const SExpr* n = node.find("net");
        return n ? static_cast<int>(n->number_at(1)) : -1;
    };

    // ---- tracks ----
    for (const SExpr* s : root.find_all("segment")) {
        XY a = detail::get_xy(*s, "start"), e = detail::get_xy(*s, "end");
        b.segments.push_back({net_of(*s), cu_of(*s), a.x, a.y, e.x, e.y,
                              s->number_of("width")});
    }
    for (const SExpr* s : root.find_all("arc")) {  // chord approximation, counted
        XY a = detail::get_xy(*s, "start"), e = detail::get_xy(*s, "end");
        b.segments.push_back({net_of(*s), cu_of(*s), a.x, a.y, e.x, e.y,
                              s->number_of("width")});
        ++b.approximated_arcs;
    }
    for (const SExpr* v : root.find_all("via")) {
        XY at = detail::get_xy(*v, "at");
        const SExpr* vl = v->find("layers");
        int f = 0, t = static_cast<int>(b.copper_names.size()) - 1;
        if (vl && vl->children().size() >= 3) {
            f = b.copper_ordinal(vl->atom_at(1));
            t = b.copper_ordinal(vl->atom_at(2));
            if (f < 0 || t < 0) throw BoardError("kicad: via on unknown layer");
            if (f > t) std::swap(f, t);
        }
        // drill may be omitted (inherited from the netclass). No rule depends
        // on it — 0 means "unspecified" and is counted for the report; the
        // renderer draws no hole rather than inventing a diameter.
        const SExpr* dr = v->find("drill");
        if (!dr) ++b.vias_without_drill;
        b.vias.push_back({net_of(*v), at.x, at.y, v->number_of("size"),
                          dr ? dr->number_at(1) : 0.0, f, t});
    }

    // ---- zones ----
    // KiCad 6+ filled_polygons carry their own (layer ...); KiCad 5 ones
    // inherit the zone's layer.
    for (const SExpr* z : root.find_all("zone")) {
        int net = net_of(*z);
        int zone_cu = -1;
        if (const SExpr* zl = z->find("layer"))
            zone_cu = b.copper_ordinal(zl->atom_at(1));
        else if (const SExpr* zls = z->find("layers"))
            for (size_t i = 1; i < zls->children().size() && zone_cu < 0; ++i)
                if (zls->children()[i].is_atom())
                    zone_cu = b.copper_ordinal(zls->children()[i].atom());
        for (const SExpr* fp : z->find_all("filled_polygon")) {
            const SExpr* layer = fp->find("layer");
            const SExpr* pts = fp->find("pts");
            if (!pts) continue;
            int cu = layer ? b.copper_ordinal(layer->atom_at(1)) : zone_cu;
            if (cu < 0) continue;  // zones on non-copper layers (keepout gfx)
            ZonePoly poly{net, cu, {}};
            for (const auto& p : pts->children()) {
                if (!p.is_list()) continue;
                if (p.name() == "xy") {
                    poly.pts.push_back({p.number_at(1), p.number_at(2)});
                } else if (p.name() == "arc") {  // outline arc → endpoints, counted
                    XY s = detail::get_xy(p, "start"), e = detail::get_xy(p, "end");
                    poly.pts.push_back({s.x, s.y});
                    poly.pts.push_back({e.x, e.y});
                    ++b.approximated_arcs;
                }
            }
            if (poly.pts.size() >= 3) b.zones.push_back(std::move(poly));
        }
    }

    // ---- footprints ("module" before KiCad 6): components + pads ----
    std::vector<const SExpr*> fps = root.find_all("footprint");
    for (const SExpr* m : root.find_all("module")) fps.push_back(m);
    for (const SExpr* f : fps) {
        Component comp;
        comp.footprint = f->children().size() > 1 && f->children()[1].is_atom()
                             ? f->children()[1].atom()
                             : "";
        const SExpr* at = f->find("at");
        if (!at) continue;
        comp.x = at->number_at(1);
        comp.y = at->number_at(2);
        comp.rot_deg = at->children().size() > 3 ? at->number_at(3) : 0.0;
        // v8/9: (property "Reference" "R5" ...); v6/7: (fp_text reference "R5" ...)
        for (const SExpr* p : f->find_all("property"))
            if (p->children().size() > 2) {
                if (p->atom_at(1) == "Reference") comp.reference = p->atom_at(2);
                if (p->atom_at(1) == "Value") comp.value = p->atom_at(2);
            }
        for (const SExpr* t : f->find_all("fp_text"))
            if (t->children().size() > 2) {
                if (t->atom_at(1) == "reference") comp.reference = t->atom_at(2);
                if (t->atom_at(1) == "value") comp.value = t->atom_at(2);
            }

        // pads: absolute position = footprint origin + offset rotated by the
        // footprint angle. KiCad's y axis points DOWN and rotations are CCW in
        // board view, so the maths uses -angle. Pad w/h swap at 90/270.
        double th = -comp.rot_deg * M_PI / 180.0;
        double c = std::cos(th), sn = std::sin(th);
        for (const SExpr* p : f->find_all("pad")) {
            const SExpr* pat = p->find("at");
            const SExpr* psz = p->find("size");
            if (!pat || !psz) continue;
            double dx = pat->number_at(1), dy = pat->number_at(2);
            double prot = pat->children().size() > 3 ? pat->number_at(3) : comp.rot_deg;
            Pad pad;
            pad.component = comp.reference;
            pad.net = net_of(*p);
            pad.x = comp.x + dx * c - dy * sn;
            pad.y = comp.y + dx * sn + dy * c;
            double w = psz->number_at(1), h = psz->number_at(2);
            bool swap = std::abs(std::fmod(std::abs(prot), 180.0) - 90.0) < 1e-6;
            pad.w = swap ? h : w;
            pad.h = swap ? w : h;
            const std::string& ptype = p->atom_at(2);  // thru_hole|smd|connect|np_thru_hole
            pad.through_hole = (ptype == "thru_hole" || ptype == "np_thru_hole");
            pad.cu = -1;
            if (!pad.through_hole) {
                const SExpr* pl = p->find("layers");
                if (pl && pl->children().size() > 1) {
                    // first copper layer listed ("F.Cu"/"B.Cu"; masks ignored)
                    for (size_t i = 1; i < pl->children().size(); ++i) {
                        int cu = b.copper_ordinal(pl->children()[i].atom());
                        if (cu >= 0) { pad.cu = cu; break; }
                    }
                }
            }
            b.pads.push_back(pad);
        }
        b.components.push_back(std::move(comp));
    }

    // ---- board outline bbox (Edge.Cuts), else geometry bbox (reported) ----
    double x1 = 1e30, y1 = 1e30, x2 = -1e30, y2 = -1e30;
    bool outline = false;
    auto grow = [&](double x, double y) {
        x1 = std::min(x1, x); y1 = std::min(y1, y);
        x2 = std::max(x2, x); y2 = std::max(y2, y);
    };
    for (const char* kind : {"gr_line", "gr_rect", "gr_arc", "gr_circle", "gr_poly"}) {
        for (const SExpr* g : root.find_all(kind)) {
            const SExpr* layer = g->find("layer");
            if (!layer || layer->atom_at(1) != "Edge.Cuts") continue;
            outline = true;
            for (const char* pt : {"start", "end", "mid", "center"}) {
                if (const SExpr* c = g->find(pt)) grow(c->number_at(1), c->number_at(2));
            }
            if (const SExpr* pts = g->find("pts"))
                for (const auto& p : pts->children())
                    if (p.is_list() && p.name() == "xy")
                        grow(p.number_at(1), p.number_at(2));
        }
    }
    if (!outline) {
        for (const auto& s : b.segments) { grow(s.x1, s.y1); grow(s.x2, s.y2); }
        for (const auto& z : b.zones)
            for (const auto& p : z.pts) grow(p.x, p.y);
        for (const auto& p : b.pads) grow(p.x, p.y);
    }
    if (x1 > x2)
        throw BoardError("kicad: board has no geometry at all");
    b.bbox_x1 = x1; b.bbox_y1 = y1; b.bbox_x2 = x2; b.bbox_y2 = y2;
    b.bbox_from_outline = outline;

    return b;
}

// A generic symmetric FR4 stackup for n copper layers, total 1.6 mm: 35 µm
// copper, dielectric heights split evenly across the n-1 gaps, eps_r 4.5.
// This is a USER-SELECTED approximation, never an implicit default — the
// caller asks for it explicitly and the report states "user:default-Nlayer".
inline Stackup generic_stackup(int n_copper) {
    if (n_copper < 2)
        throw BoardError("generic_stackup: need at least 2 copper layers");
    constexpr double kBoardMm = 1.6, kCuMm = 0.035, kEps = 4.5;
    double diel_total = kBoardMm - n_copper * kCuMm;
    if (diel_total <= 0)
        throw BoardError("generic_stackup: " + std::to_string(n_copper) +
                         " copper layers do not fit in a 1.6 mm board");
    double each = diel_total / (n_copper - 1);
    Stackup s;
    s.source = "user:default-" + std::to_string(n_copper) + "layer";
    for (int i = 0; i < n_copper; ++i) {
        std::string name = i == 0 ? "F.Cu"
                         : i == n_copper - 1 ? "B.Cu"
                         : "In" + std::to_string(i) + ".Cu";
        s.layers.push_back({LayerKind::Copper, name, kCuMm, std::nullopt, "signal"});
        if (i < n_copper - 1)
            s.layers.push_back({LayerKind::Dielectric,
                                "dielectric " + std::to_string(i + 1), each, kEps, ""});
    }
    return s;
}

// Explicit built-in stackups — only ever selected BY THE USER (CLI flag /
// GUI card), never applied implicitly. The 2- and 4-layer entries use real
// asymmetric prepreg/core geometry; "default-Nlayer" for any other n falls to
// the generic symmetric stack above.
inline Stackup builtin_stackup(const std::string& name) {
    auto cu = [](const char* n) {
        return StackLayer{LayerKind::Copper, n, 0.035, std::nullopt, "signal"};
    };
    auto diel = [](const char* n, double t, double eps) {
        return StackLayer{LayerKind::Dielectric, n, t, eps, ""};
    };
    Stackup s;
    s.source = "user:" + name;
    if (name == "default-2layer") {
        s.layers = {cu("F.Cu"), diel("core", 1.51, 4.5), cu("B.Cu")};
        return s;
    }
    if (name == "default-4layer") {
        s.layers = {cu("F.Cu"),    diel("prepreg 1", 0.2, 4.4),
                    cu("In1.Cu"),  diel("core", 1.065, 4.5),
                    cu("In2.Cu"),  diel("prepreg 2", 0.2, 4.4),
                    cu("B.Cu")};
        return s;
    }
    // default-Nlayer for any other N
    if (name.rfind("default-", 0) == 0 &&
        name.size() > 13 && name.compare(name.size() - 5, 5, "layer") == 0) {
        const std::string digits = name.substr(8, name.size() - 13);
        if (!digits.empty() &&
            digits.find_first_not_of("0123456789") == std::string::npos)
            return generic_stackup(std::stoi(digits));
    }
    throw BoardError("unknown built-in stackup '" + name +
                     "' (default-2layer | default-4layer | default-<N>layer)");
}

}  // namespace faraday
