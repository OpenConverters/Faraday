#pragma once
// HyperLynx .HYP (Signal-Integrity Transfer Format) -> BoardIR.
//
// Why this format: Altium, PADS, Expedition and Eagle all export it, so one
// importer reaches four more EDA tools. It also carries what Faraday needs —
// stackup with permittivity, per-net geometry, component pins — unlike Gerber.
//
// Written from the published format and a reference export; no code is taken
// from hyp2mat (GPL), which must not be linked into this MIT layer.
//
// Structure:
//   {VERSION=2.10}  {UNITS=ENGLISH LENGTH}  or  {UNITS=METRIC LENGTH=MM}
//   {BOARD    (PERIMETER_SEGMENT X1= Y1= X2= Y2=) ... }
//   {STACKUP  (SIGNAL T= P= L=) (DIELECTRIC T= C= L=) (PLANE T= L=) ... }
//   {DEVICES  (C REF="C1" VAL=100n L="Top") ... }
//   {PADSTACK=name (layer,type,sx,sy,angle) ... }
//   {NET=name (SEG X1= Y1= X2= Y2= W= L=) (VIA X= Y= L1= L2=)
//             (PIN X= Y= R=refdes.pin P=padstack)
//             {POLYGON L= T=POUR ID= X= Y= (LINE X= Y=) ...} }
// A '*' begins a comment line; text after a record's ')' is also commentary.

#include "BoardIR.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace faraday {

namespace hyp {

struct Record {
    std::string type;                            // SEG, PIN, SIGNAL, LINE, ...
    std::map<std::string, std::string> kv;       // X1=, T=, L=, ...
    std::vector<std::string> positional;         // padstack "Top,1,0.035,..."

    bool has(const std::string& k) const { return kv.count(k) != 0; }
    const std::string& str(const std::string& k) const {
        auto it = kv.find(k);
        if (it == kv.end())
            throw BoardError("hyp: record (" + type + " ...) has no " + k + "=");
        return it->second;
    }
    double num(const std::string& k) const {
        const std::string& s = str(k);
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end != s.c_str() + s.size())
            throw BoardError("hyp: " + k + "=" + s + " is not a number");
        return v;
    }
    double num_or(const std::string& k, double dflt) const {
        return has(k) ? num(k) : dflt;
    }
};

struct Block {
    std::string name;                 // BOARD, STACKUP, NET, POLYGON, ...
    std::string value;                // NET=<value>, PADSTACK=<value>
    // header attributes: {POLYGON L="Bottom" T=POUR ID=1 X=.. Y=..} carries the
    // layer AND the polygon's first vertex before any (LINE ...) record
    std::map<std::string, std::string> kv;
    std::vector<Record> records;
    std::vector<Block> blocks;        // nested {POLYGON ...} inside {NET ...}

    bool has(const std::string& k) const { return kv.count(k) != 0; }
    const std::string& str(const std::string& k) const {
        auto it = kv.find(k);
        if (it == kv.end())
            throw BoardError("hyp: block {" + name + "} has no " + k + "=");
        return it->second;
    }
    double num(const std::string& k) const {
        const std::string& s = str(k);
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end != s.c_str() + s.size())
            throw BoardError("hyp: " + k + "=" + s + " is not a number");
        return v;
    }
};

class Parser {
  public:
    explicit Parser(std::string_view t) : t_(t) {}

    std::vector<Block> parse_all() {
        std::vector<Block> out;
        while (true) {
            skip_filler();
            if (pos_ >= t_.size()) break;
            if (t_[pos_] == '{') out.push_back(parse_block());
            else skip_line();
        }
        return out;
    }

  private:
    void skip_line() {
        while (pos_ < t_.size() && t_[pos_] != '\n') ++pos_;
        if (pos_ < t_.size()) ++pos_;
    }
    // whitespace, '*' comment lines, and trailing commentary between records
    void skip_filler() {
        while (pos_ < t_.size()) {
            char c = t_[pos_];
            if (std::isspace(static_cast<unsigned char>(c))) { ++pos_; continue; }
            if (c == '*') { skip_line(); continue; }
            if (c == '{' || c == '(' || c == '}') return;
            skip_line();   // commentary after a record
        }
    }

