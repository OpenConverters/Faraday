#pragma once
// Shielding effectiveness, decomposed the way it actually behaves.
//
// The single most useful thing this header encodes is that SE has two regimes
// with opposite sensitivities, and quoting one number hides that:
//
//   * At LOW frequency the wall is the limit. A 0.2 mm shield gives 59 dB in
//     tin-plated steel and 10 dB in brass at 500 kHz — a 49 dB difference from
//     permeability alone. Material choice is everything.
//   * At HIGH frequency the wall is opaque and irrelevant. The same steel wall
//     gives 948 dB of absorption at 130 MHz, which is not a number, it means
//     "infinite". What you actually get is set by the SEAM. Material choice is
//     worth nothing; contact pitch is worth everything.
//
// So a can specified only by its metal is specified in the wrong variable for
// most switching-converter work.
//
// AND THE PART THAT IS EASIEST TO GET WRONG. Shielding effectiveness depends
// on the WAVE IMPEDANCE of the field, which near a source is set by the source
// and not by the medium. Reflection loss is enormous for a high-impedance
// electric field and poor for a low-impedance magnetic one, and the two differ
// by ten orders of magnitude at millimetre distances. A single "SE = 50 dB"
// figure applied to both is wrong by ~100 dB in one direction or the other.
// Nothing here returns an SE without being told which field it is shielding.
//
// Formulas: Schelkunoff decomposition SE = A + R + M (Ott, EMC Engineering
// ch. 6). Absorption A = 8.686 * t / delta with delta = 66100/sqrt(f mu_r
// sigma_r) in micrometres — verified against the published copper ladder
// 66.1 / 12.1 / 6.61 / 2.09 um at 1 / 30 / 100 / 1000 MHz.

#include "NearField.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace faraday::shield {

// Conductivity and permeability relative to copper / free space. Steel's
// permeability is the reason a steel can beats a brass one by ~49 dB at
// 500 kHz and by nothing at all at 130 MHz.
struct Material {
    const char* id;
    const char* label;
    double sigma_r;    // relative to copper
    double mu_r;
    // The frequency to which the quoted mu_r is meaningful. High-permeability
    // alloys roll off hard — mu-metal's 20000 is a DC/audio figure and is long
    // gone by 1 MHz — and that roll-off is the actual reason they are not used
    // at HF, not conductivity. Carrying a constant mu_r past this point makes
    // mu-metal look like the best HF shield there is, which is backwards.
    double mu_valid_to_hz;
    const char* note;
};

inline const std::vector<Material>& materials() {
    static const std::vector<Material> m = {
        {"tinsteel", "Tin-plated steel", 0.10, 100.0, 10e6,
         "the usual shield-can metal; mu_r ~100-300 holds well into the MHz range"},
        {"brass", "Brass", 0.28, 1.0, 1e12,
         "non-magnetic: markedly weaker below a few MHz, identical above"},
        {"copper", "Copper", 1.00, 1.0, 1e12,
         "best conductor, no permeability, so no low-frequency magnetic help"},
        {"alu", "Aluminium", 0.61, 1.0, 1e12, "non-magnetic, light, poor at LF magnetic"},
        {"mumetal", "Mu-metal", 0.03, 20000.0, 1e5,
         "for LOW-frequency magnetic fields, where conductors fail. The 20000 is "
         "a DC/audio figure: it collapses above ~100 kHz, and the alloy saturates "
         "easily, so it is the wrong answer for a switching-node ring"},
    };
    return m;
}

inline const Material& material_by_id(const std::string& id) {
    for (const auto& m : materials())
        if (id == m.id) return m;
    throw std::invalid_argument(
        "shield: unknown material '" + id +
        "' — conductivity and permeability decide the answer at low frequency, "
        "so they cannot be guessed");
}

// Skin depth, micrometres.
inline double skin_depth_um(double f_hz, const Material& m) {
    if (!(f_hz > 0)) throw std::invalid_argument("shield: frequency must be > 0");
    return 66100.0 / std::sqrt(f_hz * m.mu_r * m.sigma_r);
}

// Absorption loss. This is the term that goes to hundreds of dB in the HF
// limit, which is exactly why the metal stops mattering there.
inline double absorption_db(double t_mm, double f_hz, const Material& m) {
    if (!(t_mm > 0)) throw std::invalid_argument("shield: wall thickness must be > 0");
    return 8.686 * (t_mm * 1000.0) / skin_depth_um(f_hz, m);
}

// ---------------------------------------------------------------------------
// Aperture and seam leakage — the real limit at HF
// ---------------------------------------------------------------------------
//
//   SE = 20 log10(lambda / 2 L)
//
// with L the LONGEST aperture dimension. For a two-part can the aperture is
// not a hole, it is the gap between discrete cover-to-frame contacts, so L is
// the contact PITCH. That is the number a can should be selected on above a
// few MHz, and it is rarely the number on the front of the datasheet.
//
// The bound is deliberately pessimistic for narrow slots (a max-linear-
// dimension rule), and it breaks down as the aperture approaches lambda/2 —
// beyond which the aperture resonates and the shield can be worse than nothing.
inline double aperture_se_db(double f_hz, double seam_mm) {
    if (!(seam_mm > 0)) throw std::invalid_argument("shield: seam length must be > 0");
    const double lam_mm = nf::wavelength(f_hz) * 1e3;
    if (2.0 * seam_mm >= lam_mm)
        throw std::invalid_argument(
            "shield: the seam is at least half a wavelength at this frequency — "
            "it is an antenna, not an aperture, and this bound does not apply");
    return 20.0 * std::log10(lam_mm / (2.0 * seam_mm));
}

