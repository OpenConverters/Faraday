#pragma once
// Faraday's neutral board IR: what every importer produces and the screener
// consumes. Geometry is in millimetres (KiCad native); physics code converts
// to ratios/SI at the point of use. The IR deliberately carries only what
// EMC analysis needs — it is not a fabrication format.
//
// This is the P0 *internal* IR. It becomes the governed BAS schema only after
// the screening engine has proven the field set (and with explicit approval).

#include "Values.hpp"

#include <nlohmann/json.hpp>
#include <map>
#include <optional>
#include <stdexcept>
#include <regex>
#include <string>
#include <vector>

namespace faraday {

struct BoardError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// "This format carries no dielectric" is a QUESTION, not a failure, and the
// one thing the asker already knows is how many copper layers the board has —
// every importer counts them before it asks. Carrying that count structurally
// (rather than leaving the UI to regex it back out of the message, which made
// every stackup-less KiCad board suggest 2-layer because the CLI usage text
// says "default-2layer") lets the caller offer the ONE builtin that fits.
struct StackupNeeded : BoardError {
    StackupNeeded(const std::string& what, int copper_count)
        : BoardError(what), copper_count(copper_count) {}
    int copper_count;
};

// ---- Stackup ----

enum class LayerKind { Copper, Dielectric };

struct StackLayer {
    LayerKind kind;
    std::string name;        // "F.Cu", "In1.Cu", "B.Cu", "dielectric 1", ...
    double thickness_mm;
    // Dielectric only. No default: a dielectric without epsilon_r is invalid
    // (the importer/CLI must supply a complete stackup or refuse).
    std::optional<double> epsilon_r;
    // Copper only: KiCad layer type hint ("signal", "power", "mixed").
    std::string copper_type;
};

struct Stackup {
    std::vector<StackLayer> layers;  // top → bottom, alternating Cu/dielectric
    std::string source;              // "board-file" | "user:<name>" — stated in reports

    // Indices into `layers` of the copper layers, top → bottom.
    std::vector<size_t> copper_indices() const {
        std::vector<size_t> out;
        for (size_t i = 0; i < layers.size(); ++i)
            if (layers[i].kind == LayerKind::Copper) out.push_back(i);
        return out;
    }

    // z of the TOP face of each copper layer, mm, z grows downward from 0.
    std::vector<double> copper_z() const {
        std::vector<double> z;
        double acc = 0.0;
        for (const auto& l : layers) {
            if (l.kind == LayerKind::Copper) z.push_back(acc);
            acc += l.thickness_mm;
        }
        return z;
    }

    // Dielectric height and effective epsilon_r between copper layers a and b
    // (copper-layer ordinals, a<b adjacent or not): series-stacked dielectrics.
    // Effective epsilon for stacked dielectrics of heights h_i: series
    // capacitor model eps_eff = (sum h_i) / (sum h_i/eps_i).
    void dielectric_between(size_t cu_a, size_t cu_b, double& height_mm,
                            double& eps_r) const {
        auto cu = copper_indices();
        if (cu_a >= cu.size() || cu_b >= cu.size() || cu_a >= cu_b)
            throw BoardError("stackup: bad copper ordinals for dielectric_between");
        double h = 0.0, h_over_eps = 0.0;
        for (size_t i = cu[cu_a] + 1; i < cu[cu_b]; ++i) {
            const auto& l = layers[i];
            if (l.kind != LayerKind::Dielectric) continue;
            if (!l.epsilon_r)
                throw BoardError("stackup: dielectric '" + l.name +
                                 "' has no epsilon_r — supply a complete stackup");
            h += l.thickness_mm;
            h_over_eps += l.thickness_mm / *l.epsilon_r;
        }
        if (h <= 0.0)
            throw BoardError("stackup: no dielectric between requested copper layers");
        height_mm = h;
        eps_r = h / h_over_eps;
    }
};

// ---- Connectivity + geometry ----

struct Net {
    int id;
    std::string name;
};

struct Segment {
    int net;
    int cu;        // copper-layer ordinal (0 = F.Cu ... last = B.Cu)
    double x1, y1, x2, y2;  // mm
    double width;           // mm
};

struct Via {
    int net;
    double x, y;      // mm
    double size, drill;  // mm
    int cu_from, cu_to;  // copper-layer ordinals spanned
};

struct Point {
    double x, y;
};

struct ZonePoly {
    int net;
    int cu;
    std::vector<Point> pts;  // filled polygon outline, mm
    // Holes in the fill (thermal reliefs, clearances). ODB++ surfaces carry
    // them explicitly; treating a cleared area as copper misclassifies a
    // holey signal layer as a plane (seen on fomu's In1), so contains()
    // honours them.
    std::vector<std::vector<Point>> holes;

    double signed_area() const {  // shoelace; sign = winding
        double a = 0.0;
        for (size_t i = 0, n = pts.size(); i < n; ++i) {
            const Point& p = pts[i];
            const Point& q = pts[(i + 1) % n];
            a += p.x * q.y - q.x * p.y;
        }
        return 0.5 * a;
    }

