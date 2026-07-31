#pragma once
// ODB++ set importer. An ODB++ job is a directory tree; the importer takes it
// as a set of (path, text) files — from a zip in the browser, or a directory
// on the CLI — and reads the subset that carries electrical meaning:
//
//   matrix/matrix                     layer list: order, type, drill spans
//   steps/<s>/layers/<l>/features     copper geometry (lines, pads, surfaces)
//   steps/<s>/layers/<l>/components   CMP/TOP records: refdes, values, pins
//   steps/<s>/eda/data                nets, and the feature-to-net mapping
//   steps/<s>/profile                 the board outline
//
// Net identity is EXACT here — eda/data's FID records name the net of every
// copper feature, TOP records carry each pin's net, and an SNT VIA subnet
// groups a via's hole with its pads — so unlike Gerber there is no
// coincidence matching anywhere. Component values (PRP Value) survive too,
// which means the PDN tool works on ODB++ boards.
//
// Honest limits, stated not guessed: surface HOLES (thermal reliefs, zone
// clearances) are skipped and counted — the pour polygon is its outer
// boundary, so pour coverage reads slightly optimistic where holes are
// dense. Arcs (A records, OC curve segments) are chord-approximated and
// counted. Y is negated throughout: ODB++ is y-up, the board IR is KiCad's
// y-down. A tree without eda/data is refused — netless analysis is
// meaningless for the net-aware rules. ODB++ carries no dielectric
// thicknesses, so a stackup must be chosen, exactly like the Gerber path.

#include "BoardIR.hpp"
#include "GerberImporter.hpp"   // NamedFile

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace faraday::odb {

using gerber::NamedFile;

namespace detail {

inline std::string lower(std::string s) {
    for (char& c : s) c = std::tolower((unsigned char)c);
    return s;
}

// Split a features-file record into whitespace tokens, dropping the
// ";attr" tail.
inline std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream ss(line.substr(0, line.find(';')));
    std::string t;
    while (ss >> t) out.push_back(t);
    return out;
}

// A symbol's dimensions live in its NAME, in 1/1000 of the file unit
// (µm for MM, mils for inch). r300.0 → 0.3 mm round; rect900.0x950.0xr225.0
// → 0.9 × 0.95 mm rectangle (corner radius ignored).
struct Symbol { double w = 0, h = 0; };

inline Symbol parse_symbol(const std::string& name, double unit_mm) {
    auto dims = [&](size_t at) {
        std::vector<double> v;
        std::string d = name.substr(at);
        size_t p = 0;
        while (p < d.size()) {
            if (d[p] == 'x') { ++p; continue; }
            if (d[p] == 'r' && !v.empty()) break;   // corner-radius suffix
            char* end;
            v.push_back(std::strtod(d.c_str() + p, &end));
            if (end == d.c_str() + p) break;
            p = end - d.c_str();
        }
        return v;
    };
    const double k = unit_mm / 1000.0;
    Symbol s;
    if (name.rfind("rect", 0) == 0) {
        auto v = dims(4);
        if (v.size() >= 2) { s.w = v[0] * k; s.h = v[1] * k; return s; }
    } else if (name.rfind("oval", 0) == 0) {
        auto v = dims(4);
        if (v.size() >= 2) { s.w = v[0] * k; s.h = v[1] * k; return s; }
    } else if (name[0] == 'r' || name[0] == 's') {
        auto v = dims(1);
        if (!v.empty()) { s.w = s.h = v[0] * k; return s; }
    }
    throw BoardError("odb: unsupported symbol '" + name +
                     "' on a copper layer — cannot recover its width");
}

}  // namespace detail

// ---------------------------------------------------------------------------
// One parsed features file
// ---------------------------------------------------------------------------

// One island of a surface: its outer ring and the hole rings cut from it.
struct OIsland {
    std::vector<Point> ring;
    std::vector<std::vector<Point>> holes;
};

struct OFeature {
    char kind = 0;                    // L, P, A, S (T recorded, geometry-free)
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;   // mm, y already negated
    double w = 0, h = 0;              // symbol dims
    // S only. A surface stays ONE feature however many islands it has —
    // eda/data FID indices count file records, and splitting islands into
    // extra features would silently shift the net of everything after them.
    std::vector<OIsland> islands;
};

