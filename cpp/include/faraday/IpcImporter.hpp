#pragma once
// IPC-2581 (revision B/C) -> BoardIR.
//
// IPC-2581 is the XML CAD-to-CAM interchange standard, and as of 2026 no open
// source reader exists — only closed viewers. It carries everything Faraday
// needs: layer stack, netlist, per-net copper geometry and component pins.
//
// Structure consumed here:
//   <IPC-2581 revision="C">
//     <Ecad><CadHeader units="MILLIMETER">
//       <Spec name=".." .../>                       (materials, sometimes Er)
//     </CadHeader>
//     <CadData>
//       <Layer name="TOP" layerFunction="CONDUCTOR" side="TOP" .../>
//       <Stackup><StackupGroup>
//         <StackupLayer layerOrGroupRef="TOP" materialType="COPPER" thickness=".."/>
//       </StackupGroup></Stackup>
//       <Step name="..">
//         <Profile><Polygon>..</Polygon></Profile>
//         <Component refDes="R1" layerRef="TOP"><Location x=".." y=".."/></Component>
//         <LogicalNet name="GND"><PinRef componentRef="R1" pin="1"/></LogicalNet>
//         <LayerFeature layerRef="TOP">
//           <Set net="GND"><Features><Location x= y=/>
//             <Line startX= startY= endX= endY=><LineDescRef id=".."/></Line>
//             <Polygon><PolyBegin x= y=/><PolyStepSegment x= y=/>..</Polygon>
//           </Features></Set>
//         </LayerFeature>
//       </Step>
//     </CadData></Ecad>
//   </IPC-2581>
//
// Line width lives in the LineDesc dictionary, referenced by id.
// Permittivity has no fixed home in the schema (it appears as <Attribute> or
// <Spec> free text), so when it cannot be found the import REFUSES and asks
// for an explicit stackup rather than inventing a number.

#include "BoardIR.hpp"
#include "Xml.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>