    static bool ring_contains(const std::vector<Point>& ring, double x,
                              double y) {  // even-odd ray cast
        bool in = false;
        for (size_t i = 0, n = ring.size(), j = n - 1; i < n; j = i++) {
            const Point& a = ring[i];
            const Point& b = ring[j];
            if ((a.y > y) != (b.y > y) &&
                x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
                in = !in;
        }
        return in;
    }

    bool contains(double x, double y) const {
        if (!ring_contains(pts, x, y)) return false;
        for (const auto& h : holes)
            if (ring_contains(h, x, y)) return false;
        return true;
    }
};

struct Pad {
    std::string component;  // footprint reference (R5, U2, ...)
    int net;                // -1 when unconnected
    double x, y;            // absolute, mm
    double w, h;            // mm (axis-aligned bbox of the pad after rotation)
    bool through_hole;
    int cu;                 // copper ordinal for SMD; -1 for through-hole
    // Pin name/number ("1", "K", "D") — last, so aggregate initializers that
    // predate it stay valid. Both KiCad and ODB++ carry it; keeping it makes
    // diode polarity and IC pin roles derivable EXACTLY instead of being
    // inferred from pad counts (Franz's Stromanalyse needs conduction paths).
    std::string pin;
};

struct Component {
    std::string reference;
    std::string footprint;  // "lib:name", or a bare package ("CC3216-1206")
    std::string value;      // e.g. "100n", "STM32F4..."
    double x, y, rot_deg;
    // The manufacturer's ordering code, when the export carried one as a
    // field of its own. Altium's ODB++ does — and writes no value at all —
    // so for those boards this is the only thing a catalogue can be asked
    // about. Never a package name and never a placeholder: see the ODB++
    // importer, which drops Altium's unresolved "=PartNumber" text. Last in
    // the struct so the five-year-old {ref, fp, value, x, y, rot} aggregate
    // initialisers in the other importers keep working.
    std::string part_number;
};

struct BoardIR {
    Stackup stackup;
    // What the CATALOGUE knows about the parts on this board, by refdes: the
    // measured ESR and ESL a layout cannot give and Faraday was assuming. Empty
    // until something identifies the parts; see values::PartData.
    std::map<std::string, values::PartData> part_data;
    std::vector<std::string> copper_names;  // ordinal → KiCad name
    std::vector<Net> nets;
    std::vector<Segment> segments;
    std::vector<Via> vias;
    std::vector<ZonePoly> zones;
    std::vector<Pad> pads;
    std::vector<Component> components;
    double bbox_x1 = 0, bbox_y1 = 0, bbox_x2 = 0, bbox_y2 = 0;  // outline bbox, mm
    bool bbox_from_outline = false;  // false → computed from geometry (reported)
    int approximated_arcs = 0;       // arcs collapsed to chords (reported, no silent caps)
    int vias_without_drill = 0;      // drill inherited from netclass (reported)
    // Gerber clear-polarity (LPC) objects skipped by the importer: treating a
    // cleared area as copper would make pours look solid where they are not,
    // so the skip is counted and surfaced instead of silent.
    int gerber_clear_skipped = 0;
    // IPC-D-356 net propagation: islands whose seeds named DIFFERENT nets
    // (majority won; the count is surfaced, never silent)
    int ipc_net_conflicts = 0;
    // Non-fatal findings from the import-time plausibility gate (see
    // Plausible.hpp). Impossible boards throw; these are the merely odd ones,
    // carried into the report so the reader sees them.
    std::vector<std::string> plausibility_notes;

    const std::string& net_name(int id) const {
        static const std::string unknown = "?";
        for (const auto& n : nets)
            if (n.id == id) return n.name;
        return unknown;
    }

    int copper_ordinal(const std::string& kicad_name) const {
        for (size_t i = 0; i < copper_names.size(); ++i)
            if (copper_names[i] == kicad_name) return static_cast<int>(i);
        return -1;
    }
};

// Fill in values the export did not carry, from a refdes->value table. Returns
// how many were applied. A value already on the board is never overwritten —
// the layout outranks a side file, always.
inline size_t apply_values(BoardIR& b, values::ValueTable& t) {
    for (auto& c : b.components) {
        auto it = t.by_refdes.find(c.reference);
        if (it == t.by_refdes.end()) continue;
        if (!c.value.empty()) { ++t.ignored; continue; }
        c.value = it->second;
        ++t.applied;
    }
    return t.applied;
}

// Is this name just the part's PACKAGE? A chip code ("0603", "1206") or a
// standard outline is a shape, not something a catalogue can look up, so it is
// never offered as a part number. Deliberately narrow — it only has to catch
// the package names EDA tools actually write; anything it misses is offered
// and answered with "no such part", which is visible, whereas a package
// wrongly kept as a part number is a lookup that quietly finds the wrong thing.
inline bool looks_like_package(const std::string& name) {
    static const std::regex chip(
        R"((^|[^0-9])(0201|0402|0603|0805|1206|1210|1218|1806|1812|2010|2512|)"
        R"(1005|1608|2012|3216|3225|4532)([^0-9]|$))");
    static const std::regex outline(
        R"((^|[^A-Za-z])(SOT-?\d+|TO-?\d+|D2?PAK|[SD]?QFN|QFP|TQFP|LQFP|SOIC|)"
        R"(SOP|TSSOP|MSOP|SSOP|DIP|BGA|MELF|DO-?\d+|SMA|SMB|SMC|TESTPOINT))",
        std::regex::icase);
    return std::regex_search(name, chip) || std::regex_search(name, outline);
}

// Components whose value the export did NOT carry, with the part number it
// carried instead — the question to ask a parts catalogue.
//
// ODB++ has a field of its own for this and the importer vets it, so it is
// believed outright. A format without one (KiCad) sometimes has the part
// number typed into the footprint slot, which is worth asking about — but
// only when the name is not simply the package.
inline std::vector<std::pair<std::string, std::string>> parts_without_values(
    const BoardIR& b) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& c : b.components) {
        if (!c.value.empty()) continue;
        if (!c.part_number.empty())
            out.push_back({c.reference, c.part_number});
        else if (!c.footprint.empty() && !looks_like_package(c.footprint))
            out.push_back({c.reference, c.footprint});
    }
    return out;
}

