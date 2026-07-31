#pragma once
// A physical-plausibility gate on an imported board.
//
// WHY THIS EXISTS. An importer bug does not usually announce itself: it
// produces a board that parses, screens, and reports — just at the wrong
// scale, the wrong handedness, or missing a layer. The ODB++ unit default bug
// (2026-07-31) read an inch job as millimetres and turned a 92 x 80 mm PoE
// board into a 3.6 x 3.1 mm one with 5 um traces. Nothing threw. The screener
// ran happily and emitted 4 findings where there were 161, and every number
// in them was wrong by 25.4x. The user's only clue was that the answer looked
// thin.
//
// The checks here depend on NOTHING but fabrication reality, so no importer
// bug can satisfy them by accident. They are deliberately not "is this board
// well designed" — they are "could this artefact have been manufactured at
// all". Bounds are set well outside any real process so that an unusual board
// passes and only a broken import fails:
//
//   * conductor width: the tightest commercial process is ~0.075 mm (3 mil);
//     advanced HDI reaches ~0.05 mm. Below 0.02 mm nothing is etched.
//   * drilled hole: mechanical drills stop near 0.15 mm, laser microvias near
//     0.075 mm. Below 0.02 mm nothing is drilled.
//   * outline: a chip-scale module is a few mm on a side; a fab panel tops out
//     well under 1 m.
//
// Two tiers, deliberately: IMPOSSIBLE throws, because continuing would put
// confidently wrong numbers in front of an engineer. SUSPICIOUS is carried in
// the report so the reader sees it without the board being refused.

#include "BoardIR.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace faraday::plausible {

struct Note {
    bool fatal = false;
    std::string what;
};

namespace detail {

inline double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

// Fabrication floors. Anything below these was not made by any process.
constexpr double kMinConductorMm = 0.02;
constexpr double kMinHoleMm = 0.02;
// Outline bounds: smaller than a 01005-scale module, or larger than a panel.
constexpr double kMinOutlineMm = 3.0;
constexpr double kMaxOutlineMm = 1000.0;

}  // namespace detail

// The unit mistake this gate exists to catch is always a clean factor: a file
// in inches read as mm (or the reverse) is out by exactly 25.4. Saying so
// converts "these numbers are impossible" into "here is what went wrong".
inline std::string scale_hypothesis(double median_width_mm) {
    if (!(median_width_mm > 0)) return "";
    const double up = median_width_mm * 25.4;
    if (up >= 0.05 && up <= 5.0)
        return " Multiplying every dimension by 25.4 gives a median width of " +
               std::to_string(up).substr(0, 5) +
               " mm, which is ordinary — this board was almost certainly "
               "exported in inches and read as millimetres.";
    const double down = median_width_mm / 25.4;
    if (down >= 0.05 && down <= 5.0)
        return " Dividing every dimension by 25.4 gives a median width of " +
               std::to_string(down).substr(0, 5) +
               " mm, which is ordinary — this board was almost certainly "
               "exported in millimetres and read as inches.";
    return "";
}

inline std::vector<Note> check(const BoardIR& b) {
    std::vector<Note> notes;

    std::vector<double> widths;
    for (const auto& s : b.segments)
        if (s.width > 0) widths.push_back(s.width);
    const double w = detail::median(widths);
    if (!widths.empty() && w < detail::kMinConductorMm)
        notes.push_back(
            {true, "the median routed conductor is " + std::to_string(w).substr(0, 6) +
                       " mm. No PCB process etches below about 0.05 mm, so this "
                       "board was not imported at its true scale." +
                       scale_hypothesis(w)});

    std::vector<double> drills;
    for (const auto& v : b.vias)
        if (v.drill > 0) drills.push_back(v.drill);
    const double d = detail::median(drills);
    if (!drills.empty() && d < detail::kMinHoleMm)
        notes.push_back(
            {true, "the median drilled hole is " + std::to_string(d).substr(0, 6) +
                       " mm. Laser microvias stop near 0.075 mm and mechanical "
                       "drills near 0.15 mm, so this board was not imported at "
                       "its true scale." + scale_hypothesis(w)});

    const double bw = b.bbox_x2 - b.bbox_x1, bh = b.bbox_y2 - b.bbox_y1;
    const double big = std::max(bw, bh);
    if (big > 0 && big < detail::kMinOutlineMm)
        notes.push_back(
            {true, "the board outline is " + std::to_string(bw).substr(0, 5) + " x " +
                       std::to_string(bh).substr(0, 5) +
                       " mm — smaller than the smallest real PCB, so this board "
                       "was not imported at its true scale." + scale_hypothesis(w)});
    else if (big > detail::kMaxOutlineMm)
        notes.push_back(
            {true, "the board outline is " + std::to_string(bw).substr(0, 6) + " x " +
                       std::to_string(bh).substr(0, 6) +
                       " mm — larger than any fabrication panel, so this board "
                       "was not imported at its true scale." + scale_hypothesis(w)});

    // Not fatal, but worth surfacing: a board whose copper is all one net, or
    // which has no routing at all, screens to nothing and usually means the
    // net mapping or a layer was lost rather than that the board is empty.
    if (!b.segments.empty() && b.nets.size() <= 1)
        notes.push_back({false,
                         "every routed segment carries the same net — the net "
                         "mapping may not have survived the import"});

    return notes;
}

// Run the gate and refuse the board if anything is impossible. Returns the
// non-fatal notes as plain strings, for BoardIR to carry into the report.
inline std::vector<std::string> enforce(const BoardIR& b) {
    std::vector<std::string> rest;
    std::string fatal;
    for (const auto& n : check(b)) {
        if (n.fatal) fatal += (fatal.empty() ? "" : " ALSO: ") + n.what;
        else rest.push_back(n.what);
    }
    if (!fatal.empty()) throw BoardError("implausible board: " + fatal);
    return rest;
}

}  // namespace faraday::plausible
