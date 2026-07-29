#pragma once
// Gerber X2 set importer: one file per layer, X2 attributes carrying the two
// things a bare Gerber lacks — WHICH layer a file is (%TF.FileFunction) and
// WHICH net an object belongs to (%TO.N), plus component refs (%TO.C).
//
// Plain RS-274X has no netlist, and most of Faraday's rules are net-aware:
// coupled-run must exclude same-net pairs, plane classification needs the
// pour's net, the switch-node rule needs components. So a set without X2 net
// attributes is REFUSED with the fix stated (enable X2 in the CAM export —
// KiCad has emitted it by default for years), rather than analysed into
// confident nonsense.
//
// WHAT IS SUPPORTED. Linear draws (G01/D01), flashes (D03), regions
// (G36/G37 → pours), circle and rectangle apertures, arcs chord-approximated
// and counted, MM and IN units, Excellon drills in decimal format, and the
// board outline from the Profile file. Through vias only: a drill set with
// blind/buried spans carries them in per-file layer pairs this importer does
// not read yet — stated, not guessed. Clear-polarity (LPC) objects are
// SKIPPED AND COUNTED: treating a cleared area as copper would make pours
// look solid where they are not, so the count is surfaced for the reader.
//
// Stackup: a Gerber set carries no thicknesses and no permittivity, so the
// importer refuses without one, exactly like the KiCad path — the error names
// the copper count so the UI can offer the right default-<N>layer.

#include "BoardIR.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace faraday::gerber {

struct NamedFile {
    std::string name;
    std::string text;
};

// ---------------------------------------------------------------------------
// One parsed Gerber layer
// ---------------------------------------------------------------------------

struct GTrack { double x1, y1, x2, y2, w; std::string net; };
struct GFlash { double x, y, w, h; std::string net, comp; };
struct GRegion { std::vector<Point> pts; std::string net; };

struct GerberLayer {
    std::string function;        // "Copper", "Profile", "Soldermask", ...
    int copper_index = -1;       // L<n> for Copper
    std::string side;            // Top | Inr | Bot
    std::vector<GTrack> tracks;
    std::vector<GFlash> flashes;
    std::vector<GRegion> regions;
    int arcs_approximated = 0;
    int clear_skipped = 0;
    bool has_net_attrs = false;
};

namespace detail {

struct Aperture { char kind = 'C'; double a = 0, b = 0; };   // dims in mm

inline double num(const std::string& s) { return std::atof(s.c_str()); }

// A Gerber coordinate: fixed-format integer scaled by the FS declaration.
inline double coord(const std::string& digits, int dec, double unit) {
    if (digits.empty()) return 0.0;
    return std::atof(digits.c_str()) / std::pow(10.0, dec) * unit;
}

}  // namespace detail

