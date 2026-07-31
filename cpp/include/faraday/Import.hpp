#pragma once
// One entry point for every board format Faraday reads. Detection is by
// CONTENT, not by file extension: the browser gets a dropped file with an
// arbitrary name, and IPC-2581 exports in particular are often just ".xml".

#include "BoardIR.hpp"
#include "GerberImporter.hpp"
#include "OdbImporter.hpp"
#include "HypImporter.hpp"
#include "IpcImporter.hpp"
#include "KicadImporter.hpp"
#include "Plausible.hpp"

#include <optional>
#include <string>

namespace faraday {

enum class BoardFormat { Kicad, Hyp, Ipc2581, GerberSet, Odb };

inline const char* format_name(BoardFormat f) {
    switch (f) {
        case BoardFormat::Kicad: return "kicad";
        case BoardFormat::Hyp: return "hyp";
        case BoardFormat::Ipc2581: return "ipc2581";
        case BoardFormat::GerberSet: return "gerber-x2";
        case BoardFormat::Odb: return "odb++";
    }
    return "?";
}

// Every format Faraday reads is TEXT. A binary file must be rejected on that
// fact alone, before any signature sniffing: looks_excellon() is satisfied by
// the bytes "M48" appearing anywhere with no "%FS", which an 8.9 MB Altium
// .PcbDoc hits by pure chance — the user then gets "this drill file uses
// fixed-format coordinates" about a file that is not a drill file at all.
inline bool looks_binary(const std::string& text) {
    const size_t n = std::min<size_t>(text.size(), 8192);
    return text.find('\0') < n;
}

// Altium/OrCAD native databases are OLE2 compound files, and that is by far
// the most likely binary to be dropped on Faraday — name it exactly.
inline bool looks_ole2(const std::string& text) {
    return text.compare(0, 8, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0;
}

inline BoardFormat detect_format(const std::string& text) {
    if (looks_binary(text))
        throw BoardError(
            looks_ole2(text)
                ? "this is a native CAD database (an OLE2 compound file — "
                  "Altium .PcbDoc, OrCAD .brd), not a board interchange "
                  "format. Faraday cannot read a vendor's binary project. "
                  "Export ODB++ or Gerber X2 from it and drop that."
                : "this file is binary; Faraday reads text board formats "
                  "(KiCad, HyperLynx, IPC-2581, Gerber X2, ODB++).");
    // look past leading whitespace/comments for the first meaningful token
    size_t n = std::min<size_t>(text.size(), 4096);
    std::string head = text.substr(0, n);
    if (head.find("(kicad_pcb") != std::string::npos) return BoardFormat::Kicad;
    if (head.find("IPC-2581") != std::string::npos ||
        (head.find("<?xml") != std::string::npos &&
         head.find("<IPC") != std::string::npos))
        return BoardFormat::Ipc2581;
    if (head.find("{VERSION=") != std::string::npos ||
        head.find("{UNITS=") != std::string::npos ||
        head.find("{STACKUP") != std::string::npos)
        return BoardFormat::Hyp;
    if (gerber::looks_gerber(head) || gerber::looks_excellon(head))
        throw BoardError(
            "this is a single Gerber/drill layer — a Gerber board is a SET "
            "(one file per copper layer + drills + profile). Drop the whole "
            "fabrication set, or a zip of it.");
    throw BoardError(
        "unrecognised board file. Faraday reads KiCad (.kicad_pcb), "
        "HyperLynx (.hyp), IPC-2581 (.xml) and Gerber X2 sets (all files "
        "or a zip); none of their signatures appear in this file.");
}

// Import any supported format. user_stackup, when given, overrides whatever
// the file carries (HYP always carries its own and ignores the override).
inline BoardIR import_board(const std::string& text,
                            std::optional<Stackup> user_stackup = std::nullopt,
                            BoardFormat* detected = nullptr) {
    BoardFormat f = detect_format(text);
    if (detected) *detected = f;
    BoardIR b;
    switch (f) {
        case BoardFormat::Kicad: b = import_kicad(text, std::move(user_stackup)); break;
        case BoardFormat::Ipc2581: b = import_ipc2581(text, std::move(user_stackup)); break;
        case BoardFormat::Hyp: b = import_hyp(text); break;
        default: throw BoardError("import_board: unreachable");
    }
    // EVERY import passes the plausibility gate. An importer bug that scales,
    // flips or truncates the board still parses and still screens — the gate
    // is the only thing between that and a confidently wrong report.
    b.plausibility_notes = plausible::enforce(b);
    return b;
}

// Import a SET of files. One file that is not Gerber → the single-file path;
// anything containing Gerber/Excellon → the X2 set importer. A multi-file
// drop with no Gerber in it is an error, not a guess.
inline BoardIR import_board_set(const std::vector<gerber::NamedFile>& files,
                                std::optional<Stackup> user_stackup = std::nullopt,
                                BoardFormat* detected = nullptr) {
    if (files.empty()) throw BoardError("import_board_set: no files");
    if (odb::is_odb_set(files)) {
        if (detected) *detected = BoardFormat::Odb;
        BoardIR b = odb::import_odb(files, std::move(user_stackup));
        b.plausibility_notes = plausible::enforce(b);
        return b;
    }
    // Binary members are never board data — a dropped project zip carries the
    // .PcbDoc, PDFs and spreadsheets alongside the export. Skip them rather
    // than letting a chance byte sequence sniff as Gerber.
    std::vector<gerber::NamedFile> text_files;
    for (const auto& f : files)
        if (!looks_binary(f.text)) text_files.push_back(f);
    if (text_files.empty())
        return import_board(files[0].text, std::move(user_stackup), detected);
    bool any_gerber = false;
    for (const auto& f : text_files)
        any_gerber = any_gerber || gerber::looks_gerber(f.text) ||
                     gerber::looks_excellon(f.text);
    if (text_files.size() == 1 && !any_gerber)
        return import_board(text_files[0].text, std::move(user_stackup), detected);
    if (!any_gerber)
        throw BoardError(
            "multiple files, none of them Gerber and no ODB++ matrix/matrix — "
            "Faraday takes one KiCad/HyperLynx/IPC-2581 file, a Gerber X2 "
            "set, or an ODB++ job (zip or directory).");
    if (detected) *detected = BoardFormat::GerberSet;
    BoardIR b = gerber::import_gerber_set(text_files, std::move(user_stackup));
    b.plausibility_notes = plausible::enforce(b);
    return b;
}

// ---------------------------------------------------------------------------
// Custom stackups
// ---------------------------------------------------------------------------

// A user-entered stackup, as JSON:
//   {"layers":[{"kind":"copper","name":"F.Cu","thicknessMm":0.035},
//              {"kind":"dielectric","name":"core","thicknessMm":1.51,
//               "epsilonR":4.5}, ...]}
// Validated strictly — the whole point of entering a custom stackup is
// accuracy, so nothing is defaulted or repaired: it must start and end with
// copper, strictly alternate, carry positive thicknesses everywhere and an
// epsilon_r >= 1 on every dielectric, or it is refused with the reason.
inline Stackup stackup_from_json(const nlohmann::json& j) {
    if (!j.contains("layers") || !j.at("layers").is_array() ||
        j.at("layers").empty())
        throw BoardError("custom stackup: 'layers' array required");
    Stackup s;
    s.source = "user:custom";
    bool expect_copper = true;
    for (const auto& lj : j.at("layers")) {
        StackLayer l;
        const std::string kind = lj.value("kind", "");
        if (kind != "copper" && kind != "dielectric")
            throw BoardError("custom stackup: layer kind must be 'copper' or "
                             "'dielectric', got '" + kind + "'");
        l.kind = kind == "copper" ? LayerKind::Copper : LayerKind::Dielectric;
        if ((l.kind == LayerKind::Copper) != expect_copper)
            throw BoardError(
                "custom stackup: layers must alternate copper/dielectric, "
                "starting and ending with copper");
        expect_copper = !expect_copper;
        l.name = lj.value("name", kind);
        l.thickness_mm = lj.value("thicknessMm", 0.0);
        if (!(l.thickness_mm > 0.0))
            throw BoardError("custom stackup: layer '" + l.name +
                             "' needs a positive thicknessMm");
        if (l.kind == LayerKind::Dielectric) {
            if (!lj.contains("epsilonR"))
                throw BoardError("custom stackup: dielectric '" + l.name +
                                 "' needs epsilonR");
            const double er = lj.at("epsilonR").get<double>();
            if (!(er >= 1.0))
                throw BoardError("custom stackup: dielectric '" + l.name +
                                 "' has epsilonR " + std::to_string(er) +
                                 " — must be >= 1");
            l.epsilon_r = er;
        } else {
            l.copper_type = "signal";
        }
        s.layers.push_back(std::move(l));
    }
    if (s.layers.back().kind != LayerKind::Copper)
        throw BoardError("custom stackup: must end with a copper layer");
    if (s.copper_indices().size() < 2)
        throw BoardError("custom stackup: needs at least 2 copper layers");
    return s;
}

// One resolver for every stackup spec a caller can hand us: "" -> none,
// "{...}" -> custom JSON, anything else -> a builtin name.
inline std::optional<Stackup> resolve_stackup(const std::string& spec) {
    if (spec.empty()) return std::nullopt;
    if (spec[0] == '{') return stackup_from_json(nlohmann::json::parse(spec));
    // "assumed:default-4layer" — the SAME builtin, but stamped so the report
    // never claims a dielectric the user chose. The layer count behind it is
    // read off the board; only the dielectric is assumed, and a reader of the
    // exported report has to be able to tell those apart.
    if (spec.rfind("assumed:", 0) == 0) {
        Stackup s = builtin_stackup(spec.substr(8));
        s.source = "assumed:" + spec.substr(8);
        return s;
    }
    return builtin_stackup(spec);
}

}  // namespace faraday