struct OLayer {
    std::vector<OFeature> feats;      // in file order — FID indices point here
    int arcs = 0;
};

// The job's unit, in mm per file unit. ODB++ declares it per FILE, as a
// leading "UNITS=MM|INCH" line — and a file that omits it inherits the job's,
// whose FORMAT DEFAULT IS INCH. Defaulting to mm instead silently shrank
// every undeclared job by 25.4x: Altium's exporter writes no UNITS line
// anywhere, so a real 92 x 80 mm board imported as 3.6 x 3.1 mm, with 5 mil
// traces read as 5 um and via stubs resonating at 23 GHz instead of 0.9 GHz.
// We take the declaration from ANY file in the job that carries one, so a
// partly-annotated export stays self-consistent.
inline double declared_unit(const std::string& text) {
    // the line is at the top of the file, before any feature record
    std::istringstream ss(text);
    std::string line;
    for (int i = 0; i < 40 && std::getline(ss, line); ++i) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.rfind("UNITS=", 0) == 0)
            return line.substr(6) == "MM" ? 1.0 : 25.4;
    }
    return 0.0;   // not declared here
}

inline OLayer parse_features(const std::string& text, double job_unit) {
    OLayer L;
    std::map<int, detail::Symbol> syms;
    double unit = job_unit;
    OFeature* surf = nullptr;         // open S record
    bool in_hole = false;
    std::istringstream ss(text);
    std::string line;
    auto sym_of = [&](const std::string& tok) {
        return syms.count(std::atoi(tok.c_str()))
                   ? syms[std::atoi(tok.c_str())]
                   : detail::Symbol{};
    };
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == '@' || line[0] == '&')
            continue;
        if (line.rfind("UNITS=", 0) == 0) {
            unit = line.substr(6) == "MM" ? 1.0 : 25.4;
            continue;
        }
        if (line[0] == '$') {
            const auto t = detail::tokens(line);
            if (t.size() >= 2)
                syms[std::atoi(t[0].c_str() + 1)] =
                    detail::parse_symbol(t[1], unit);
            continue;
        }
        const auto t = detail::tokens(line);
        if (t.empty()) continue;
        const std::string& k = t[0];
        if (k == "L" && t.size() >= 6) {
            OFeature f;
            f.kind = 'L';
            f.x1 = std::atof(t[1].c_str()) * unit;
            f.y1 = -std::atof(t[2].c_str()) * unit;
            f.x2 = std::atof(t[3].c_str()) * unit;
            f.y2 = -std::atof(t[4].c_str()) * unit;
            f.w = sym_of(t[5]).w;
            L.feats.push_back(f);
        } else if (k == "P" && t.size() >= 4) {
            OFeature f;
            f.kind = 'P';
            f.x1 = std::atof(t[1].c_str()) * unit;
            f.y1 = -std::atof(t[2].c_str()) * unit;
            const auto s = sym_of(t[3]);
            f.w = s.w; f.h = s.h;
            L.feats.push_back(f);
        } else if (k == "A" && t.size() >= 8) {
            // arc: chord it start→end and count; the IR has no arcs
            OFeature f;
            f.kind = 'L';
            f.x1 = std::atof(t[1].c_str()) * unit;
            f.y1 = -std::atof(t[2].c_str()) * unit;
            f.x2 = std::atof(t[3].c_str()) * unit;
            f.y2 = -std::atof(t[4].c_str()) * unit;
            f.w = sym_of(t[7]).w;
            L.feats.push_back(f);
            ++L.arcs;
        } else if (k == "S") {
            OFeature f;
            f.kind = 'S';
            L.feats.push_back(f);
            surf = &L.feats.back();
            in_hole = false;
        } else if (k == "OB" && surf) {
            in_hole = t.size() >= 4 && t[3] == "H";
            const Point p0{std::atof(t[1].c_str()) * unit,
                           -std::atof(t[2].c_str()) * unit};
            if (in_hole) {
                if (surf->islands.empty()) surf->islands.push_back({});
                surf->islands.back().holes.push_back({p0});
            } else {
                surf->islands.push_back({{p0}, {}});
            }
        } else if ((k == "OS" || k == "OC") && surf && t.size() >= 3) {
            if (surf->islands.empty()) surf->islands.push_back({});
            auto& isl = surf->islands.back();
            auto& ring = in_hole && !isl.holes.empty() ? isl.holes.back()
                                                       : isl.ring;
            ring.push_back({std::atof(t[1].c_str()) * unit,
                            -std::atof(t[2].c_str()) * unit});
            if (k == "OC") ++L.arcs;
        } else if (k == "T") {
            OFeature f;
            f.kind = 'T';        // text: indexed, no geometry
            L.feats.push_back(f);
        }
        // OE / SE close records; nothing to do
    }
    return L;
}

