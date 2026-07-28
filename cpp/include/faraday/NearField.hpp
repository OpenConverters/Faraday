#pragma once
// Component-scale near field: the quasi-static induction map.
//
// This is a DIFFERENT REGIME from the far-field estimate in Emissions.hpp, and
// the two must never be blended. The boundary is r = lambda/2pi:
//
//     1 MHz -> 47.7 m    30 MHz -> 1.59 m    100 MHz -> 477 mm    1 GHz -> 47.7 mm
//
// Every component-scale distance on a board (0.1-50 mm) below about a gigahertz
// therefore has k*r << 1, and the fields ARE the magnetostatic and electrostatic
// dipole fields — the radiation terms are ~100 dB down. At 5 mm and 30 MHz,
// k*r = 0.0031.
//
// THREE CONSEQUENCES THAT INVERT FAR-FIELD INTUITION
//
//   * Decay is 1/r^3, not 1/r: 18.06 dB per doubling of distance rather than
//     6.02. Doubling a keep-out buys 18 dB, which is the single most useful
//     fact a designer can have here.
//   * E and H are INDEPENDENT. Wave impedance is set by the source, not by the
//     medium: at 5 mm / 30 MHz it is 120 kohm for a voltage-driven source and
//     1.18 ohm for a current-driven one. The far-field identity
//     E[dBuV/m] = H[dBuA/m] + 51.5 dB is wrong by 40 to 55 dB in that window,
//     so this header never converts one field into the other.
//   * Which shield works is decided by source type, not by frequency alone.
//     A thin conductive can is excellent against a high-impedance electric
//     source and nearly useless against a low-frequency magnetic one.
//
// WHAT IT MUST NOT DO. There is no reliable near-field to far-field transform.
// Nothing here produces dBuV/m, a limit line, a margin or a verdict. The
// defensible outputs are rank order, relative dB, and "strongest on this board".
//
// Exact dipole fields follow Balanis 4-8a..c (electric) and 5-27a..c (magnetic).
// LearnEMC's published forms have three sign defects traceable to a missing
// minus in their Faraday step; they are not used.

#include <cmath>
#include <complex>
#include <stdexcept>
#include <string>
#include <vector>

