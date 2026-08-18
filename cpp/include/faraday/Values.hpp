#pragma once
// Component value strings, parsed once for everybody.
//
// Lives on its own because three layers need it and the include direction
// forbids sharing it any other way: the screener (Y-capacitor resonance), the
// PDN model (decoupling branches) and the operating-point derivation (the
// input branch) all read the same fields, and Pdn.hpp already includes
// Screener.hpp. One definition, no per-caller re-implementation.

#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace faraday::values {

// The MICRO SIGN is not ASCII, and real boards are full of it. KiCad writes
// "390µF" (U+00B5, 0xC2 0xB5) and some libraries write Greek mu (U+03BC,
// 0xCE 0xBC); a parser that only knows 'u' quietly drops every electrolytic on
// the board. On the LibreSolar MPPT that was 780 uF of input bulk — three of
// the five capacitors on the converter's own input rail. Normalising the two
// encodings is not a guess: nothing else in a value field is spelled that way.
inline std::string normalize_micro(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = (unsigned char)raw[i];
        if (i + 1 < raw.size()) {
            const unsigned char d = (unsigned char)raw[i + 1];
            if ((c == 0xC2 && d == 0xB5) || (c == 0xCE && d == 0xBC)) {
                out += 'u';
                ++i;
                continue;
            }
        }
        out += raw[i];
    }
    return out;
}

// "100n", "4u7", "0.1uF", "2200p", "390µF". A BARE NUMBER IS REFUSED: a "100"
// is picofarads in one library and nanofarads in the next, and a wrong guess
// poisons every curve it feeds.
inline std::optional<double> parse_si_value(const std::string& raw_in,
                                            char unit_letter) {
    const std::string raw = normalize_micro(raw_in);
    std::string v;
    for (char c : raw)
        if (!std::isspace((unsigned char)c)) v += (char)std::tolower((unsigned char)c);
    if (v.empty()) return std::nullopt;
    if (v.size() > 1 && v.back() == (char)std::tolower((unsigned char)unit_letter))
        v.pop_back();
    if (v.empty()) return std::nullopt;
    auto mult = [](char c) -> double {
        switch (c) {
            case 'p': return 1e-12;
            case 'n': return 1e-9;
            case 'u': return 1e-6;
            case 'm': return 1e-3;
            default: return 0;
        }
    };
    for (size_t i = 0; i < v.size(); ++i) {
        const double m = mult(v[i]);
        if (m == 0) continue;
        const std::string a = v.substr(0, i), b = v.substr(i + 1);
        try {
            if (!a.empty() && b.empty()) return std::stod(a) * m;
            if (!a.empty() && !b.empty() &&
                b.find_first_not_of("0123456789") == std::string::npos)
                return std::stod(a + "." + b) * m;
        } catch (...) { return std::nullopt; }
        return std::nullopt;
    }
    return std::nullopt;
}

inline std::optional<double> parse_capacitance(const std::string& raw) {
    return parse_si_value(raw, 'f');
}

inline std::optional<double> parse_inductance(const std::string& raw) {
    return parse_si_value(raw, 'h');
}

// ESL by package size, read from the footprint name. Order matters: "1210"
// contains "121", so match longest first.
inline double esl_from_footprint(const std::string& fp) {
    static const std::vector<std::pair<const char*, double>> table = {
        {"01005", 0.25e-9}, {"0201", 0.3e-9}, {"0402", 0.4e-9},
        {"0603", 0.5e-9},   {"0805", 0.7e-9}, {"1206", 1.0e-9},
        {"1210", 1.2e-9},   {"1812", 1.6e-9}, {"2220", 2.0e-9},
    };
    for (const auto& [k, v] : table)
        if (fp.find(k) != std::string::npos) return v;
    return 1.0e-9;   // unknown package: mid-of-road, stated in the output
}

}  // namespace faraday::values