// ---------------------------------------------------------------------------
// The set importer
// ---------------------------------------------------------------------------

inline bool is_odb_set(const std::vector<NamedFile>& files) {
    for (const auto& f : files) {
        std::string n = detail::lower(f.name);
        if (n == "matrix/matrix" || n.size() > 13 &&
            n.compare(n.size() - 14, 14, "/matrix/matrix") == 0)
            return true;
    }
    return false;
}

inline BoardIR import_odb(const std::vector<NamedFile>& files,
                          std::optional<Stackup> user_stackup) {
    // locate the tree root via matrix/matrix
    std::string root;
    const std::string* matrix = nullptr;
    for (const auto& f : files) {
        std::string n = detail::lower(f.name);
        const size_t at = n.rfind("matrix/matrix");
        if (at != std::string::npos &&
            (at == 0 || n[at - 1] == '/') &&
            at + 13 == n.size()) {
            root = f.name.substr(0, at);
            matrix = &f.text;
        }
    }
    if (!matrix)
        throw BoardError("odb: no matrix/matrix in the set — not an ODB++ job");
    auto file_at = [&](const std::string& rel) -> const std::string* {
        const std::string want = detail::lower(root + rel);
        for (const auto& f : files)
            if (detail::lower(f.name) == want) return &f.text;
        return nullptr;
    };

    // job unit: the first explicit declaration anywhere in the set wins;
    // absent one, the format's default (inch) — never mm, see declared_unit
    double job_unit = 0.0;
    for (const auto& f : files)
        if (double u = declared_unit(f.text)) { job_unit = u; break; }
    if (job_unit == 0.0) job_unit = 25.4;

    // ---- matrix: layer order, types, drill spans ----
    struct MLayer { std::string name, type, start, end; int row = 0; };
    std::vector<MLayer> mlayers;
    {
        MLayer cur;
        bool in_layer = false;
        std::istringstream ss(*matrix);
        std::string line;
        auto val = [](const std::string& l) {
            return l.substr(l.find('=') + 1);
        };
        while (std::getline(ss, line)) {
            // strip indentation and \r
            size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            size_t e = line.find_last_not_of(" \t\r");
            line = line.substr(b, e - b + 1);
            if (line.rfind("LAYER", 0) == 0 && line.find('{') != std::string::npos) {
                in_layer = true; cur = {};
            } else if (line == "}" && in_layer) {
                in_layer = false;
                if (!cur.name.empty()) mlayers.push_back(cur);
            } else if (in_layer) {
                if (line.rfind("NAME=", 0) == 0) cur.name = val(line);
                else if (line.rfind("TYPE=", 0) == 0) cur.type = val(line);
                else if (line.rfind("ROW=", 0) == 0) cur.row = std::atoi(val(line).c_str());
                else if (line.rfind("START_NAME=", 0) == 0) cur.start = val(line);
                else if (line.rfind("END_NAME=", 0) == 0) cur.end = val(line);
            }
        }
    }
    std::sort(mlayers.begin(), mlayers.end(),
              [](const MLayer& a, const MLayer& b) { return a.row < b.row; });

    std::vector<MLayer> coppers, drills, compl_;
    for (const auto& m : mlayers) {
        if (m.type == "SIGNAL" || m.type == "POWER_GROUND" || m.type == "MIXED")
            coppers.push_back(m);
        else if (m.type == "DRILL")
            drills.push_back(m);
        else if (m.type == "COMPONENT")
            compl_.push_back(m);
    }
    if (coppers.size() < 2)
        throw BoardError("odb: fewer than 2 copper layers in matrix/matrix");

    // ---- find the step ----
    std::string step;
    for (const auto& f : files) {
        std::string n = detail::lower(f.name);
        const std::string pre = detail::lower(root) + "steps/";
        if (n.rfind(pre, 0) == 0) {
            step = n.substr(pre.size(), n.find('/', pre.size()) - pre.size());
            break;
        }
    }
    if (step.empty()) throw BoardError("odb: no steps/ in the set");

    BoardIR b;
    std::map<std::string, int> cu_of;   // lowercase matrix name → ordinal
    for (size_t i = 0; i < coppers.size(); ++i) {
        b.copper_names.push_back(coppers[i].name);
        cu_of[detail::lower(coppers[i].name)] = (int)i;
    }
    if (user_stackup) {
        b.stackup = *user_stackup;
        if ((size_t)b.stackup.copper_indices().size() != coppers.size())
            throw BoardError(
                "odb: the chosen stackup has " +
                std::to_string(b.stackup.copper_indices().size()) +
                " copper layers but the job has " +
                std::to_string(coppers.size()));
    } else {
        throw StackupNeeded(
            "odb: no stackup — ODB++ carries no dielectric thicknesses. This "
            "job has " + std::to_string(coppers.size()) +
            " copper layers; choose default-" + std::to_string(coppers.size()) +
            "layer or supply one.",
            (int)coppers.size());
    }

    // ---- eda/data: nets + feature→net + toeprint ownership ----
    const std::string* eda = file_at("steps/" + step + "/eda/data");
    if (!eda)
        throw BoardError(
            "odb: no steps/" + step + "/eda/data — without it no feature has "
            "a net, and netless analysis is meaningless for the net-aware "
            "rules. Export with EDA data included.");
    std::vector<std::string> eda_lyrs;             // FID layer index → name
    struct FeatNet { int net = 0; char subnet = 0; int comp = -1, pin = -1; char side = 0; };
    std::map<std::pair<int, int>, FeatNet> feat_net;  // (lyr idx, feat idx)
    int n_nets = 0;
    {
        std::istringstream ss(*eda);
        std::string line;
        int cur_net = -1;
        char cur_sub = 0, cur_side = 0;
        int cur_comp = -1, cur_pin = -1;
        while (std::getline(ss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const auto t = detail::tokens(line);
            if (t.empty()) continue;
            if (t[0] == "LYR") {
                eda_lyrs.assign(t.begin() + 1, t.end());
            } else if (t[0] == "NET" && t.size() >= 2) {
                cur_net = n_nets++;
                b.nets.push_back({cur_net,
                                  t[1] == "$NONE$" ? "" : t[1]});
            } else if (t[0] == "SNT" && t.size() >= 2) {
                cur_sub = t[1] == "TOP" ? 'T'
                          : t[1] == "VIA" ? 'V'
                          : t[1] == "TRC" ? 'R' : 'P';
                cur_comp = cur_pin = -1; cur_side = 0;
                if (cur_sub == 'T' && t.size() >= 5) {
                    cur_side = t[2][0];                   // T | B
                    cur_comp = std::atoi(t[3].c_str());
                    cur_pin = std::atoi(t[4].c_str());
                }
            } else if (t[0] == "FID" && t.size() >= 4 && cur_net >= 0) {
                feat_net[{std::atoi(t[2].c_str()), std::atoi(t[3].c_str())}] =
                    {cur_net, cur_sub, cur_comp, cur_pin, cur_side};
            }
        }
    }
    auto fid_lyr = [&](const std::string& lname) {
        for (size_t i = 0; i < eda_lyrs.size(); ++i)
            if (detail::lower(eda_lyrs[i]) == detail::lower(lname))
                return (int)i;
        return -1;
    };

    // ---- copper layers ----
    for (const auto& m : coppers) {
        const std::string* ft =
            file_at("steps/" + step + "/layers/" + detail::lower(m.name) +
                    "/features");
        if (!ft) continue;   // a copper layer with no features file is empty
        OLayer L = parse_features(*ft, job_unit);
        b.approximated_arcs += L.arcs;
        const int cu = cu_of[detail::lower(m.name)];
        const int fl = fid_lyr(m.name);
        for (size_t fi = 0; fi < L.feats.size(); ++fi) {
            const OFeature& f = L.feats[fi];
            FeatNet fn;
            if (auto it = feat_net.find({fl, (int)fi}); it != feat_net.end())
                fn = it->second;
            if (f.kind == 'L') {
                if (f.w > 0)
                    b.segments.push_back({fn.net, cu, f.x1, f.y1, f.x2, f.y2,
                                          f.w});
            } else if (f.kind == 'P') {
                if (fn.subnet == 'V') continue;   // via pad: the via row covers it
                // toeprints become Pads once components are read; a bare
                // copper flash (testpoint) becomes an unnamed pad
                b.pads.push_back({"", fn.net, f.x1, f.y1, f.w, f.h, false, cu});
                if (fn.subnet == 'T') {
                    // remember which component owns it via the parallel map
                    b.pads.back().component =
                        "\x01" + std::string(1, fn.side) +
                        std::to_string(fn.comp);   // resolved below
                    // toeprint INDEX for now; the components pass swaps it
                    // for the real pin NAME off its TOP records
                    b.pads.back().pin = "\x01" + std::to_string(fn.pin);
                }
            } else if (f.kind == 'S') {
                if (fn.subnet == 'V') continue;   // via pad drawn as a surface
                if (fn.subnet == 'T') {
                    // a custom-shaped PAD exported as a surface (fomu's FPGA
                    // balls) — a pour it is not
                    double px1 = 1e30, py1 = 1e30, px2 = -1e30, py2 = -1e30;
                    for (const auto& isl : f.islands)
                        for (const auto& p : isl.ring) {
                            px1 = std::min(px1, p.x); py1 = std::min(py1, p.y);
                            px2 = std::max(px2, p.x); py2 = std::max(py2, p.y);
                        }
                    if (px2 > px1)
                        b.pads.push_back({"\x01" + std::string(1, fn.side) +
                                              std::to_string(fn.comp),
                                          fn.net, (px1 + px2) / 2,
                                          (py1 + py2) / 2, px2 - px1,
                                          py2 - py1, false, cu});
                    continue;
                }
                for (const auto& isl : f.islands)
                    if (isl.ring.size() >= 3)
                        b.zones.push_back({fn.net, cu, isl.ring, isl.holes});
            }
        }
    }

    // ---- components: refdes, values, and toeprint ownership ----
    // CMP index is per component layer (top/bottom), matching SNT TOP's side.
    for (const auto& m : compl_) {
        const std::string* ct =
            file_at("steps/" + step + "/layers/" + detail::lower(m.name) +
                    "/components");
        if (!ct) continue;
        const bool top = detail::lower(m.name).find("top") != std::string::npos;
        // CMP x/y are in file units like every other coordinate — they were
        // read raw, which put every component's placement 25.4x off on an
        // inch job (and left it right only because mm jobs scale by 1)
        const double cunit = declared_unit(*ct) ? declared_unit(*ct) : job_unit;
        std::istringstream ss(*ct);
        std::string line;
        int cmp_idx = -1;
        std::string cur_ref;
        while (std::getline(ss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const auto t = detail::tokens(line);
            if (t.empty()) continue;
            if (t[0] == "CMP" && t.size() >= 7) {
                ++cmp_idx;
                cur_ref = t[6];
                b.components.push_back({cur_ref,
                                        t.size() >= 8 ? t[7] : "", "",
                                        std::atof(t[2].c_str()) * cunit,
                                        -std::atof(t[3].c_str()) * cunit,
                                        std::atof(t[4].c_str())});
                // rewrite the placeholder component keys on pads
                const std::string key =
                    "\x01" + std::string(1, top ? 'T' : 'B') +
                    std::to_string(cmp_idx);
                for (auto& p : b.pads)
                    if (p.component == key) p.component = cur_ref;
            } else if (t[0] == "TOP" && t.size() >= 9 && !cur_ref.empty()) {
                // TOP <toep> x y rot mir <net> <snt> <name>: resolve the
                // temporary toeprint index into the real pin name. Altium
                // writes "<ref>-<pin>", KiCad writes the bare pin.
                const std::string& nm = t[8];
                const size_t dash = nm.rfind('-');
                const std::string pin_name =
                    dash != std::string::npos ? nm.substr(dash + 1) : nm;
                if (!pin_name.empty()) {
                    const std::string want = "\x01" + t[1];
                    for (auto& p : b.pads)
                        if (p.component == cur_ref && p.pin == want)
                            p.pin = pin_name;
                }
            } else if (t[0] == "PRP" && t.size() >= 3 && t[1] == "Value" &&
                       !b.components.empty()) {
                // PRP Value '100n' — quoted, possibly with spaces
                const size_t q1 = line.find('\'');
                const size_t q2 = line.rfind('\'');
                if (q1 != std::string::npos && q2 > q1)
                    b.components.back().value = line.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }
    // any unresolved placeholder keys (eda names a component the components
    // file does not have) become anonymous rather than leaking \x01 keys;
    // same for pin indices no TOP record named — absence, not a wrong number
    for (auto& p : b.pads) {
        if (!p.component.empty() && p.component[0] == '\x01') p.component = "";
        if (!p.pin.empty() && p.pin[0] == '\x01') p.pin = "";
    }

    // A through-hole pin appears as one toeprint PER COPPER LAYER in ODB++;
    // the IR's convention (from the KiCad importer) is ONE pad with cu = -1.
    // Merge same (component, net, position) across layers so pad counts and
    // per-component views agree between formats.
    {
        std::map<std::tuple<std::string, int, long, long>, size_t> first;
        std::vector<Pad> merged;
        for (const auto& p : b.pads) {
            const auto key = std::make_tuple(
                p.component, p.net, std::lround(p.x * 1000.0),
                std::lround(p.y * 1000.0));
            auto it = first.find(key);
            if (it == first.end()) {
                first[key] = merged.size();
                merged.push_back(p);
            } else {
                Pad& q = merged[it->second];
                q.through_hole = true;
                q.cu = -1;
                q.w = std::max(q.w, p.w);
                q.h = std::max(q.h, p.h);
            }
        }
        b.pads = std::move(merged);
    }

    // ---- vias: drill layers, span from the matrix, net from FID H ----
    for (const auto& m : drills) {
        // non-plated drills are mounting holes — no barrel, no via
        if (detail::lower(m.name).find("non-plated") != std::string::npos)
            continue;
        const std::string* ft =
            file_at("steps/" + step + "/layers/" + detail::lower(m.name) +
                    "/features");
        if (!ft) continue;
        const int from = cu_of.count(detail::lower(m.start))
                             ? cu_of[detail::lower(m.start)] : 0;
        const int to = cu_of.count(detail::lower(m.end))
                           ? cu_of[detail::lower(m.end)]
                           : (int)coppers.size() - 1;
        OLayer L = parse_features(*ft, job_unit);
        const int fl = fid_lyr(m.name);
        for (size_t fi = 0; fi < L.feats.size(); ++fi) {
            const OFeature& f = L.feats[fi];
            if (f.kind != 'P') continue;
            FeatNet fn;
            if (auto it = feat_net.find({fl, (int)fi}); it != feat_net.end())
                fn = it->second;
            if (fn.subnet == 'T') continue;   // component hole, not a via
            b.vias.push_back({fn.net, f.x1, f.y1, f.w * 1.6, f.w,
                              std::min(from, to), std::max(from, to)});
        }
    }

    // ---- outline from the profile ----
    double x1 = 1e30, y1 = 1e30, x2 = -1e30, y2 = -1e30;
    auto grow = [&](double x, double y) {
        x1 = std::min(x1, x); y1 = std::min(y1, y);
        x2 = std::max(x2, x); y2 = std::max(y2, y);
    };
    if (const std::string* pf = file_at("steps/" + step + "/profile")) {
        OLayer P = parse_features(*pf, job_unit);
        for (const auto& f : P.feats) {
            for (const auto& isl : f.islands)
                for (const auto& p : isl.ring) grow(p.x, p.y);
            if (f.kind == 'L') { grow(f.x1, f.y1); grow(f.x2, f.y2); }
        }
        b.bbox_from_outline = x2 > x1;
    }
    if (!(x2 > x1)) {
        for (const auto& s : b.segments) { grow(s.x1, s.y1); grow(s.x2, s.y2); }
        for (const auto& z : b.zones)
            for (const auto& p : z.pts) grow(p.x, p.y);
        for (const auto& v : b.vias) grow(v.x, v.y);
        b.bbox_from_outline = false;
    }
    if (!(x2 > x1)) throw BoardError("odb: the job contains no geometry");
    b.bbox_x1 = x1; b.bbox_y1 = y1; b.bbox_x2 = x2; b.bbox_y2 = y2;

    return b;
}

}  // namespace faraday::odb