namespace faraday::nf {

// Derived, never pasted: this makes the duality identity below exact by
// construction rather than exact to however many digits someone typed.
inline constexpr double MU0 = 4.0e-7 * 3.14159265358979323846;
inline constexpr double C0 = 299792458.0;
inline constexpr double ETA0 = MU0 * C0;          // 376.730313461771 ohm
inline constexpr double PI_N = 3.14159265358979323846;

inline double wavelength(double f_hz) {
    if (!(f_hz > 0)) throw std::invalid_argument("nf: frequency must be > 0");
    return C0 / f_hz;
}

// The near/far boundary for an ELECTRICALLY SMALL source. For a source whose
// largest dimension D is comparable to a wavelength the Fraunhofer distance
// 2D^2/lambda applies instead, and the true onset is the larger of the two.
inline double near_far_boundary(double f_hz) { return C0 / (2.0 * PI_N * f_hz); }

inline double far_field_onset(double f_hz, double d_m) {
    if (!(d_m >= 0)) throw std::invalid_argument("nf: source size must be >= 0");
    const double lam = wavelength(f_hz);
    return std::max(near_far_boundary(f_hz), 2.0 * d_m * d_m / lam);
}

inline double kr(double f_hz, double r_m) {
    if (!(r_m > 0)) throw std::invalid_argument("nf: distance must be > 0");
    return 2.0 * PI_N * r_m / wavelength(f_hz);
}

// ---------------------------------------------------------------------------
// Wave impedance — exact closed forms
// ---------------------------------------------------------------------------
//
// Evaluated in the plane of maximum radiation (theta = 90 deg). With x = k*r,
//
//   Z_w,E = eta0 * |1 - j/x - 1/x^2| / |1 - j/x|
//   Z_w,M = eta0 * |1 - j/x|         / |1 - j/x - 1/x^2|
//
// These are reciprocal about eta0 at EVERY r, exactly. That identity is the
// single best unit test for a near-field implementation: it cannot hold by
// accident, and it fails loudly if either dipole is wrong.

inline double wave_impedance_electric(double f_hz, double r_m) {
    const double x = kr(f_hz, r_m);
    const std::complex<double> j(0.0, 1.0);
    const std::complex<double> a = 1.0 - j / x - 1.0 / (x * x);
    const std::complex<double> b = 1.0 - j / x;
    return ETA0 * std::abs(a) / std::abs(b);
}

inline double wave_impedance_magnetic(double f_hz, double r_m) {
    const double x = kr(f_hz, r_m);
    const std::complex<double> j(0.0, 1.0);
    const std::complex<double> a = 1.0 - j / x - 1.0 / (x * x);
    const std::complex<double> b = 1.0 - j / x;
    return ETA0 * std::abs(b) / std::abs(a);
}

// ---------------------------------------------------------------------------
// Source validity — the gate, not a warning
// ---------------------------------------------------------------------------
//
// The point-dipole model is INVALID, not merely approximate, within a few
// source dimensions. Worked: at 5 mm from a 100 mm^2 loop the dipole model is
// wrong, not marginal — the exact equivalent-circular-loop field is 37.2 A/m
// and the first valid dipole point is about 28 mm. Between roughly 5 and 20 mm
// from a typical power inductor the real decay is closer to 1/r^2 (12 dB per
// doubling) than 1/r^3.
//
// House rule is no silent fallbacks, so the model REFUSES rather than
// extrapolating. `a_eff` is the equivalent radius sqrt(A/pi) for a loop.

inline constexpr double DIPOLE_VALID_RATIO = 5.0;

inline double effective_radius(double area_m2) {
    if (!(area_m2 > 0)) throw std::invalid_argument("nf: source area must be > 0");
    return std::sqrt(area_m2 / PI_N);
}

inline bool dipole_valid(double r_m, double area_m2) {
    return r_m >= DIPOLE_VALID_RATIO * effective_radius(area_m2);
}

inline double dipole_valid_from_m(double area_m2) {
    return DIPOLE_VALID_RATIO * effective_radius(area_m2);
}

// ---------------------------------------------------------------------------
// Magnetic dipole (current loop) — the quasi-static near zone
// ---------------------------------------------------------------------------

// m = N * I * A. The ranking metric is the CURRENT-AREA PRODUCT: publish the
// area (geometry, certain) separately from the current (an assumption), so the
// linear scaling and the uncertainty stay visible.
inline double magnetic_moment(double turns, double current_a, double area_m2) {
    if (!(turns > 0)) throw std::invalid_argument("nf: turns must be > 0");
    if (!(area_m2 > 0)) throw std::invalid_argument("nf: loop area must be > 0");
    if (current_a < 0) throw std::invalid_argument("nf: current must be >= 0");
    return turns * current_a * area_m2;
}

// |H| = m * sqrt(1 + 3 cos^2 theta) / (4 pi r^3)
//   theta = 0   : on the dipole axis, H = m / (2 pi r^3)   (the maximum)
//   theta = 90  : in the equatorial plane, H = m / (4 pi r^3)
inline double h_field(double moment_am2, double r_m, double theta_rad) {
    if (!(r_m > 0)) throw std::invalid_argument("nf: distance must be > 0");
    if (moment_am2 < 0) throw std::invalid_argument("nf: moment must be >= 0");
    const double c = std::cos(theta_rad);
    return moment_am2 * std::sqrt(1.0 + 3.0 * c * c) / (4.0 * PI_N * r_m * r_m * r_m);
}

inline double h_axial(double moment_am2, double r_m) {
    return h_field(moment_am2, r_m, 0.0);
}
inline double h_equatorial(double moment_am2, double r_m) {
    return h_field(moment_am2, r_m, PI_N / 2.0);
}

// ---------------------------------------------------------------------------
// Biot-Savart over a filamentary loop — exact at every distance
// ---------------------------------------------------------------------------
//
// The point-dipole law is only valid beyond a few source dimensions, and on a
// real board that exclusion is crippling: a 267 mm^2 commutation loop has
// a_eff = 9.2 mm, so the dipole is invalid within 46 mm — nearly half of a
// 100 mm board, and it excludes exactly the parts sitting next to the switcher
// that anyone would want to ask about.
//
// Biot-Savart over the loop's own polygon has no such restriction. For a
// straight segment from A to B carrying current I, with a = A - P and
// b = B - P at the field point P:
//
//     H = (I / 4pi) * (a x b) (|a| + |b|) / (|a| |b| (|a| |b| + a.b))
//
// Summing that over the polygon is EXACT for a filamentary loop at any
// distance outside the conductor itself. It reduces to I/(2 pi d) beside a
// conductor and to the dipole m/(4 pi r^3) far away — both limits are asserted
// in the tests, and the dipole convergence is what proves the two models are
// the same physics rather than two guesses.

struct Vec3 { double x = 0, y = 0, z = 0; };

inline double h_segment(const Vec3& A, const Vec3& B, const Vec3& P,
                        double current_a) {
    const Vec3 a{A.x - P.x, A.y - P.y, A.z - P.z};
    const Vec3 b{B.x - P.x, B.y - P.y, B.z - P.z};
    const double na = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const double nb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    if (!(na > 0) || !(nb > 0))
        throw std::invalid_argument("nf: field point lies on a loop vertex");
    const Vec3 c{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x};
    const double nc = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
    const double dot = a.x * b.x + a.y * b.y + a.z * b.z;
    const double den = na * nb * (na * nb + dot);
    // collinear with the segment: it contributes nothing, rather than a
    // division that would silently produce a spike
    if (!(den > 0) || !(nc > 0)) return 0.0;
    return current_a * nc * (na + nb) / (4.0 * PI_N * den);
}

// H VECTOR at P from a closed polygon loop. Components are summed as vectors,
// which is what makes the far-field cancellation into a 1/r^3 dipole come out
// right; summing magnitudes would leave a spurious 1/r tail. The vector matters
// in its own right: the victim's pickup goes as cos(theta) between this field
// and its own loop normal, and that angle is real layout information.
inline Vec3 h_loop_vec(const std::vector<Vec3>& poly, const Vec3& P,
                       double current_a) {
    if (poly.size() < 3)
        throw std::invalid_argument("nf: a loop needs at least three vertices");
    if (current_a < 0) throw std::invalid_argument("nf: current must be >= 0");
    Vec3 h;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Vec3& A = poly[i];
        const Vec3& B = poly[(i + 1) % poly.size()];
        const Vec3 a{A.x - P.x, A.y - P.y, A.z - P.z};
        const Vec3 b{B.x - P.x, B.y - P.y, B.z - P.z};
        const double na = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
        const double nb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
        if (!(na > 0) || !(nb > 0))
            throw std::invalid_argument("nf: field point lies on a loop vertex");
        const Vec3 c{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                     a.x * b.y - a.y * b.x};
        const double dot = a.x * b.x + a.y * b.y + a.z * b.z;
        const double den = na * nb * (na * nb + dot);
        if (!(den > 0)) continue;
        const double k = current_a * (na + nb) / (4.0 * PI_N * den);
        h.x += k * c.x; h.y += k * c.y; h.z += k * c.z;
    }
    return h;
}