// Frequency at which a seam reaches lambda/2 and stops being an aperture.
inline double seam_resonance_hz(double seam_mm) {
    if (!(seam_mm > 0)) throw std::invalid_argument("shield: seam length must be > 0");
    return nf::C0 / (2.0 * seam_mm * 1e-3);
}

// ---------------------------------------------------------------------------
// The verdict
// ---------------------------------------------------------------------------

enum class FieldKind { MagneticNear, ElectricNear, PlaneWave };

// A shield can drawn over part of the board, carrying the SE its own material,
// wall and seam earn at the frequency in question — never a flat assumption.
struct Rect {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;   // mm
    double se_db = 0;
    bool contains(double x, double y) const {
        return x >= std::min(x1, x2) && x <= std::max(x1, x2) &&
               y >= std::min(y1, y2) && y <= std::max(y1, y2);
    }
};

struct Can {
    std::string material = "tinsteel";
    double wall_mm = 0.2;
    double seam_pitch_mm = 5.0;   // cover-to-frame contact spacing
    bool five_sided = true;       // the PCB is the sixth wall
    // Permeability override: 0 = the material's own. Vendors sell the same
    // shield in several permeability GRADES (Würth's magnetic-shielding line
    // quotes µ′(f) curves per grade); entering µ′ read off the datasheet AT
    // the analysis frequency replaces the material's generic figure — and
    // because it is a value for THIS frequency, the roll-off extrapolation
    // flag does not apply to it.
    double mu_r = 0;
};

struct Verdict {
    double absorption_db = 0;
    double aperture_db = 0;
    double se_db = 0;             // what you actually get
    double skin_depth_um = 0;
    double walls_per_skin = 0;
    std::string limited_by;       // "wall" or "seam"
    std::string caveat;
    // True when the frequency is past the point where the material's quoted
    // permeability still holds. The absorption figure is then an overestimate
    // and must not be shown as an answer.
    bool permeability_extrapolated = false;
};

inline Verdict evaluate(const Can& can, double f_hz, FieldKind kind) {
    Material m = material_by_id(can.material);
    if (can.mu_r != 0) {
        if (can.mu_r < 1.0)
            throw std::invalid_argument(
                "shield: permeability override must be >= 1 (relative), got " +
                std::to_string(can.mu_r));
        m.mu_r = can.mu_r;
        m.mu_valid_to_hz = 1e12;   // user-supplied at this frequency
    }
    Verdict v;
    v.permeability_extrapolated = f_hz > m.mu_valid_to_hz;
    v.skin_depth_um = skin_depth_um(f_hz, m);
    v.absorption_db = absorption_db(can.wall_mm, f_hz, m);
    v.walls_per_skin = (can.wall_mm * 1000.0) / v.skin_depth_um;
    v.aperture_db = aperture_se_db(f_hz, can.seam_pitch_mm);

    // Reflection loss is the term that separates the three field types, and it
    // is where a single-number SE goes wrong by ~100 dB. Rather than emit a
    // reflection figure whose near-field form is only valid in narrow windows,
    // the model takes the binding constraint and names it.
    switch (kind) {
        case FieldKind::MagneticNear:
            // Low wave impedance: reflection is poor and the shield works by
            // absorption and by eddy currents opposing the flux. Below about a
            // skin depth of wall there is essentially nothing there, which is
            // the honest reason a thin can does little against LF magnetic.
            if (v.walls_per_skin < 1.0) {
                v.se_db = std::min(v.absorption_db, v.aperture_db);
                v.limited_by = "wall";
                v.caveat =
                    "the wall is thinner than one skin depth, so eddy currents "
                    "barely develop; against a low-frequency magnetic field a "
                    "thin can gives single-digit dB whatever it is made of";
            } else {
                v.se_db = std::min(v.absorption_db, v.aperture_db);
                v.limited_by = v.absorption_db < v.aperture_db ? "wall" : "seam";
            }
            break;
        case FieldKind::ElectricNear:
            // High wave impedance: reflection is essentially total and the
            // metal is never the limit. What matters is the bond to reference —
            // an ungrounded can is a coupling plate, not a shield.
            v.se_db = v.aperture_db;
            v.limited_by = "seam";
            v.caveat =
                "against an electric near field reflection is essentially total, "
                "so the metal never limits; the bond to reference does, and an "
                "ungrounded can couples rather than shields";
            break;
        case FieldKind::PlaneWave:
            v.se_db = std::min(v.absorption_db, v.aperture_db);
            v.limited_by = v.absorption_db < v.aperture_db ? "wall" : "seam";
            break;
    }
    if (v.permeability_extrapolated) {
        const std::string extra =
            std::string("above ~") +
            std::to_string((long long)(m.mu_valid_to_hz / 1e6)) +
            " MHz the quoted permeability of this material no longer holds, so "
            "the wall figure is an overestimate — high-mu alloys lose their "
            "permeability long before a conductor loses its conductivity";
        v.caveat = v.caveat.empty() ? extra : v.caveat + ". Note " + extra;
    }
    if (can.five_sided && kind == FieldKind::MagneticNear) {
        const std::string extra =
            "five-sided: the PCB is the sixth wall, so magnetic flux routes "
            "around through the board and the delivered figure is an upper bound";
        v.caveat = v.caveat.empty() ? extra : v.caveat + ". Also " + extra;
    }
    return v;
}

}  // namespace faraday::shield
