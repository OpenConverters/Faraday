#pragma once
// IPC-D-356(A) reader — the netlist that ships INSIDE most fabrication
// outputs. An Altium "Gerber_NCdrills_IPC" export (every TI EVM/TIDA design
// zip) carries one .ipc member with, per pad record: net name, refdes, pin,
// position, size, access layer. That is exactly what plain RS-274X lacks:
// with it, a classic Gerber set gets exact nets, components AND pin names —
// the same information an ODB++ job provides — instead of being refused for
// missing X2 attributes.
//
// Format (fixed columns, IPC-D-356A):
//   317/327 <net 14ch>   <ref 6ch>-<pin 4ch> ... A##X±######Y±######X####Y####R###
//   317 = through-hole (has D#### drill), 327 = SMD. "N/C" = unconnected.
//   P NNAME<n> <long name> aliases net fields that exceed 14 characters.
//   P UNITS CUST 0 -> 0.0001 inch, CUST 1 -> 0.001 mm (both seen in the
//   wild; the caller cross-checks against the Gerber extents, which carry
//   their own unit statement, and refuses a mismatch rather than guessing).

#include "BoardIR.hpp"

#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace faraday::ipc356 {

struct Record {
    std::string net, ref, pin;
    double x = 0, y = 0, w = 0, h = 0;   // mm
    bool through_hole = false;
    int access = 0;                       // 0 = both sides, 1 = top, n = layer n
};

struct Netlist {
    std::vector<Record> records;
    int skipped = 0;                      // unparsable record lines, counted
};

inline bool looks_ipc356(const std::string& text) {
    // the header P-records or a 317/327 body line near the top
    const size_t n = std::min<size_t>(text.size(), 4096);
    const std::string head = text.substr(0, n);
    return head.find("IPC-D-356") != std::string::npos ||
           head.find("P  JOB") != std::string::npos ||
           head.find("P JOB") != std::string::npos;
}

namespace detail {

inline std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b])) ++b;
    return s.substr(b);
}

// signed coordinate field: sign or leading space means +
inline std::optional<double> coord(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try {
        return std::stod(s);
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace detail

inline Netlist parse(const std::string& text) {
    Netlist out;
    std::map<std::string, std::string> alias;   // NNAMEn -> long name
    double unit = 0.0;                          // mm per file count
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.size() < 3) continue;
        if (line[0] == 'P') {
            const std::string body = detail::trim(line.substr(1));
            if (body.rfind("UNITS", 0) == 0) {
                // "UNITS CUST"/"UNITS CUST 0" = 0.0001 inch; "CUST 1" =
                // 0.001 mm; "CUST 2" = 0.0001 inch (A revision)
                unit = body.find("CUST 1") != std::string::npos ? 0.001
                                                                : 0.00254;
            } else if (body.rfind("NNAME", 0) == 0) {
                const size_t sp = body.find_first_of(" \t");
                if (sp != std::string::npos)
                    alias[body.substr(0, sp)] =
                        detail::trim(body.substr(sp + 1));
            }
            continue;
        }
        const std::string op = line.substr(0, 3);
        if (op != "317" && op != "327") continue;
        if (unit == 0.0) {
            // a record before any UNITS statement: refuse to guess
            ++out.skipped;
            continue;
        }
        if (line.size() < 40) { ++out.skipped; continue; }
        Record r;
        r.net = detail::trim(line.substr(3, 14));
        if (auto it = alias.find(r.net); it != alias.end()) r.net = it->second;
        if (r.net == "N/C") r.net.clear();
        r.ref = detail::trim(line.substr(20, 6));
        if (line.size() > 26 && line[26] == '-')
            r.pin = detail::trim(line.substr(27, 4));
        r.through_hole = op == "317";
        // access layer + coordinates live at variable offsets past col 31 —
        // scan for the markers rather than trusting exporter spacing
        const std::string tail = line.substr(31);
        size_t ax = tail.find('A');
        if (ax != std::string::npos && ax + 2 < tail.size())
            r.access = std::atoi(tail.substr(ax + 1, 2).c_str());
        const size_t X = tail.find('X', ax == std::string::npos ? 0 : ax);
        const size_t Y = tail.find('Y', X == std::string::npos ? 0 : X);
        if (X == std::string::npos || Y == std::string::npos) {
            ++out.skipped;
            continue;
        }
        auto xv = detail::coord(tail.substr(X + 1, Y - X - 1));
        size_t X2 = tail.find('X', Y);
        // The Y value runs to the end of its own NUMBER. It used to be
        // delimited by the next 'X' (assuming a pad-size field follows) or
        // failing that by the next space — which assumes the sign is written
        // as '+'/'-' rather than as the leading blank the format also allows.
        // A record with NEITHER a size field NOR an explicit sign satisfied
        // both fallbacks wrongly: the "next space" was the sign blank itself,
        // one character in, so the field came out empty and the whole record
        // was skipped. Every record of such a netlist was dropped, and the set
        // then read as "no IPC-D-356 netlist" rather than as a parse failure.
        // Scanning the number itself has no such assumption.
        size_t ye = Y + 1;
        if (ye < tail.size() &&
            (tail[ye] == '+' || tail[ye] == '-' || tail[ye] == ' '))
            ++ye;
        while (ye < tail.size() && std::isdigit((unsigned char)tail[ye])) ++ye;
        auto yv = detail::coord(tail.substr(Y + 1, ye - (Y + 1)));
        if (!xv || !yv) { ++out.skipped; continue; }
        r.x = *xv * unit;
        // y stays y-up: the Gerber importer (this reader's consumer) keeps
        // the fab files' own frame end to end, so the seeds must too
        r.y = *yv * unit;
        if (X2 != std::string::npos) {
            size_t Y2 = tail.find('Y', X2);
            auto wv = detail::coord(tail.substr(
                X2 + 1,
                (Y2 == std::string::npos ? std::string::npos : Y2 - X2 - 1)));
            if (wv) r.w = *wv * unit;
            if (Y2 != std::string::npos) {
                size_t e = tail.find_first_of("R S", Y2 + 1);
                auto hv = detail::coord(tail.substr(
                    Y2 + 1,
                    e == std::string::npos ? std::string::npos : e - Y2 - 1));
                if (hv) r.h = *hv * unit;
            }
        }
        if (r.h == 0) r.h = r.w;
        out.records.push_back(std::move(r));
    }
    return out;
}

}  // namespace faraday::ipc356