// ---- JSON (for the web viewer / report) ----

inline nlohmann::json to_json(const BoardIR& b) {
    nlohmann::json j;
    j["stackupSource"] = b.stackup.source;
    j["stackup"] = nlohmann::json::array();
    for (const auto& l : b.stackup.layers) {
        nlohmann::json lj{{"kind", l.kind == LayerKind::Copper ? "copper" : "dielectric"},
                          {"name", l.name},
                          {"thicknessMm", l.thickness_mm}};
        if (l.epsilon_r) lj["epsilonR"] = *l.epsilon_r;
        if (!l.copper_type.empty()) lj["copperType"] = l.copper_type;
        j["stackup"].push_back(lj);
    }
    j["copperNames"] = b.copper_names;
    j["nets"] = nlohmann::json::array();
    for (const auto& n : b.nets) j["nets"].push_back({{"id", n.id}, {"name", n.name}});
    j["segments"] = nlohmann::json::array();
    for (const auto& s : b.segments)
        j["segments"].push_back({{"net", s.net}, {"cu", s.cu},
                                 {"x1", s.x1}, {"y1", s.y1}, {"x2", s.x2}, {"y2", s.y2},
                                 {"w", s.width}});
    j["vias"] = nlohmann::json::array();
    for (const auto& v : b.vias)
        j["vias"].push_back({{"net", v.net}, {"x", v.x}, {"y", v.y},
                             {"size", v.size}, {"drill", v.drill},
                             {"cuFrom", v.cu_from}, {"cuTo", v.cu_to}});
    j["zones"] = nlohmann::json::array();
    for (const auto& z : b.zones) {
        nlohmann::json pts = nlohmann::json::array();
        for (const auto& p : z.pts) pts.push_back({p.x, p.y});
        nlohmann::json zj{{"net", z.net}, {"cu", z.cu}, {"pts", pts}};
        if (!z.holes.empty()) {
            nlohmann::json hs = nlohmann::json::array();
            for (const auto& h : z.holes) {
                nlohmann::json hp = nlohmann::json::array();
                for (const auto& p : h) hp.push_back({p.x, p.y});
                hs.push_back(hp);
            }
            zj["holes"] = hs;
        }
        j["zones"].push_back(zj);
    }
    j["pads"] = nlohmann::json::array();
    for (const auto& p : b.pads)
        j["pads"].push_back({{"component", p.component}, {"pin", p.pin},
                             {"net", p.net},
                             {"x", p.x}, {"y", p.y}, {"w", p.w}, {"h", p.h},
                             {"th", p.through_hole}, {"cu", p.cu}});
    // The parts themselves. Pads already name their component, but the
    // viewer needs the part's own facts — value, footprint, placement — to
    // draw it as a body and to ask a parts catalogue about it. What the
    // export did not carry stays empty here (an Altium ODB++ job carries a
    // part number and no value at all): the viewer says so rather than
    // inventing one.
    j["components"] = nlohmann::json::array();
    for (const auto& c : b.components)
        j["components"].push_back({{"ref", c.reference},
                                   {"footprint", c.footprint},
                                   {"partNumber", c.part_number},
                                   {"value", c.value},
                                   {"x", c.x}, {"y", c.y}, {"rot", c.rot_deg}});
    j["bbox"] = {b.bbox_x1, b.bbox_y1, b.bbox_x2, b.bbox_y2};
    j["bboxFromOutline"] = b.bbox_from_outline;
    j["approximatedArcs"] = b.approximated_arcs;
    j["viasWithoutDrill"] = b.vias_without_drill;
    j["gerberClearSkipped"] = b.gerber_clear_skipped;
    j["ipcNetConflicts"] = b.ipc_net_conflicts;
    j["plausibilityNotes"] = b.plausibility_notes;
    return j;
}

}  // namespace faraday