inline GerberLayer parse_gerber(const std::string& text) {
    using detail::Aperture;
    GerberLayer L;
    std::map<int, Aperture> apertures;
    int dec = 6;                      // FS decimal digits
    double unit = 1.0;                // mm multiplier
    int ap = -1;
    double cx = 0, cy = 0;            // current point
    bool in_region = false;
    bool dark = true;                 // LPD / LPC
    int interp = 1;                   // G01 linear, G02/G03 arcs
    std::string net, comp;
    GRegion region;

    size_t i = 0;
    const size_t n = text.size();
    auto skip_ws = [&] {
        while (i < n && (std::isspace((unsigned char)text[i]))) ++i;
    };
    while (i < n) {
        skip_ws();
        if (i >= n) break;
        if (text[i] == '%') {                       // extended command
            const size_t end = text.find('%', i + 1);
            if (end == std::string::npos) break;
            std::string cmd = text.substr(i + 1, end - i - 1);
            i = end + 1;
            // strip trailing '*' and newlines
            while (!cmd.empty() && (cmd.back() == '*' || cmd.back() == '\n' ||
                                    cmd.back() == '\r'))
                cmd.pop_back();
            if (cmd.rfind("FSLA", 0) == 0) {
                const size_t x = cmd.find('X');
                if (x != std::string::npos && x + 2 < cmd.size())
                    dec = cmd[x + 2] - '0';
            } else if (cmd == "MOMM") {
                unit = 1.0;
            } else if (cmd == "MOIN") {
                unit = 25.4;
            } else if (cmd.rfind("ADD", 0) == 0) {
                // %ADD10C,0.25*%  /  %ADD11R,1.2X0.8*%
                size_t p = 3;
                int code = 0;
                while (p < cmd.size() && std::isdigit((unsigned char)cmd[p]))
                    code = code * 10 + (cmd[p++] - '0');
                if (p < cmd.size()) {
                    Aperture a;
                    a.kind = cmd[p];
                    const size_t comma = cmd.find(',', p);
                    if (comma != std::string::npos) {
                        const std::string dims = cmd.substr(comma + 1);
                        const size_t x = dims.find('X');
                        a.a = detail::num(dims.substr(0, x)) * unit;
                        a.b = x == std::string::npos
                                  ? a.a
                                  : detail::num(dims.substr(x + 1)) * unit;
                    }
                    apertures[code] = a;
                }
            } else if (cmd.rfind("TF.FileFunction,", 0) == 0) {
                // Copper,L2,Inr  /  Profile,NP  /  Soldermask,Top ...
                std::string rest = cmd.substr(16);
                const size_t c1 = rest.find(',');
                L.function = rest.substr(0, c1);
                if (L.function == "Copper" && c1 != std::string::npos) {
                    const size_t c2 = rest.find(',', c1 + 1);
                    std::string ln = rest.substr(c1 + 1, c2 - c1 - 1);
                    if (!ln.empty() && ln[0] == 'L')
                        L.copper_index = std::atoi(ln.c_str() + 1);
                    if (c2 != std::string::npos) L.side = rest.substr(c2 + 1);
                    const size_t c3 = L.side.find(',');
                    if (c3 != std::string::npos) L.side = L.side.substr(0, c3);
                }
            } else if (cmd.rfind("TO.N,", 0) == 0) {
                net = cmd.substr(5);
                L.has_net_attrs = true;
            } else if (cmd.rfind("TO.C,", 0) == 0) {
                comp = cmd.substr(5);
            } else if (cmd.rfind("TO.P,", 0) == 0) {
                // the PIN attribute (%TO.P,REF,PAD) — what KiCad actually
                // emits on pad flashes; the refdes is the part before the
                // second comma
                const size_t c = cmd.find(',', 5);
                comp = cmd.substr(5, c - 5);
            } else if (cmd == "TD" || cmd == "TD.N") {
                net.clear();
                if (cmd == "TD") comp.clear();
            } else if (cmd == "TD.C" || cmd == "TD.P") {
                comp.clear();
            } else if (cmd == "LPD") {
                dark = true;
            } else if (cmd == "LPC") {
                dark = false;
            }
            continue;
        }
        // word command up to '*'
        const size_t end = text.find('*', i);
        if (end == std::string::npos) break;
        std::string w = text.substr(i, end - i);
        i = end + 1;
        // strip whitespace inside
        w.erase(std::remove_if(w.begin(), w.end(),
                               [](char c) { return std::isspace((unsigned char)c); }),
                w.end());
        if (w.empty() || w.rfind("G04", 0) == 0 || w == "M02") continue;
        if (w == "G36") { in_region = true; region = {}; region.net = net; continue; }
        if (w == "G37") {
            in_region = false;
            if (dark && region.pts.size() >= 3) L.regions.push_back(region);
            else if (!dark && region.pts.size() >= 3) ++L.clear_skipped;
            continue;
        }
        if (w == "G01") { interp = 1; continue; }
        if (w == "G02") { interp = 2; continue; }
        if (w == "G03") { interp = 3; continue; }
        if (w == "G75" || w == "G74" || w == "G71" || w == "G70" ||
            w == "G90" || w == "G91")
            continue;
        if (w.rfind("G54", 0) == 0) w = w.substr(3);   // deprecated select prefix
        if (w[0] == 'D' && w.size() > 1 && std::isdigit((unsigned char)w[1])) {
            const int code = std::atoi(w.c_str() + 1);
            if (code >= 10) ap = code;
            continue;
        }
        if (w[0] == 'X' || w[0] == 'Y' || w[0] == 'I' || w[0] == 'J') {
            // coordinate word: X..Y..I..J..D0n
            double nx = cx, ny = cy, ci = 0, cj = 0;
            int op = 1;   // default modal D01 is deprecated but seen
            size_t p = 0;
            while (p < w.size()) {
                const char c = w[p];
                if (c == 'D') { op = std::atoi(w.c_str() + p + 1); break; }
                ++p;
                std::string digits;
                while (p < w.size() &&
                       (std::isdigit((unsigned char)w[p]) || w[p] == '-' ||
                        w[p] == '+'))
                    digits += w[p++];
                const double v = detail::coord(digits, dec, unit);
                if (c == 'X') nx = v;
                else if (c == 'Y') ny = v;
                else if (c == 'I') ci = v;
                else if (c == 'J') cj = v;
            }
            if (in_region) {
                if (op == 2) { region.pts.clear(); region.pts.push_back({nx, ny}); }
                else if (op == 1) {
                    if (interp != 1) ++L.arcs_approximated;
                    region.pts.push_back({nx, ny});
                }
                cx = nx; cy = ny;
                continue;
            }
            const Aperture a = apertures.count(ap) ? apertures[ap] : Aperture{};
            if (op == 1 && dark) {
                if (interp == 1) {
                    L.tracks.push_back({cx, cy, nx, ny,
                                        a.kind == 'R' ? std::min(a.a, a.b) : a.a,
                                        net});
                } else {
                    // arc: chord-approximate about the centre (cx+ci, cy+cj)
                    const double ox = cx + ci, oy = cy + cj;
                    const double r = std::hypot(cx - ox, cy - oy);
                    double a0 = std::atan2(cy - oy, cx - ox);
                    double a1 = std::atan2(ny - oy, nx - ox);
                    if (interp == 3 && a1 <= a0) a1 += 2 * M_PI;   // CCW
                    if (interp == 2 && a1 >= a0) a1 -= 2 * M_PI;   // CW
                    const int chords = 8;
                    double px = cx, py = cy;
                    for (int k = 1; k <= chords; ++k) {
                        const double t = a0 + (a1 - a0) * k / chords;
                        const double qx = ox + r * std::cos(t);
                        const double qy = oy + r * std::sin(t);
                        L.tracks.push_back({px, py, qx, qy,
                                            a.kind == 'R' ? std::min(a.a, a.b)
                                                          : a.a,
                                            net});
                        px = qx; py = qy;
                    }
                    ++L.arcs_approximated;
                }
            } else if (op == 1 && !dark) {
                ++L.clear_skipped;
            } else if (op == 3 && dark) {
                L.flashes.push_back({nx, ny, a.a, a.kind == 'C' ? a.a : a.b,
                                     net, comp});
            }
            cx = nx; cy = ny;
            continue;
        }
        // anything else (M01, G worthless) — ignore
    }
    return L;
}

