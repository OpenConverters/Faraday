#pragma once
// Faraday's neutral board IR: what every importer produces and the screener
// consumes. Geometry is in millimetres (KiCad native); physics code converts
// to ratios/SI at the point of use. The IR deliberately carries only what
// EMC analysis needs — it is not a fabrication format.
//
// This is the P0 *internal* IR. It becomes the governed BAS schema only after
// the screening engine has proven the field set (and with explicit approval).

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace faraday {

struct BoardError : std::runtime_error {
    using std::runtime_error::runtime_error;
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
};

struct Component {
    std::string reference;
    std::string footprint;  // "lib:name"
    std::string value;      // e.g. "100n", "STM32F4..."
    double x, y, rot_deg;
};

struct BoardIR {
    Stackup stackup;
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
        j["pads"].push_back({{"component", p.component}, {"net", p.net},
                             {"x", p.x}, {"y", p.y}, {"w", p.w}, {"h", p.h},
                             {"th", p.through_hole}, {"cu", p.cu}});
    j["bbox"] = {b.bbox_x1, b.bbox_y1, b.bbox_x2, b.bbox_y2};
    j["bboxFromOutline"] = b.bbox_from_outline;
    j["approximatedArcs"] = b.approximated_arcs;
    j["viasWithoutDrill"] = b.vias_without_drill;
    j["gerberClearSkipped"] = b.gerber_clear_skipped;
    return j;
}

}  // namespace faraday