namespace faraday {

namespace ipc {

inline std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

inline bool is_conductor(const std::string& fn) {
    std::string f = upper(fn);
    return f == "CONDUCTOR" || f == "SIGNAL" || f == "PLANE" || f == "POWER" ||
           f == "GROUND" || f == "POWER_GROUND" || f == "MIXED";
}

inline bool is_dielectric(const std::string& fn) {
    std::string f = upper(fn);
    return f.rfind("DIEL", 0) == 0;   // DIELCORE, DIELPREG, DIELECTRIC
}

// Permittivity from an element's <Attribute name=".." value=".."/> children or
// from a matching <Spec>. Recognises the names real exporters use.
inline std::optional<double> find_permittivity(const XmlNode& n) {
    for (const auto* a : n.descendants("Attribute")) {
        std::string name = upper(a->attr_or("name", ""));
        if (name.find("DIELECTRIC_CONSTANT") == std::string::npos &&
            name.find("PERMITTIVITY") == std::string::npos &&
            name != "ER" && name.find("EPSILON") == std::string::npos)
            continue;
        std::string v = a->attr_or("value", a->attr_or("stringValue", ""));
        if (v.empty()) continue;
        char* end = nullptr;
        double d = std::strtod(v.c_str(), &end);
        if (end != v.c_str() && d >= 1.0) return d;
    }
    return std::nullopt;
}

}  // namespace ipc

// user_stackup overrides whatever the file carries, exactly as for KiCad.
inline BoardIR import_ipc2581(const std::string& text,
                              std::optional<Stackup> user_stackup = std::nullopt) {
    XmlNode root = parse_xml(text);
    if (root.name != "IPC-2581")
        throw BoardError("ipc2581: root element is <" + root.name +
                         ">, expected <IPC-2581>");

    const XmlNode* ecad = root.first("Ecad");
    if (!ecad) throw BoardError("ipc2581: no <Ecad> section");
    const XmlNode* cad = ecad->first("CadData");
    if (!cad) throw BoardError("ipc2581: no <CadData> section");

    // ---- units ----
    double scale = 1.0;
    {
        const XmlNode* hdr = ecad->first("CadHeader");
        std::string u = hdr ? ipc::upper(hdr->attr_or("units", "")) : "";
        if (u.empty())
            throw BoardError("ipc2581: <CadHeader> has no units= — refusing to "
                             "guess the unit of every coordinate in the file");
        if (u == "MILLIMETER" || u == "MILLIMETRE" || u == "MM") scale = 1.0;
        else if (u == "INCH" || u == "INCHES") scale = 25.4;
        else if (u == "MICRON" || u == "MICROMETER") scale = 0.001;
        else throw BoardError("ipc2581: unknown units='" + u + "'");
    }

    BoardIR board;

    // ---- layers: conductors in stack order ----
    std::map<std::string, const XmlNode*> layer_by_name;
    for (const auto* l : cad->all("Layer"))
        layer_by_name[l->attr_or("name", l->attr_or("id", ""))] = l;

    // StackupLayer order defines the physical stack; Layer gives the function.
    std::vector<const XmlNode*> stack_layers;
    for (const auto* sg : cad->descendants("StackupLayer")) stack_layers.push_back(sg);

    if (user_stackup) {
        board.stackup = std::move(*user_stackup);
        // copper names still come from the file, in stack order when available
        for (const auto* sl : stack_layers) {
            std::string ref = sl->attr_or("layerOrGroupRef", "");
            auto it = layer_by_name.find(ref);
            std::string mt = ipc::upper(sl->attr_or("materialType", ""));
            bool copper = mt == "COPPER" || mt == "CONDUCTOR" ||
                          (it != layer_by_name.end() &&
                           ipc::is_conductor(it->second->attr_or("layerFunction", "")));
            if (copper) board.copper_names.push_back(ref);
        }
        if (board.copper_names.empty())
            for (auto& [name, l] : layer_by_name)
                if (ipc::is_conductor(l->attr_or("layerFunction", "")))
                    board.copper_names.push_back(name);
    } else {
        if (stack_layers.empty())
            throw BoardError("ipc2581: no <StackupLayer> entries — pass an "
                             "explicit stackup");
        board.stackup.source = "board-file";
        for (const auto* sl : stack_layers) {
            std::string ref = sl->attr_or("layerOrGroupRef", "");
            double t = sl->num_or("thickness", 0.0) * scale;
            std::string mt = ipc::upper(sl->attr_or("materialType", ""));
            auto it = layer_by_name.find(ref);
            std::string fn = it != layer_by_name.end()
                           ? it->second->attr_or("layerFunction", "") : "";
            bool copper = mt == "COPPER" || mt == "CONDUCTOR" || ipc::is_conductor(fn);
            bool diel = !copper && (mt == "DIELECTRIC" || mt.rfind("DIEL", 0) == 0 ||
                                    ipc::is_dielectric(fn) || mt == "PREPREG" ||
                                    mt == "CORE" || mt == "FR4");
            if (copper) {
                board.copper_names.push_back(ref);
                std::string f = ipc::upper(fn);
                board.stackup.layers.push_back(
                    {LayerKind::Copper, ref, t, std::nullopt,
                     (f == "PLANE" || f == "POWER" || f == "GROUND" ||
                      f == "POWER_GROUND") ? "power" : "signal"});
            } else if (diel) {
                std::optional<double> er = ipc::find_permittivity(*sl);
                if (!er && it != layer_by_name.end())
                    er = ipc::find_permittivity(*it->second);
                if (!er)
                    throw BoardError(
                        "ipc2581: dielectric layer '" + ref + "' carries no "
                        "permittivity (IPC-2581 has no fixed field for it, and "
                        "this exporter did not write one). Pass an explicit "
                        "stackup instead of having Faraday invent a value.");
                board.stackup.layers.push_back(
                    {LayerKind::Dielectric, ref, t, er, ""});
            }
        }
    }
    if (board.copper_names.empty())
        throw BoardError("ipc2581: no conductor layers found");

    size_t n_cu_stack = board.stackup.copper_indices().size();
    if (n_cu_stack != board.copper_names.size())
        throw BoardError("ipc2581: stackup has " + std::to_string(n_cu_stack) +
                         " copper layers but the board has " +
                         std::to_string(board.copper_names.size()));

    auto cu_of = [&](const std::string& name) { return board.copper_ordinal(name); };

    // ---- line-width dictionary ----
    std::map<std::string, double> line_width;
    for (const auto* d : root.descendants("LineDesc")) {
        std::string id = d->attr_or("id", "");
        if (id.empty()) continue;
        line_width[id] = d->num_or("lineWidth", 0.0) * scale;
    }
    // <EntryLineDesc id=".."><LineDesc lineWidth=".."/></EntryLineDesc>
    for (const auto* e : root.descendants("EntryLineDesc")) {
        std::string id = e->attr_or("id", "");
        const XmlNode* d = e->first("LineDesc");
        if (!id.empty() && d) line_width[id] = d->num_or("lineWidth", 0.0) * scale;
    }

    const XmlNode* step = cad->first("Step");
    if (!step) throw BoardError("ipc2581: <CadData> has no <Step>");

    // ---- nets ----
    int next_net = 1;
    std::map<std::string, int> net_ids;
    auto net_id = [&](const std::string& name) {
        if (name.empty()) return -1;
        auto it = net_ids.find(name);
        if (it != net_ids.end()) return it->second;
        int id = next_net++;
        net_ids[name] = id;
        board.nets.push_back({id, name});
        return id;
    };
    for (const auto* ln : step->all("LogicalNet")) net_id(ln->attr_or("name", ""));

    // ---- components (+ pin nets from LogicalNet/PinRef) ----
    std::map<std::string, std::pair<double, double>> comp_pos;
    for (const auto* c : step->all("Component")) {
        Component comp;
        comp.reference = c->attr_or("refDes", "");
        comp.value = c->attr_or("part", c->attr_or("packageRef", ""));
        const XmlNode* loc = c->first("Location");
        comp.x = loc ? loc->num_or("x", 0.0) * scale : 0.0;
        comp.y = loc ? loc->num_or("y", 0.0) * scale : 0.0;
        const XmlNode* xf = c->first("Xform");
        comp.rot_deg = xf ? xf->num_or("rotation", 0.0) : 0.0;
        comp_pos[comp.reference] = {comp.x, comp.y};
        board.components.push_back(std::move(comp));
    }
    // Pin geometry is not required for the rules that use pads (component
    // prefix + net + position); the component location is a faithful stand-in
    // when the package pin offsets are not resolved.
    for (const auto* ln : step->all("LogicalNet")) {
        int net = net_id(ln->attr_or("name", ""));
        for (const auto* pr : ln->all("PinRef")) {
            std::string ref = pr->attr_or("componentRef", "");
            auto it = comp_pos.find(ref);
            if (it == comp_pos.end()) continue;
            Pad pad;
            pad.component = ref;
            pad.pin = pr->attr_or("pinRef", "");   // IPC-2581 names the pin
            pad.net = net;
            pad.x = it->second.first;
            pad.y = it->second.second;
            pad.w = pad.h = 0.0;
            pad.through_hole = false;
            pad.cu = 0;
            board.pads.push_back(std::move(pad));
        }
    }

    // ---- copper features per layer ----
    for (const auto* lf : step->all("LayerFeature")) {
        int cu = cu_of(lf->attr_or("layerRef", ""));
        if (cu < 0) continue;                    // non-copper layer feature
        for (const auto* set : lf->all("Set")) {
            int net = net_id(set->attr_or("net", ""));
            for (const auto* feats : set->all("Features")) {
                // <Location> shifts every geometry inside this Features block
                const XmlNode* loc = feats->first("Location");
                double ox = loc ? loc->num_or("x", 0.0) * scale : 0.0;
                double oy = loc ? loc->num_or("y", 0.0) * scale : 0.0;

                for (const auto* ln : feats->descendants("Line")) {
                    double w = 0.0;
                    if (const XmlNode* r = ln->first("LineDescRef")) {
                        auto it = line_width.find(r->attr_or("id", ""));
                        if (it != line_width.end()) w = it->second;
                    }
                    if (w <= 0.0) w = ln->num_or("lineWidth", 0.0) * scale;
                    if (w <= 0.0) continue;      // a zero-width line is not copper
                    board.segments.push_back({net, cu,
                        ox + ln->num("startX") * scale, oy + ln->num("startY") * scale,
                        ox + ln->num("endX") * scale,   oy + ln->num("endY") * scale, w});
                }
                for (const auto* pg : feats->descendants("Polygon")) {
                    ZonePoly poly;
                    poly.net = net;
                    poly.cu = cu;
                    if (const XmlNode* pb = pg->first("PolyBegin"))
                        poly.pts.push_back({ox + pb->num("x") * scale,
                                            oy + pb->num("y") * scale});
                    for (const auto& ch : pg->children) {
                        if (ch.name.rfind("PolyStep", 0) != 0) continue;
                        poly.pts.push_back({ox + ch.num_or("x", 0.0) * scale,
                                            oy + ch.num_or("y", 0.0) * scale});
                        if (ch.name == "PolyStepCurve") ++board.approximated_arcs;
                    }
                    if (poly.pts.size() >= 3) board.zones.push_back(std::move(poly));
                }
            }
        }
    }

    // ---- outline from <Profile> ----
    double x1 = 1e30, y1 = 1e30, x2 = -1e30, y2 = -1e30;
    bool outline = false;
    auto grow = [&](double x, double y) {
        x1 = std::min(x1, x); y1 = std::min(y1, y);
        x2 = std::max(x2, x); y2 = std::max(y2, y);
    };
    if (const XmlNode* prof = step->first("Profile")) {
        for (const auto* pt : prof->descendants("PolyBegin")) {
            outline = true;
            grow(pt->num("x") * scale, pt->num("y") * scale);
        }
        for (const auto* pt : prof->descendants("PolyStepSegment"))
            grow(pt->num_or("x", 0.0) * scale, pt->num_or("y", 0.0) * scale);
    }
    if (!outline) {
        for (const auto& s : board.segments) { grow(s.x1, s.y1); grow(s.x2, s.y2); }
        for (const auto& z : board.zones)
            for (const auto& p : z.pts) grow(p.x, p.y);
        for (const auto& p : board.pads) grow(p.x, p.y);
    }
    if (x1 > x2) throw BoardError("ipc2581: board has no geometry at all");
    board.bbox_x1 = x1; board.bbox_y1 = y1;
    board.bbox_x2 = x2; board.bbox_y2 = y2;
    board.bbox_from_outline = outline;
    return board;
}

}  // namespace faraday