// ---------------------------------------------------------------------------
// Excellon drills (decimal format, as KiCad and most CAM tools emit)
// ---------------------------------------------------------------------------

struct Drill { double x, y, d; };

inline bool looks_excellon(const std::string& t) {
    return t.find("M48") != std::string::npos &&
           t.find("%FS") == std::string::npos;
}

inline std::vector<Drill> parse_excellon(const std::string& text) {
    std::vector<Drill> out;
    std::map<int, double> tools;
    double unit = 1.0;
    int tool = -1;
    bool header = true;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line == "M48") { header = true; continue; }
        if (line == "%" || line == "M95") { header = false; continue; }
        if (line.rfind("METRIC", 0) == 0) { unit = 1.0; continue; }
        if (line.rfind("INCH", 0) == 0) { unit = 25.4; continue; }
        if (line[0] == 'T') {
            const size_t c = line.find('C');
            const int t = std::atoi(line.c_str() + 1);
            if (header && c != std::string::npos)
                tools[t] = detail::num(line.substr(c + 1)) * unit;
            else if (!header)
                tool = t;
            continue;
        }
        if (line[0] == 'X' || line[0] == 'Y') {
            if (line.find('.') == std::string::npos)
                // Fixed-format Excellon needs the header's digit declaration,
                // which varies by tool and is not implemented — refusing beats
                // placing every via at a wrong coordinate.
                throw BoardError(
                    "gerber: this drill file uses fixed-format coordinates; "
                    "export Excellon with decimal coordinates (KiCad default)");
            double x = 0, y = 0;
            const size_t yat = line.find('Y');
            if (line[0] == 'X')
                x = detail::num(line.substr(1, yat - 1)) * unit;
            if (yat != std::string::npos)
                y = detail::num(line.substr(yat + 1)) * unit;
            if (tool >= 0 && tools.count(tool))
                out.push_back({x, y, tools[tool]});
            continue;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Set assembly
// ---------------------------------------------------------------------------

inline bool looks_gerber(const std::string& t) {
    const std::string head = t.substr(0, std::min<size_t>(t.size(), 4000));
    return head.find("%FS") != std::string::npos ||
           head.find("%TF.") != std::string::npos ||
           head.find("G04") != std::string::npos;
}

inline BoardIR import_gerber_set(const std::vector<NamedFile>& files,
                                 std::optional<Stackup> user_stackup) {
    std::vector<GerberLayer> coppers;
    std::optional<GerberLayer> profile;
    std::vector<Drill> drills;
    int clear_skipped = 0, arcs = 0;

    for (const auto& f : files) {
        if (looks_excellon(f.text)) {
            auto d = parse_excellon(f.text);
            drills.insert(drills.end(), d.begin(), d.end());
            continue;
        }
        if (!looks_gerber(f.text)) continue;   // README, job files, ...
        GerberLayer L = parse_gerber(f.text);
        clear_skipped += L.clear_skipped;
        arcs += L.arcs_approximated;
        if (L.function == "Copper" && L.copper_index > 0)
            coppers.push_back(std::move(L));
        else if (L.function == "Profile")
            profile = std::move(L);
        // masks, silks, paste: nothing for EMC analysis
    }
    if (coppers.empty())
        throw BoardError(
            "gerber: no copper layer with an X2 FileFunction attribute found — "
            "a set needs %TF.FileFunction,Copper,L<n>,... on each copper file. "
            "Enable X2 attributes in the CAM export (KiCad: Plot → 'Use "
            "extended X2 format').");
    std::sort(coppers.begin(), coppers.end(),
              [](const GerberLayer& a, const GerberLayer& b) {
                  return a.copper_index < b.copper_index;
              });
    bool any_nets = false;
    for (const auto& c : coppers) any_nets = any_nets || c.has_net_attrs;
    if (!any_nets)
        throw BoardError(
            "gerber: no %TO.N net attributes anywhere in the set. Without nets "
            "most of the analysis is meaningless (coupled-run cannot exclude "
            "same-net pairs, planes have no identity), so this is refused "
            "rather than guessed. Enable 'include netlist attributes' in the "
            "CAM export.");

    BoardIR b;
    b.approximated_arcs = arcs;
    for (size_t i = 0; i < coppers.size(); ++i) {
        const auto& c = coppers[i];
        b.copper_names.push_back(
            c.side == "Top" ? "Top"
            : c.side == "Bot" ? "Bottom"
                              : "In" + std::to_string(c.copper_index - 1));
    }
    if (user_stackup) {
        b.stackup = *user_stackup;   // builtin_stackup already stamps "user:"
    } else {
        throw BoardError(
            "gerber: no stackup — a Gerber set carries no layer thicknesses "
            "and no permittivity. This set has " +
            std::to_string(coppers.size()) +
            " copper layers; choose default-" + std::to_string(coppers.size()) +
            "layer or supply one.");
    }

    // nets
    std::map<std::string, int> net_id;
    b.nets.push_back({0, ""});
    auto net_of = [&](const std::string& name) {
        if (name.empty()) return 0;
        auto it = net_id.find(name);
        if (it != net_id.end()) return it->second;
        const int id = (int)b.nets.size();
        b.nets.push_back({id, name});
        net_id[name] = id;
        return id;
    };

    // copper geometry; flashes are gathered for via matching below
    struct FlashRef { double x, y, w, h; int net, cu; std::string comp; };
    std::vector<FlashRef> flashes;
    for (size_t cu = 0; cu < coppers.size(); ++cu) {
        for (const auto& t : coppers[cu].tracks) {
            if (!(t.w > 0)) continue;
            b.segments.push_back({net_of(t.net), (int)cu, t.x1, t.y1, t.x2,
                                  t.y2, t.w});
        }
        for (const auto& r : coppers[cu].regions)
            b.zones.push_back({net_of(r.net), (int)cu, r.pts});
        for (const auto& f : coppers[cu].flashes)
            flashes.push_back({f.x, f.y, f.w, f.h, net_of(f.net), (int)cu,
                               f.comp});
    }

    // Drills become THROUGH vias, their net taken from a coincident flash.
    // Blind/buried spans live in per-file layer pairs this importer does not
    // read yet; treating every drill as through is stated, not hidden.
    const int last_cu = (int)coppers.size() - 1;
    std::set<size_t> via_flashes;
    for (const auto& d : drills) {
        int net = 0;
        double size = d.d * 1.6;
        bool component_hole = false, via_ring = false;
        for (size_t k = 0; k < flashes.size(); ++k) {
            if (std::hypot(flashes[k].x - d.x, flashes[k].y - d.y) < 0.05) {
                net = flashes[k].net;
                size = std::max(flashes[k].w, flashes[k].h);
                if (flashes[k].comp.empty()) { via_flashes.insert(k); via_ring = true; }
                else component_hole = true;
            }
        }
        // What is a via here — calibrated against the native import on the
        // 9-board corpus. A compless flash on ANY layer is a via ring, so the
        // drill is a via (this keeps via-in-pad, where the outer layer flash
        // carries the pad's %TO.P). Flashes on every layer owned by a
        // component = a through-hole pin. No flash at all = an NPTH mounting
        // hole (KiCad merges NPTH into the same .drl by default).
        if (!via_ring) continue;
        (void)component_hole;
        b.vias.push_back({net, d.x, d.y, size, d.d, 0, last_cu});
    }

    // Remaining flashes are pads; component refs come from %TO.C
    std::map<std::string, std::pair<double, double>> comp_pos;
    std::map<std::string, int> comp_n;
    for (size_t k = 0; k < flashes.size(); ++k) {
        if (via_flashes.count(k)) continue;
        const auto& f = flashes[k];
        b.pads.push_back({f.comp, f.net, f.x, f.y, f.w, f.h, false, f.cu});
        if (!f.comp.empty()) {
            comp_pos[f.comp].first += f.x;
            comp_pos[f.comp].second += f.y;
            ++comp_n[f.comp];
        }
    }
    for (const auto& [ref, sum] : comp_pos)
        b.components.push_back({ref, "", "", sum.first / comp_n[ref],
                                sum.second / comp_n[ref], 0.0});

    // outline from the Profile file, else from the copper extents
    double x1 = 1e30, y1 = 1e30, x2 = -1e30, y2 = -1e30;
    auto grow = [&](double x, double y) {
        x1 = std::min(x1, x); y1 = std::min(y1, y);
        x2 = std::max(x2, x); y2 = std::max(y2, y);
    };
    if (profile) {
        for (const auto& t : profile->tracks) { grow(t.x1, t.y1); grow(t.x2, t.y2); }
        for (const auto& r : profile->regions)
            for (const auto& p : r.pts) grow(p.x, p.y);
        b.bbox_from_outline = x2 > x1;
    }
    if (!(x2 > x1)) {
        for (const auto& s : b.segments) { grow(s.x1, s.y1); grow(s.x2, s.y2); }
        for (const auto& z : b.zones)
            for (const auto& p : z.pts) grow(p.x, p.y);
        for (const auto& v : b.vias) grow(v.x, v.y);
        b.bbox_from_outline = false;
    }
    if (!(x2 > x1))
        throw BoardError("gerber: the set contains no geometry at all");
    b.bbox_x1 = x1; b.bbox_y1 = y1; b.bbox_x2 = x2; b.bbox_y2 = y2;

    if ((size_t)b.stackup.copper_indices().size() != coppers.size())
        throw BoardError(
            "gerber: the chosen stackup has " +
            std::to_string(b.stackup.copper_indices().size()) +
            " copper layers but the set has " + std::to_string(coppers.size()));

    b.gerber_clear_skipped = clear_skipped;
    return b;
}

}  // namespace faraday::gerber
