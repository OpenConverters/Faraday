#pragma once
// Component value strings, parsed once for everybody.
//
// Lives on its own because three layers need it and the include direction
// forbids sharing it any other way: the screener (Y-capacitor resonance), the
// PDN model (decoupling branches) and the operating-point derivation (the
// input branch) all read the same fields, and Pdn.hpp already includes
// Screener.hpp. One definition, no per-caller re-implementation.

#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
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
//
// A value field usually carries MORE than the value — "220n 100V", "10uF/25V",
// "4.7µF 16V X7R" — and refusing those was refusing most of the real world.
// It is not a guess to read them: the leading field is a complete, unambiguous
// capacitance and what follows is a rating. So the string is split on its
// separators and the FIRST one or two fields are offered to the strict parser
// ("100 nF" is one value written with a space; "220n 100V" is a value and a
// rating). Everything after that is ignored, and anything that does not parse
// as a whole field is still refused.
inline std::optional<double> parse_si_strict(const std::string& v, char unit_letter);

inline std::optional<double> parse_si_value(const std::string& raw_in,
                                            char unit_letter) {
    std::string raw = normalize_micro(raw_in);
    for (char& c : raw)
        if (c == '/' || c == ',' || c == ';') c = ' ';
    std::vector<std::string> fields;
    for (size_t i = 0; i < raw.size();) {
        while (i < raw.size() && std::isspace((unsigned char)raw[i])) ++i;
        size_t j = i;
        while (j < raw.size() && !std::isspace((unsigned char)raw[j])) ++j;
        if (j > i) fields.push_back(raw.substr(i, j - i));
        i = j;
    }
    if (fields.empty()) return std::nullopt;
    if (auto one = parse_si_strict(fields[0], unit_letter)) return one;
    // "100 nF": one value written with a space in it
    if (fields.size() >= 2)
        if (auto two = parse_si_strict(fields[0] + fields[1], unit_letter))
            return two;
    return std::nullopt;
}

inline std::optional<double> parse_si_strict(const std::string& raw,
                                             char unit_letter) {
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
        // std::stod STOPS at the first character it cannot use and reports
        // success — "50v100" comes back as 50. That turned "50V 100n" (a
        // rating written before the value) into 50 nF: a wrong number, silently,
        // which is the one outcome this parser exists to prevent. Require the
        // WHOLE numeric field to be consumed.
        auto whole = [](const std::string& text) -> std::optional<double> {
            if (text.empty()) return std::nullopt;
            try {
                size_t used = 0;
                const double value = std::stod(text, &used);
                if (used != text.size()) return std::nullopt;
                return value;
            } catch (...) { return std::nullopt; }
        };
        if (!a.empty() && b.empty()) {
            if (auto x = whole(a)) return *x * m;
            return std::nullopt;
        }
        // IEC 60062 with no whole part: the multiplier IS the decimal point and
        // it leads. "u1" is 0.1 uF, "n47" is 0.47 nF — 73 of Glasgow's 92
        // capacitors are written this way, and refusing them left that board's
        // PDN almost entirely unmodelled. Unambiguous, so not a guess.
        if (a.empty() && !b.empty() &&
            b.find_first_not_of("0123456789") == std::string::npos) {
            if (auto x = whole("0." + b)) return *x * m;
        }
        if (!a.empty() && !b.empty() &&
            b.find_first_not_of("0123456789") == std::string::npos) {
            if (auto x = whole(a + "." + b)) return *x * m;
        }
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

// ---------------------------------------------------------------------------
// Values a CAD export did not carry
// ---------------------------------------------------------------------------
// A board can arrive with no component values at all: Altium's ODB++ writes the
// manufacturer PART NUMBER in the component record and nothing else, so every
// capacitor reaches the screener nameless. The values are not lost — they are
// in the catalogue that knows those part numbers — but they have to be handed
// back in. This reads the "refdes,value" table that does it.
//
// Only EMPTY values are filled. A value the board itself carries always wins:
// a side file must never quietly overrule what the layout says.
// ---------------------------------------------------------------------------
// What the CATALOGUE knows about a part that the board cannot
// ---------------------------------------------------------------------------
// A board gives geometry: where a capacitor sits, how far its pads escape to
// their vias. It cannot give ESR, and Faraday was using 0.015 ohm for every
// capacitor on every board — a constant that reaches the conducted-emissions
// maths through Operating's input branch, so the noise number carried it.
// Package-derived ESL is the same shape of guess, one table lookup wide.
//
// Kelvin holds the measured figures per part number. When the catalogue has
// identified a part EXACTLY, its values belong in the model in place of the
// guesses, and — because the two must never be confusable — each branch records
// which of its numbers came from a datasheet and which is still an assumption.
//
// Absence travels as NaN, never 0: a 0 ohm ESR is physically impossible and
// would read as a perfect capacitor. This mirrors Kelvin's own convention, and
// for the same reason (its ABT #455).
struct PartData {
    double c_f = std::numeric_limits<double>::quiet_NaN();
    double esr_ohm = std::numeric_limits<double>::quiet_NaN();
    double esr_freq_hz = std::numeric_limits<double>::quiet_NaN();
    double esl_h = std::numeric_limits<double>::quiet_NaN();
    // Semiconductors. Coss is the one the screener has been missing outright:
    // the commutation-loop finding computes a loop inductance and then stops at
    // "with the switch output capacitance this sets the ringing frequency",
    // because it had no Coss to finish the sentence with. The SPICE export says
    // the same thing in its own words — it lists device models under "absent".
    double coss_f = std::numeric_limits<double>::quiet_NaN();
    double qg_c = std::numeric_limits<double>::quiet_NaN();
    double rds_on_ohm = std::numeric_limits<double>::quiet_NaN();
    std::string mpn;        // the part the catalogue matched
    std::string source;     // "kelvin" — where it came from, for the report
};

inline bool has(double v) { return std::isfinite(v) && v > 0; }

struct PartTable {
    std::map<std::string, PartData> by_refdes;
    size_t applied = 0;
};

struct ValueTable {
    std::map<std::string, std::string> by_refdes;
    size_t applied = 0, ignored = 0;      // ignored = the board already had one
};

inline ValueTable parse_value_table(const std::string& text) {
    ValueTable t;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t comma = line.find(',');
        if (comma == std::string::npos) continue;
        auto trim = [](std::string s) {
            const char* ws = " \t\r\n\"";
            const size_t a = s.find_first_not_of(ws);
            const size_t b = s.find_last_not_of(ws);
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        const std::string ref = trim(line.substr(0, comma));
        const std::string val = trim(line.substr(comma + 1));
        if (ref.empty() || val.empty() || ref == "refdes") continue;
        t.by_refdes[ref] = val;
    }
    if (t.by_refdes.empty())
        throw std::invalid_argument(
            "values: no 'refdes,value' rows found — the file is empty, or it is "
            "not the two-column table this expects");
    return t;
}

}  // namespace faraday::values