    Block parse_block() {
        ++pos_;  // '{'
        Block b;
        while (pos_ < t_.size() && !std::isspace(static_cast<unsigned char>(t_[pos_])) &&
               t_[pos_] != '}' && t_[pos_] != '(' && t_[pos_] != '=')
            b.name.push_back(t_[pos_++]);
        if (pos_ < t_.size() && t_[pos_] == '=') {
            ++pos_;
            b.value = read_value();
        }
        // header attributes on the same line, e.g. {POLYGON L="Top" T=POUR X=..}
        while (pos_ < t_.size() && t_[pos_] != '\n' && t_[pos_] != '(' &&
               t_[pos_] != '{' && t_[pos_] != '}') {
            if (std::isspace(static_cast<unsigned char>(t_[pos_]))) { ++pos_; continue; }
            std::string key;
            while (pos_ < t_.size() && t_[pos_] != '=' && t_[pos_] != '\n' &&
                   !std::isspace(static_cast<unsigned char>(t_[pos_])) &&
                   t_[pos_] != '(' && t_[pos_] != '{' && t_[pos_] != '}')
                key.push_back(t_[pos_++]);
            if (pos_ < t_.size() && t_[pos_] == '=') {
                ++pos_;
                std::string val = read_value();
                std::transform(key.begin(), key.end(), key.begin(), ::toupper);
                b.kv[key] = val;
            } else if (key.empty()) {
                break;
            }
            // a bare word in the header is commentary; keep scanning the line
        }
        while (true) {
            skip_filler();
            if (pos_ >= t_.size()) break;
            if (t_[pos_] == '}') { ++pos_; break; }
            if (t_[pos_] == '{') { b.blocks.push_back(parse_block()); continue; }
            if (t_[pos_] == '(') { b.records.push_back(parse_record()); continue; }
            skip_line();
        }
        return b;
    }

    std::string read_value() {
        std::string v;
        if (pos_ < t_.size() && t_[pos_] == '"') {
            ++pos_;
            while (pos_ < t_.size() && t_[pos_] != '"') v.push_back(t_[pos_++]);
            if (pos_ < t_.size()) ++pos_;
            return v;
        }
        while (pos_ < t_.size() && !std::isspace(static_cast<unsigned char>(t_[pos_])) &&
               t_[pos_] != ')' && t_[pos_] != '}')
            v.push_back(t_[pos_++]);
        return v;
    }

    Record parse_record() {
        ++pos_;  // '('
        Record r;
        bool first = true;
        while (pos_ < t_.size() && t_[pos_] != ')') {
            if (std::isspace(static_cast<unsigned char>(t_[pos_])) || t_[pos_] == ',') {
                ++pos_;
                continue;
            }
            std::string tok;
            while (pos_ < t_.size() && t_[pos_] != ')' && t_[pos_] != ',' &&
                   t_[pos_] != '=' && !std::isspace(static_cast<unsigned char>(t_[pos_])))
                tok.push_back(t_[pos_++]);
            if (pos_ < t_.size() && t_[pos_] == '=') {
                ++pos_;
                std::string val = read_value();
                std::transform(tok.begin(), tok.end(), tok.begin(), ::toupper);
                r.kv[tok] = val;
            } else if (first) {
                r.type = tok;              // record type: SEG, PIN, SIGNAL, ...
            } else if (!tok.empty()) {
                r.positional.push_back(tok);
            }
            first = false;
        }
        if (pos_ < t_.size()) ++pos_;      // ')'
        std::transform(r.type.begin(), r.type.end(), r.type.begin(), ::toupper);
        return r;
    }

    std::string_view t_;
    size_t pos_ = 0;
};

}  // namespace hyp

