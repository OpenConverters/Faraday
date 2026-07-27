#pragma once
// Screening-tier transmission-line closed forms. Every formula is cited and
// pinned by tests against published values. These are RANKING numbers with
// stated error bars, not sign-off numbers — the field-solver tier (OMFEM 2D
// RLGC + Kirchhoff/ngspice) refines selected pairs.
//
// All lengths in the same unit (mm throughout Faraday); results are Ω / ratios.

#include <cmath>
#include <stdexcept>

namespace faraday::tline {

inline constexpr double ETA0 = 376.730313668;  // free-space impedance, Ω

// ---- Microstrip: Hammerstad (1975) synthesis equations ----
// Hammerstad, "Equations for microstrip circuit design", Proc. 5th EuMC 1975;
// the form also adopted by IPC-2141A. Accuracy ~±1% for 0.1 < w/h < 10,
// eps_r < 16, thin conductors (t ≪ h; t neglected here — screening tier).

inline double microstrip_eps_eff(double w, double h, double eps_r) {
    if (w <= 0 || h <= 0 || eps_r < 1)
        throw std::invalid_argument("microstrip_eps_eff: w, h must be > 0, eps_r >= 1");
    double u = w / h;
    double ee = (eps_r + 1.0) / 2.0 +
                (eps_r - 1.0) / 2.0 * std::pow(1.0 + 12.0 / u, -0.5);
    if (u < 1.0) ee += (eps_r - 1.0) / 2.0 * 0.04 * (1.0 - u) * (1.0 - u);
    return ee;
}

inline double microstrip_z0(double w, double h, double eps_r) {
    double u = w / h;
    double ee = microstrip_eps_eff(w, h, eps_r);
    if (u <= 1.0)
        return 60.0 / std::sqrt(ee) * std::log(8.0 / u + u / 4.0);
    return ETA0 / (std::sqrt(ee) * (u + 1.393 + 0.667 * std::log(u + 1.444)));
}

// ---- Symmetric stripline: IPC-2141A (Cohn-derived) ----
// Z0 = 60/sqrt(eps_r) * ln(1.9 b / (0.8 w + t)),  b = plane-to-plane spacing.
// Valid for w/b < 2, t/b < 0.25 — outside that range it degrades gracefully
// but stays monotonic, which is all the screening rank needs.

inline double stripline_z0(double w, double b, double t, double eps_r) {
    if (w <= 0 || b <= 0 || eps_r < 1)
        throw std::invalid_argument("stripline_z0: w, b must be > 0, eps_r >= 1");
    if (0.8 * w + t >= 1.9 * b)
        throw std::invalid_argument("stripline_z0: trace too wide for gap (w/b out of range)");
    return 60.0 / std::sqrt(eps_r) * std::log(1.9 * b / (0.8 * w + t));
}

// ---- Crosstalk screening coefficient ----
// Johnson & Graham, "High-Speed Digital Design" (1993), §5: the coupled-noise
// quick estimate  x(d) ≈ K / (1 + (d/h)^2)  where d is the CENTER-to-center
// separation, h the height above the reference plane, and K the saturated
// near-end (backward) crosstalk bound for tightly coupled lines, K ≈ 0.25.
//
// This is the worst case: NEXT saturates for coupled length L_c > t_r·v/2 and
// we do not know t_r at screening time, so the SATURATED value is reported
// (confidence tier "screening-estimate", error bar stated as ±6 dB in the
// finding text). FEXT needs excitation (∝ L_c/t_r) and is deliberately NOT
// quantified at this tier — findings carry the coupled length instead.

inline constexpr double KB_SAT = 0.25;

// Same-layer edge coupling: d = center-center separation, h = height to the
// nearest reference plane.
inline double next_sat_edge(double d, double h) {
    if (d < 0 || h <= 0)
        throw std::invalid_argument("next_sat_edge: d >= 0, h > 0 required");
    double r = d / h;
    return KB_SAT / (1.0 + r * r);
}

// Broadside coupling (traces on adjacent signal layers running parallel):
// h_v = vertical dielectric separation between the two copper layers,
// d_lat = lateral centerline offset. Same Johnson form with the vertical
// spacing as the normalising height — a screening heuristic (stacked traces
// with zero offset hit the saturated bound, decays with lateral offset).
inline double next_sat_broadside(double d_lat, double h_v) {
    if (d_lat < 0 || h_v <= 0)
        throw std::invalid_argument("next_sat_broadside: d_lat >= 0, h_v > 0 required");
    double r = d_lat / h_v;
    return KB_SAT / (1.0 + r * r);
}

// ---- Resonance of an open-ended structure ----
// An open stub of physical length L is a quarter-wave resonator at
//   f = c / (4 L sqrt(eps_eff))
// where it transforms the open into a short and the structure radiates /
// loads the driver hardest. Length in mm, result in Hz.
inline constexpr double C0 = 299792458.0;  // m/s

inline double quarter_wave_hz(double len_mm, double eps_eff) {
    if (len_mm <= 0 || eps_eff < 1)
        throw std::invalid_argument("quarter_wave_hz: len > 0, eps_eff >= 1 required");
    return C0 / (4.0 * (len_mm * 1e-3) * std::sqrt(eps_eff));
}

// Effective permittivity seen by a via barrel passing through the board: the
// barrel is fully embedded in the laminate, so it is the bulk eps_r, not the
// microstrip mixed-media value.
inline double via_eps_eff(double eps_r) { return eps_r; }

inline double to_db(double ratio) {
    if (ratio <= 0) throw std::invalid_argument("to_db: ratio must be > 0");
    return 20.0 * std::log10(ratio);
}

}  // namespace faraday::tline
