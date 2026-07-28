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

inline BoardFormat detect_format(const std::string& text) {
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
    switch (f) {
        case BoardFormat::Kicad: return import_kicad(text, std::move(user_stackup));
        case BoardFormat::Ipc2581: return import_ipc2581(text, std::move(user_stackup));
        case BoardFormat::Hyp: return import_hyp(text);
    }
    throw BoardError("import_board: unreachable");
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
        return odb::import_odb(files, std::move(user_stackup));
    }
    bool any_gerber = false;
    for (const auto& f : files)
        any_gerber = any_gerber || gerber::looks_gerber(f.text) ||
                     gerber::looks_excellon(f.text);
    if (files.size() == 1 && !any_gerber)
        return import_board(files[0].text, std::move(user_stackup), detected);
    if (!any_gerber)
        throw BoardError(
            "multiple files, none of them Gerber and no ODB++ matrix/matrix — "
            "Faraday takes one KiCad/HyperLynx/IPC-2581 file, a Gerber X2 "
            "set, or an ODB++ job (zip or directory).");
    if (detected) *detected = BoardFormat::GerberSet;
    return gerber::import_gerber_set(files, std::move(user_stackup));
}

}  // namespace faraday
