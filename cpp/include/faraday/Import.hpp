#pragma once
// One entry point for every board format Faraday reads. Detection is by
// CONTENT, not by file extension: the browser gets a dropped file with an
// arbitrary name, and IPC-2581 exports in particular are often just ".xml".

#include "BoardIR.hpp"
#include "HypImporter.hpp"
#include "IpcImporter.hpp"
#include "KicadImporter.hpp"

#include <optional>
#include <string>

namespace faraday {

enum class BoardFormat { Kicad, Hyp, Ipc2581 };

inline const char* format_name(BoardFormat f) {
    switch (f) {
        case BoardFormat::Kicad: return "kicad";
        case BoardFormat::Hyp: return "hyp";
        case BoardFormat::Ipc2581: return "ipc2581";
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
    throw BoardError(
        "unrecognised board file. Faraday reads KiCad (.kicad_pcb), "
        "HyperLynx (.hyp) and IPC-2581 (.xml); none of their signatures "
        "appear in this file.");
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

}  // namespace faraday