inline double h_loop(const std::vector<Vec3>& poly, const Vec3& P,
                     double current_a) {
    const Vec3 h = h_loop_vec(poly, P, current_a);
    return std::sqrt(h.x * h.x + h.y * h.y + h.z * h.z);
}

// Field of a long straight conductor. This — not 1/r^3 — is the right law
// close to a TRACE, where the dipole approximation does not apply and would
// badly under-report.
inline double h_wire(double current_a, double r_m) {
    if (!(r_m > 0)) throw std::invalid_argument("nf: distance must be > 0");
    if (current_a < 0) throw std::invalid_argument("nf: current must be >= 0");
    return current_a / (2.0 * PI_N * r_m);
}

inline double b_from_h(double h_a_per_m) { return MU0 * h_a_per_m; }

inline double to_dbua_m(double h_a_per_m) {
    if (!(h_a_per_m > 0)) return -400.0;
    return 20.0 * std::log10(h_a_per_m * 1e6);
}

// ---------------------------------------------------------------------------
// Electric dipole (voltage-driven node)
// ---------------------------------------------------------------------------

// Displacement current out of a dV/dt node — the switch-node mechanism.
inline double displacement_current(double c_stray_f, double dv_dt_v_per_s) {
    if (c_stray_f < 0 || dv_dt_v_per_s < 0)
        throw std::invalid_argument("nf: capacitance and slew rate must be >= 0");
    return c_stray_f * dv_dt_v_per_s;
}

// Broadside overlap capacitance: the dominant PCB coupling mechanism, and the
// one a layout tool can compute exactly. 0.19 pF per mm^2 at 0.2 mm on FR-4.
inline double overlap_capacitance(double area_m2, double sep_m, double eps_r) {
    if (!(area_m2 > 0) || !(sep_m > 0) || !(eps_r >= 1))
        throw std::invalid_argument("nf: overlap needs positive area, gap, eps_r >= 1");
    return 8.8541878128e-12 * eps_r * area_m2 / sep_m;
}