inline BoardIR import_hyp(const std::string& text) {
    std::vector<hyp::Block> blocks = hyp::Parser(text).parse_all();

    // ---- units ----
    double scale = 25.4;   // ENGLISH (inches) is the format's default
    bool units_seen = false;
    for (const auto& b : blocks) {
        if (b.name != "UNITS") continue;
        units_seen = true;
        std::string v = b.value;
        std::transform(v.begin(), v.end(), v.begin(), ::toupper);
        if (v.rfind("METRIC", 0) == 0) scale = 1.0;
        else if (v.rfind("ENGLISH", 0) == 0) scale = 25.4;
        else throw BoardError("hyp: unknown UNITS '" + b.value + "'");
    }
    if (!units_seen)
        throw BoardError("hyp: no {UNITS=...} block — refusing to guess "
                         "whether this board is in inches or millimetres");

    BoardIR board;
    board.stackup.source = "board-file";

    // ---- stackup ----
    const hyp::Block* stack = nullptr;
    for (const auto& b : blocks) if (b.name == "STACKUP") stack = &b;
    if (!stack)
        throw BoardError("hyp: no {STACKUP ...} block — Faraday needs the "
                         "layer stack to estimate impedance and coupling");
    for (const auto& r : stack->records) {
        if (r.type == "SIGNAL" || r.type == "PLANE") {
            std::string name = r.has("L") ? r.str("L")
                                          : "L" + std::to_string(board.copper_names.size());
            board.copper_names.push_back(name);
            board.stackup.layers.push_back(
                {LayerKind::Copper, name, r.num("T") * scale, std::nullopt,
                 r.type == "PLANE" ? "power" : "signal"});
        } else if (r.type == "DIELECTRIC") {
            // permittivity is C= in this format, not ER=
            if (!r.has("C"))
                throw BoardError("hyp: dielectric layer '" +
                                 (r.has("L") ? r.str("L") : std::string("?")) +
                                 "' has no C= (permittivity)");
            board.stackup.layers.push_back(
                {LayerKind::Dielectric, r.has("L") ? r.str("L") : "dielectric",
                 r.num("T") * scale, r.num("C"), ""});
        }
    }
    if (board.copper_names.empty())
        throw BoardError("hyp: {STACKUP} declares no SIGNAL or PLANE layers");

    auto cu_of = [&](const std::string& layer) -> int {
        return board.copper_ordinal(layer);
    };

    // ---- components ----
    for (const auto& b : blocks) {
        if (b.name != "DEVICES") continue;
        for (const auto& r : b.records) {
            if (!r.has("REF")) continue;
            Component c;
            c.reference = r.str("REF");
            c.value = r.has("VAL") ? r.str("VAL")
                    : r.has("NAME") ? r.str("NAME") : "";
            c.x = c.y = c.rot_deg = 0.0;   // absolute pin positions come from PIN
            board.components.push_back(std::move(c));
        }
    }

    // ---- padstacks: pad size per name (first copper entry wins) ----
    struct PadSize { double w = 0, h = 0; std::string layer; };
    std::map<std::string, PadSize> padstacks;
    for (const auto& b : blocks) {
        if (b.name != "PADSTACK" || b.value.empty()) continue;
        for (const auto& r : b.records) {
            // (layer, type, sx, sy, angle) — all positional, layer is r.type
            if (r.positional.size() < 3) continue;
            PadSize ps;
            ps.layer = r.type;
            ps.w = std::strtod(r.positional[1].c_str(), nullptr) * scale;
            ps.h = std::strtod(r.positional[2].c_str(), nullptr) * scale;
            padstacks[b.value] = ps;
            break;
        }
    }

    // ---- board outline ----
    double x1 = 1e30, y1 = 1e30, x2 = -1e30, y2 = -1e30;
    bool outline = false;
    for (const auto& b : blocks) {
        if (b.name != "BOARD") continue;
        for (const auto& r : b.records) {
            if (r.type != "PERIMETER_SEGMENT" && r.type != "PERIMETER_ARC") continue;
            outline = true;
            for (const char* k : {"X1", "X2"})
                if (r.has(k)) { double v = r.num(k) * scale;
                                x1 = std::min(x1, v); x2 = std::max(x2, v); }
            for (const char* k : {"Y1", "Y2"})
                if (r.has(k)) { double v = r.num(k) * scale;
                                y1 = std::min(y1, v); y2 = std::max(y2, v); }
            if (r.type == "PERIMETER_ARC") ++board.approximated_arcs;
        }
    }

    // ---- nets ----
    int next_net = 1;
    std::map<std::string, int> net_ids;
    auto net_id = [&](const std::string& name) {
        auto it = net_ids.find(name);
        if (it != net_ids.end()) return it->second;
        int id = next_net++;
        net_ids[name] = id;
        board.nets.push_back({id, name});
        return id;
    };

    for (const auto& b : blocks) {
        if (b.name != "NET" || b.value.empty()) continue;
        int net = net_id(b.value);
        for (const auto& r : b.records) {
            if (r.type == "SEG" || r.type == "ARC") {
                int cu = cu_of(r.str("L"));
                if (cu < 0) continue;
                board.segments.push_back({net, cu,
                    r.num("X1") * scale, r.num("Y1") * scale,
                    r.num("X2") * scale, r.num("Y2") * scale,
                    r.num("W") * scale});
                if (r.type == "ARC") ++board.approximated_arcs;
            } else if (r.type == "VIA") {
                // L1=/L2= name the span; absent means a through via
                int f = r.has("L1") ? cu_of(r.str("L1")) : 0;
                int t = r.has("L2") ? cu_of(r.str("L2"))
                                    : (int)board.copper_names.size() - 1;
                if (f < 0) f = 0;
                if (t < 0) t = (int)board.copper_names.size() - 1;
                if (f > t) std::swap(f, t);
                double d = r.num_or("D", 0.0) * scale;   // drill, when present
                if (d <= 0) ++board.vias_without_drill;
                board.vias.push_back({net, r.num("X") * scale, r.num("Y") * scale,
                                      r.num_or("S", d > 0 ? d * 2 : 0.6) * scale,
                                      d, f, t});
            } else if (r.type == "PIN") {
                Pad pad;
                // R=<refdes>.<pin>
                std::string ref = r.has("R") ? r.str("R") : "";
                size_t dot = ref.find('.');
                pad.component = dot == std::string::npos ? ref : ref.substr(0, dot);
                pad.net = net;
                pad.x = r.num("X") * scale;
                pad.y = r.num("Y") * scale;
                pad.w = pad.h = 0.0;
                pad.through_hole = false;
                pad.cu = 0;
                if (r.has("P")) {
                    auto it = padstacks.find(r.str("P"));
                    if (it != padstacks.end()) {
                        pad.w = it->second.w;
                        pad.h = it->second.h;
                        int c = cu_of(it->second.layer);
                        if (c >= 0) pad.cu = c;
                        else { pad.through_hole = true; pad.cu = -1; }
                    }
                }
                board.pads.push_back(std::move(pad));
            }
        }
        // polygons: {POLYGON L= T=POUR ...} with (LINE X= Y=) vertices.
        // POLYVOID blocks describe cut-outs; they are counted, not subtracted
        // (the screener's coverage test would need real boolean geometry).
        for (const auto& pb : b.blocks) {
            if (pb.name == "POLYVOID") { ++board.approximated_arcs; continue; }
            if (pb.name != "POLYGON" && pb.name != "POLYLINE") continue;
            if (!pb.has("L")) continue;          // layer lives in the header
            int cu = cu_of(pb.str("L"));
            if (cu < 0) continue;                // pour on a non-copper layer
            ZonePoly poly;
            poly.net = net;
            poly.cu = cu;
            // the header also carries the polygon's FIRST vertex
            if (pb.has("X") && pb.has("Y"))
                poly.pts.push_back({pb.num("X") * scale, pb.num("Y") * scale});
            for (const auto& r : pb.records) {
                if (r.type != "LINE" && r.type != "CURVE") continue;
                poly.pts.push_back({r.num("X") * scale, r.num("Y") * scale});
                if (r.type == "CURVE") ++board.approximated_arcs;
            }
            if (poly.pts.size() < 3) continue;
            board.zones.push_back(std::move(poly));
        }
    }

    if (!outline) {
        for (const auto& s : board.segments) {
            x1 = std::min({x1, s.x1, s.x2}); y1 = std::min({y1, s.y1, s.y2});
            x2 = std::max({x2, s.x1, s.x2}); y2 = std::max({y2, s.y1, s.y2});
        }
        for (const auto& z : board.zones)
            for (const auto& p : z.pts) {
                x1 = std::min(x1, p.x); y1 = std::min(y1, p.y);
                x2 = std::max(x2, p.x); y2 = std::max(y2, p.y);
            }
        for (const auto& p : board.pads) {
            x1 = std::min(x1, p.x); y1 = std::min(y1, p.y);
            x2 = std::max(x2, p.x); y2 = std::max(y2, p.y);
        }
    }
    if (x1 > x2) throw BoardError("hyp: board has no geometry at all");
    board.bbox_x1 = x1; board.bbox_y1 = y1;
    board.bbox_x2 = x2; board.bbox_y2 = y2;
    board.bbox_from_outline = outline;
    return board;
}

}  // namespace faraday
