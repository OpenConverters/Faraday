#pragma once
// Radiated emissions from a current loop, against the regulatory limit line.
//
// This is the question the rest of the tool has been circling: not "how big is
// this loop" but "does it pass". Faraday already extracts the one input that
// is hard to get — the ENCLOSED AREA of the commutation loop, measured off the
// actual copper. Everything else here is textbook, and every other estimator
// makes you type the area in by hand.
//
// WHAT IT MODELS
//
// A trapezoidal current in an electrically small loop, radiating as a magnetic
// dipole. The far-field maximum at distance r is
//
//     E = eta0 * pi * f^2 * I * A / (r * c^2)
//
// (Paul, Introduction to EMC, ch. 8; Ott, Electromagnetic Compatibility
// Engineering, ch. 12). The numeric coefficient works out to 1.3168e-14, the
// constant those texts quote, but it is composed from eta0 and c here so it can
// be checked rather than trusted.
//
// The spectrum is the exact Fourier series of the trapezoid rather than its
// envelope, because the envelope is only an upper bound and we are reporting a
// margin.
//
// WHAT IT DOES NOT MODEL — and these matter
//
//   * COMMON-MODE radiation from attached cables, which dominates real
//     products more often than loop radiation does. It needs a cable length and
//     a common-mode current that the layout alone does not determine. Faraday
//     says so rather than guessing; a clean loop prediction is not a pass.
//   * Enclosures, shielding, gaskets, and the chassis.
//   * Resonances of the board, its traces, or anything attached to it.
//   * Quasi-peak detection. Levels here are peak; a QP reading is equal or
//     lower, so this errs toward pessimism, which is the right direction.
//
// Which is why the result carries the word ESTIMATE and a stated error bar, in
// exactly the same way the screening tier does. It ranks loops and tells you
// which one dominates and roughly by how much. It does not replace a chamber.

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace faraday::emc {

inline constexpr double ETA0 = 376.730313668;   // free-space impedance, ohm
inline constexpr double C_LIGHT = 299792458.0;  // m/s
inline constexpr double PI_E = 3.14159265358979323846;

// CISPR receiver resolution bandwidth over 30 MHz - 1 GHz.
inline constexpr double CISPR_RBW_HZ = 120e3;

// ---------------------------------------------------------------------------
// Source: a trapezoidal current
// ---------------------------------------------------------------------------

struct Trapezoid {
    double amplitude_a = 1.0;    // peak switched current, A
    double f_sw_hz = 500e3;      // switching frequency
    double duty = 0.4;           // pulse width at the 50% points / period
    double rise_s = 20e-9;       // 0-100% edge; fall assumed equal
};

// Amplitude of the nth harmonic, amperes (single-sided).
//
//   |c_n| = 2 A D |sinc(n pi D)| |sinc(n pi tr / T)|
//
// with D the 50%-level duty and tr the edge. The two sinc factors are what
// produce the familiar -20 dB/decade break at 1/(pi D T) and the -40 dB/decade
// break at 1/(pi tr): above the second one the radiated spectrum goes FLAT,
// because E rises as f^2 while the current falls as 1/f^2. That plateau is set
// by the edge rate alone, and it is where a converter usually fails.
inline double harmonic_a(const Trapezoid& t, int n) {
    if (n < 1) throw std::invalid_argument("emc: harmonic index must be >= 1");
    if (!(t.amplitude_a >= 0) || !(t.f_sw_hz > 0))
        throw std::invalid_argument("emc: amplitude must be >= 0 and f_sw > 0");
    if (!(t.duty > 0 && t.duty < 1))
        throw std::invalid_argument("emc: duty must lie strictly between 0 and 1");
    if (!(t.rise_s > 0))
        throw std::invalid_argument("emc: rise time must be > 0");
    const double T = 1.0 / t.f_sw_hz;
    if (t.rise_s >= t.duty * T)
        throw std::invalid_argument(
            "emc: the edge is longer than the pulse — the waveform is triangular, "
            "not trapezoidal, and this series does not describe it");
    auto sinc = [](double x) { return std::abs(x) < 1e-12 ? 1.0 : std::sin(x) / x; };
    return 2.0 * t.amplitude_a * t.duty *
           std::abs(sinc(n * PI_E * t.duty)) *
           std::abs(sinc(n * PI_E * t.rise_s / T));
}

// ---------------------------------------------------------------------------
// Radiation
// ---------------------------------------------------------------------------

// Far-field maximum from a small loop, free space, V/m.
inline double e_field_loop(double f_hz, double area_m2, double current_a,
                           double r_m) {
    if (!(f_hz > 0) || !(area_m2 > 0) || !(r_m > 0))
        throw std::invalid_argument("emc: frequency, area and distance must be > 0");
    if (current_a < 0) throw std::invalid_argument("emc: current must be >= 0");
    return ETA0 * PI_E * f_hz * f_hz * current_a * area_m2 /
           (r_m * C_LIGHT * C_LIGHT);
}

// An open-area or semi-anechoic measurement takes place over a ground plane and
// the antenna is height-scanned to find the maximum, so the direct ray and the
// reflected one can arrive in phase. The worst case is a factor of two, +6 dB.
// Included by default because a margin computed without it is optimistic in a
// way the standard's own method guarantees.
inline constexpr double GROUND_REFLECTION = 2.0;

inline double to_dbuv_m(double e_v_per_m) {
    if (!(e_v_per_m > 0)) return -400.0;      // a true zero is -inf; floor it
    return 20.0 * std::log10(e_v_per_m * 1e6);
}

// The small-loop derivation assumes the current is uniform around the loop,
// which stops being true once the loop is an appreciable fraction of a
// wavelength. Past that the f^2 growth is fiction — a real loop's radiation
// resistance saturates — so the model is marked rather than extrapolated.
// Perimeter is taken as that of a circle of equal area, which is the smallest
// perimeter the area can have and therefore the most generous reading.
inline double loop_perimeter_m(double area_m2) {
    if (!(area_m2 > 0)) throw std::invalid_argument("emc: loop area must be > 0");
    return 2.0 * std::sqrt(PI_E * area_m2);
}

inline double small_loop_max_hz(double area_m2) {
    return C_LIGHT / (4.0 * loop_perimeter_m(area_m2));
}

// ---------------------------------------------------------------------------
// Limit lines
// ---------------------------------------------------------------------------

struct Band {
    double f_lo_hz, f_hi_hz, dbuv_m;
};

struct LimitLine {
    std::string id, label;
    double distance_m;
    std::vector<Band> bands;
};

// Verified against the published tables: FCC 47 CFR 15.109(a) Class B gives
// 100/150/200/500 uV/m at 3 m, which is 40/43.5/46/54 dBuV/m; CISPR 32
// (EN 55032) Class B gives 40 and 47 dBuV/m at 3 m, Class A 40 and 47 at 10 m.
inline const std::vector<LimitLine>& limit_lines() {
    // FCC states its limits as field strengths in microvolts per metre, not in
    // decibels, so they are held that way and converted. The rounded decibel
    // figures in common use (43.5, 46, 54) are 0.02 dB off the regulation, and
    // a margin quoted to a tenth has no business carrying that.
    auto uv = [](double microvolts_per_m) {
        return 20.0 * std::log10(microvolts_per_m);
    };
    static const std::vector<LimitLine> L = {
        {"cispr32b", "CISPR 32 / EN 55032 Class B", 3.0,
         {{30e6, 230e6, 40.0}, {230e6, 1000e6, 47.0}}},
        {"fcc15b", "FCC Part 15 Class B", 3.0,
         {{30e6, 88e6, uv(100)}, {88e6, 216e6, uv(150)},
          {216e6, 960e6, uv(200)}, {960e6, 1000e6, uv(500)}}},
        {"cispr32a", "CISPR 32 / EN 55032 Class A", 10.0,
         {{30e6, 230e6, 40.0}, {230e6, 1000e6, 47.0}}},
    };
    return L;
}

inline const LimitLine& limit_by_id(const std::string& id) {
    for (const auto& l : limit_lines())
        if (l.id == id) return l;
    throw std::invalid_argument(
        "emc: unknown limit line '" + id +
        "' — the margin is meaningless without knowing which standard it is against");
}

// At a band edge the tighter limit applies, which is what the standards say.
inline std::optional<double> limit_at(const LimitLine& l, double f_hz) {
    std::optional<double> best;
    for (const auto& b : l.bands)
        if (f_hz >= b.f_lo_hz && f_hz <= b.f_hi_hz)
            best = best ? std::min(*best, b.dbuv_m) : b.dbuv_m;
    return best;
}

// ---------------------------------------------------------------------------
// The prediction
// ---------------------------------------------------------------------------

struct Harmonic {
    int n = 0;
    double f_hz = 0;
    double current_a = 0;
    double e_dbuv_m = 0;
    double envelope_dbuv_m = 0;  // upper bound; see the note on nulls below
    double limit_dbuv_m = 0;
    double margin_db = 0;        // positive = under the limit (this harmonic)
    double envelope_margin_db = 0;
    bool beyond_small_loop = false;
};

// Envelope of |c_n|: the exact series has deep nulls wherever n*D or n*tr/T
// lands on an integer, and |sinc(x)| <= min(1, 1/|x|) bounds it.
//
// The envelope, not the line spectrum, is what a margin should be quoted
// against. Those nulls exist only for a perfectly periodic trapezoid with
// exactly equal rise and fall; real switching jitters, the duty moves with
// load, and rise and fall differ, all of which fill the nulls in. Designing to
// a null means designing to a coincidence.
inline double envelope_a(const Trapezoid& t, int n) {
    if (n < 1) throw std::invalid_argument("emc: harmonic index must be >= 1");
    const double T = 1.0 / t.f_sw_hz;
    auto bound = [](double x) { return std::abs(x) < 1.0 ? 1.0 : 1.0 / std::abs(x); };
    return 2.0 * t.amplitude_a * t.duty *
           bound(n * PI_E * t.duty) * bound(n * PI_E * t.rise_s / T);
}

// The flat level above BOTH breaks, in closed form:
//
//   |c_n| -> 2 A T / (n^2 pi^2 tr)   and   E ~ f^2 |c_n|,  f = n/T
//   =>  E = 2 eta0 A_loop I / (pi c^2 T tr r)          -- independent of f
//
// This is the number a designer can act on: it moves only with loop area,
// switched current, switching period and edge rate.
inline double plateau_v_per_m(double area_m2, const Trapezoid& t, double r_m) {
    if (!(area_m2 > 0) || !(r_m > 0) || !(t.rise_s > 0) || !(t.f_sw_hz > 0))
        throw std::invalid_argument("emc: plateau needs positive area, distance, edge and f_sw");
    const double T = 1.0 / t.f_sw_hz;
    return 2.0 * ETA0 * area_m2 * t.amplitude_a /
           (PI_E * C_LIGHT * C_LIGHT * T * t.rise_s * r_m);
}

struct Prediction {
    std::vector<Harmonic> harmonics;
    // Headline margin, taken against the ENVELOPE — see envelope_a() for why
    // the line spectrum's nulls must not be credited.
    double worst_margin_db = 0;
    double worst_f_hz = 0;
    double worst_level_dbuv_m = 0;
    // The same figure read off the discrete lines, which is what a spectrum
    // analyser would show for an ideal source. Never worse than the envelope.
    double worst_harmonic_margin_db = 0;
    double plateau_dbuv_m = 0;    // the flat level the edge rate sets
    double knee_hz = 0;           // 1/(pi * rise), where the plateau begins
    double small_loop_max_hz = 0;
    int beyond_model_count = 0;
    bool harmonics_unresolved = false;  // f_sw below the receiver bandwidth
    std::string limit_id, limit_label;
    double distance_m = 0, area_mm2 = 0;
};

struct PredictOptions {
    std::string limit_id = "cispr32b";
    double f_max_hz = 1000e6;
    bool ground_reflection = true;
    int max_harmonics = 4000;
};

inline Prediction predict_loop(double area_mm2, const Trapezoid& t,
                               const PredictOptions& o) {
    if (!(area_mm2 > 0))
        throw std::invalid_argument("emc: loop area must be > 0 mm^2");
    const LimitLine& lim = limit_by_id(o.limit_id);
    const double area_m2 = area_mm2 * 1e-6;
    const double gain = o.ground_reflection ? GROUND_REFLECTION : 1.0;

    Prediction p;
    p.limit_id = lim.id;
    p.limit_label = lim.label;
    p.distance_m = lim.distance_m;
    p.area_mm2 = area_mm2;
    p.small_loop_max_hz = small_loop_max_hz(area_m2);
    p.knee_hz = 1.0 / (PI_E * t.rise_s);
    // Below the receiver's resolution bandwidth the harmonics are not resolved
    // and several land in one measurement bin, so the reading is higher than any
    // single line drawn here. Common on converters that switch at 50-100 kHz.
    p.harmonics_unresolved = t.f_sw_hz < CISPR_RBW_HZ;

    // Only harmonics inside the regulated band are worth reporting; below
    // 30 MHz radiated limits do not apply (conducted ones do, separately).
    const double f_lo = lim.bands.front().f_lo_hz;
    const int n_start = std::max(1, (int)std::floor(f_lo / t.f_sw_hz));
    const int n_end = std::min((double)o.max_harmonics,
                               std::floor(o.f_max_hz / t.f_sw_hz));

    bool have = false;
    for (int n = n_start; n <= (int)n_end; ++n) {
        const double f = n * t.f_sw_hz;
        if (f < f_lo || f > o.f_max_hz) continue;
        auto lv = limit_at(lim, f);
        if (!lv) continue;
        Harmonic h;
        h.n = n;
        h.f_hz = f;
        h.current_a = harmonic_a(t, n);
        h.e_dbuv_m = to_dbuv_m(gain * e_field_loop(f, area_m2, h.current_a,
                                                   lim.distance_m));
        h.envelope_dbuv_m = to_dbuv_m(
            gain * e_field_loop(f, area_m2, envelope_a(t, n), lim.distance_m));
        h.limit_dbuv_m = *lv;
        h.margin_db = h.limit_dbuv_m - h.e_dbuv_m;
        h.envelope_margin_db = h.limit_dbuv_m - h.envelope_dbuv_m;
        h.beyond_small_loop = f > p.small_loop_max_hz;
        if (h.beyond_small_loop) ++p.beyond_model_count;
        else {
            if (!have || h.envelope_margin_db < p.worst_margin_db) {
                p.worst_margin_db = h.envelope_margin_db;
                p.worst_f_hz = f;
                p.worst_level_dbuv_m = h.envelope_dbuv_m;
            }
            if (!have || h.margin_db < p.worst_harmonic_margin_db)
                p.worst_harmonic_margin_db = h.margin_db;
            have = true;
        }
        p.harmonics.push_back(h);
    }
    if (!have)
        throw std::invalid_argument(
            "emc: no harmonic of this source falls inside the regulated band "
            "where the small-loop model is still valid — check f_sw and the "
            "loop area");
    // Closed form, not a sample: reading the plateau off "the first harmonic
    // above the knee" picks whatever line happens to sit there, and with a
    // duty of 2/5 every fifth line is a null — which read -249 dBuV/m and made
    // the edge-rate scaling come out at +295 dB instead of +6.
    p.plateau_dbuv_m = to_dbuv_m(gain * plateau_v_per_m(area_m2, t, lim.distance_m));
    return p;
}

}  // namespace faraday::emc