// Quasi-static E of an electric dipole, dual to the magnetic case: the 1/r^3
// term dominates, and the same sqrt(1 + 3 cos^2) angular factor applies.
inline double e_field_dipole(double dipole_moment_cm, double r_m, double theta_rad) {
    if (!(r_m > 0)) throw std::invalid_argument("nf: distance must be > 0");
    if (dipole_moment_cm < 0) throw std::invalid_argument("nf: moment must be >= 0");
    const double c = std::cos(theta_rad);
    return dipole_moment_cm * std::sqrt(1.0 + 3.0 * c * c) /
           (4.0 * PI_N * 8.8541878128e-12 * r_m * r_m * r_m);
}

// ---------------------------------------------------------------------------
// Victim coupling — the reciprocal half
// ---------------------------------------------------------------------------
//
//   V_N = 2 pi f * B * A * cos(theta)
//
// A layout-only tool owns A and cos(theta) EXACTLY; only B carries the current
// assumption. cos(theta) is the cheapest countermeasure in the whole subject
// and is invisible to every distance-based rule: rotating a victim loop edge-on
// nulls the coupling, and perpendicular inductor axes null it entirely.
inline double induced_voltage(double f_hz, double b_tesla, double area_m2,
                              double cos_theta) {
    if (!(f_hz > 0)) throw std::invalid_argument("nf: frequency must be > 0");
    if (area_m2 < 0 || b_tesla < 0)
        throw std::invalid_argument("nf: area and flux density must be >= 0");
    return 2.0 * PI_N * f_hz * b_tesla * area_m2 * std::abs(cos_theta);
}

// Capacitive coupling onto a high-impedance victim — the frequency-independent
// ceiling, and always the worst case.
inline double capacitive_step(double dv_switch, double c_couple, double c_victim) {
    if (c_couple < 0 || c_victim < 0)
        throw std::invalid_argument("nf: capacitances must be >= 0");
    const double tot = c_couple + c_victim;
    if (!(tot > 0)) throw std::invalid_argument("nf: victim node has no capacitance");
    return dv_switch * c_couple / tot;
}

// ---------------------------------------------------------------------------
// Victim classes and their thresholds
// ---------------------------------------------------------------------------
//
// Two ladders that CANNOT be ranked against one another: a peak-volts ladder
// (logic margin, comparator hysteresis, feedback ramp) responds to the spike,
// while a DC-accuracy ladder (offset, LSB) is reached through RF rectification
// and responds to the rectified average. Mixing them produces nonsense.
enum class VictimKind { PeakVolts, DcAccuracy };

struct VictimClass {
    const char* id;
    const char* label;
    double threshold_v;
    VictimKind kind;
    const char* why;
};

inline const std::vector<VictimClass>& victim_classes() {
    static const std::vector<VictimClass> v = {
        {"csa", "Precision current-sense amp", 15e-6, VictimKind::DcAccuracy,
         "input offset spec; a shunt drop is millivolts, so tens of microvolts matter"},
        {"adc16", "16-bit ADC input (3.3 V)", 50e-6, VictimKind::DcAccuracy,
         "LSB is 50 uV; below ~18 bits the raw LSB is the honest floor"},
        {"adc12", "12-bit ADC input (3.3 V)", 806e-6, VictimKind::DcAccuracy,
         "LSB = FSR / 2^12"},
        {"adc10", "10-bit ADC input (3.3 V)", 3.22e-3, VictimKind::DcAccuracy,
         "LSB = FSR / 2^10"},
        {"comp", "Comparator / feedback node", 10e-3, VictimKind::PeakVolts,
         "hysteresis band; a spike past it trips the loop"},
        {"xtal", "Crystal oscillator loop", 20e-3, VictimKind::PeakVolts,
         "injection pulling and jitter on a high-Q, high-Z node"},
        {"logic33", "LVCMOS 3.3 V input", 0.8, VictimKind::PeakVolts,
         "DC input margin, the smaller of VIL and VDD-VIH"},
    };
    return v;
}

inline const VictimClass& victim_by_id(const std::string& id) {
    for (const auto& v : victim_classes())
        if (id == v.id) return v;
    throw std::invalid_argument(
        "nf: unknown victim class '" + id +
        "' — the threshold is what the verdict is measured against, so it "
        "cannot be guessed");
}

}  // namespace faraday::nf
