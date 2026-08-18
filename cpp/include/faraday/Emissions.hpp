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
// Common-mode radiation from an attached cable
// ---------------------------------------------------------------------------
//
// The mechanism that fails most real products, and the one the loop model above
// explicitly does not cover. A cable carrying common-mode current is a monopole
// over the ground plane, and a short one radiates
//
//     E = eta0 * f * I_cm * L_eff / (2 * c * r)          (free space)
//
// With the ground-plane image included that doubles to eta0/(c) = 1.2566e-6,
// which is the constant Ott quotes. It is composed here rather than pasted, and
// the doubling goes through the SAME reflection factor the loop model uses, so
// the toggle means one thing in both places.
//
// FARADAY CANNOT KNOW I_cm. It depends on the common-mode voltage driving the
// cable, which comes from ground-plane impedance, return-path detours and
// connector placement — not from geometry alone. So the useful direction is
// the inverse: given the limit, how much common-mode current can this cable
// carry? That budget needs no unknowns, and it is the number EMC engineers
// actually quote — a few microamps, which is why this mechanism is so easily
// missed with a current probe that reads in milliamps.

// A monopole stops getting more effective once it passes a quarter wave; past
// that it resonates and the short-antenna formula, which would keep growing
// with length, is simply wrong. Capping at lambda/4 is the standard treatment.
inline double effective_length_m(double len_m, double f_hz) {
    if (!(len_m > 0) || !(f_hz > 0))
        throw std::invalid_argument("emc: cable length and frequency must be > 0");
    return std::min(len_m, C_LIGHT / (4.0 * f_hz));
}

inline double cm_e_field(double f_hz, double i_cm_a, double len_m, double r_m) {
    if (!(r_m > 0)) throw std::invalid_argument("emc: distance must be > 0");
    if (i_cm_a < 0) throw std::invalid_argument("emc: current must be >= 0");
    return ETA0 * f_hz * i_cm_a * effective_length_m(len_m, f_hz) /
           (2.0 * C_LIGHT * r_m);
}

// The inverse: the largest common-mode current that still meets `limit_dbuv_m`.
inline double cm_current_budget_a(double limit_dbuv_m, double f_hz, double len_m,
                                  double r_m, double gain) {
    if (!(gain > 0)) throw std::invalid_argument("emc: reflection gain must be > 0");
    const double e_limit = std::pow(10.0, limit_dbuv_m / 20.0) * 1e-6;   // V/m
    const double per_amp = gain * cm_e_field(f_hz, 1.0, len_m, r_m);
    if (!(per_amp > 0)) throw std::invalid_argument("emc: degenerate cable geometry");
    return e_limit / per_amp;
}

struct CmPoint {
    double f_hz = 0, budget_a = 0, limit_dbuv_m = 0, eff_len_m = 0;
    bool resonant = false;      // cable longer than a quarter wave here
};

struct CmBudget {
    std::vector<CmPoint> points;
    double tightest_a = 0, tightest_f_hz = 0;
    double cable_m = 0, distance_m = 0;
    double quarter_wave_hz = 0;   // where the cable first reaches lambda/4
    std::string limit_id, limit_label;
};

inline CmBudget cm_budget(double cable_m, const std::string& limit_id,
                          bool ground_reflection = true, int points = 160) {
    if (!(cable_m > 0)) throw std::invalid_argument("emc: cable length must be > 0");
    if (points < 8) throw std::invalid_argument("emc: need at least 8 points");
    const LimitLine& lim = limit_by_id(limit_id);
    const double gain = ground_reflection ? GROUND_REFLECTION : 1.0;

    CmBudget b;
    b.cable_m = cable_m;
    b.distance_m = lim.distance_m;
    b.limit_id = lim.id;
    b.limit_label = lim.label;
    b.quarter_wave_hz = C_LIGHT / (4.0 * cable_m);

    const double f_lo = lim.bands.front().f_lo_hz;
    const double f_hi = lim.bands.back().f_hi_hz;
    bool have = false;
    for (int i = 0; i < points; ++i) {
        // log spacing: the band spans a decade and a half
        const double f = f_lo * std::pow(f_hi / f_lo, (double)i / (points - 1));
        auto lv = limit_at(lim, f);
        if (!lv) continue;
        CmPoint p;
        p.f_hz = f;
        p.limit_dbuv_m = *lv;
        p.eff_len_m = effective_length_m(cable_m, f);
        p.resonant = p.eff_len_m < cable_m;
        p.budget_a = cm_current_budget_a(*lv, f, cable_m, lim.distance_m, gain);
        if (!have || p.budget_a < b.tightest_a) {
            b.tightest_a = p.budget_a;
            b.tightest_f_hz = f;
            have = true;
        }
        b.points.push_back(p);
    }
    if (!have) throw std::invalid_argument("emc: no regulated frequency in this limit line");
    return b;
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

// ---------------------------------------------------------------------------
// Conducted estimate (pre-hardware) — the bridge to a filter design
// ---------------------------------------------------------------------------
// The SAME trapezoid, driven into the two conducted paths a LISN measures:
//
//   DM: the switch cell draws |I_n| from the input capacitor; what the LISN
//       sees is the residual across the capacitor branch,
//       V ~ |I_n| · |Z_cin|,  Z_cin = ESR + jwL_branch + 1/(jwC)
//       (valid while |Z_cin| << the 100-ohm LISN path — true for any real
//       input capacitor in the band).
//   CM: the switch-node VOLTAGE trapezoid pumps the stray capacitance to
//       chassis/earth, I_cm = V_n · wC_stray (|Z_stray| >> 25 ohm), and the
//       LISN's 25-ohm common path converts it, V = I_cm · 25.
//
// These are SEEDING estimates for a filter design, not measurements: the DM
// band is ~+/-10 dB (branch parasitics assumed), the CM band ~+/-15 dB
// (C_stray is an assumption by construction). Both bands ride on top of the
// +10 dB design margin the filter chain adds. Every value is dBuV.

namespace faraday::emc {

struct ConductedEstimate {
    std::vector<double> f_hz;
    std::vector<double> dm_dbuv;
    std::vector<double> cm_dbuv;
};

inline ConductedEstimate conducted_estimate(const Trapezoid& t, double c_in_f,
                                            double esl_h, double esr_ohm,
                                            double v_bus_v, double c_stray_f,
                                            double f1_hz = 150e3,
                                            double f2_hz = 30e6) {
    if (!(c_in_f > 0) || !(esl_h >= 0) || !(esr_ohm >= 0))
        throw std::invalid_argument("conducted: input-cap branch must be physical");
    if (!(v_bus_v > 0) || !(c_stray_f > 0))
        throw std::invalid_argument("conducted: bus voltage and C_stray must be > 0");
    if (!(f1_hz > 0) || !(f2_hz > f1_hz))
        throw std::invalid_argument("conducted: bad band");
    Trapezoid v = t;            // the voltage trapezoid: same timing, V_bus high
    v.amplitude_a = v_bus_v;
    ConductedEstimate out;
    // dense comb up to harmonic 40, then max-hold per 1/12 decade — a URL-
    // sized spectrum that still contains every envelope feature
    const int n_max = (int)(f2_hz / t.f_sw_hz);
    double bucket_top = 0.0;
    double bucket_dm = -1e30, bucket_cm = -1e30, bucket_f = 0.0;
    auto flush = [&]() {
        if (bucket_f > 0.0) {
            out.f_hz.push_back(bucket_f);
            out.dm_dbuv.push_back(bucket_dm);
            out.cm_dbuv.push_back(bucket_cm);
        }
        bucket_dm = bucket_cm = -1e30;
        bucket_f = 0.0;
    };
    for (int n = 1; n <= n_max; ++n) {
        const double f = n * t.f_sw_hz;
        if (f < f1_hz || f > f2_hz) continue;
        const double w = 2.0 * PI_E * f;
        const double z_cin = std::hypot(esr_ohm, w * esl_h - 1.0 / (w * c_in_f));
        const double v_dm = harmonic_a(t, n) * z_cin;
        const double v_cm = harmonic_a(v, n) * w * c_stray_f * 25.0;
        const double dm = 20.0 * std::log10(std::max(v_dm, 1e-12) / 1e-6);
        const double cm = 20.0 * std::log10(std::max(v_cm, 1e-12) / 1e-6);
        if (n <= 40) {
            out.f_hz.push_back(f);
            out.dm_dbuv.push_back(dm);
            out.cm_dbuv.push_back(cm);
            continue;
        }
        if (f > bucket_top) {   // new 1/12-decade bucket
            flush();
            bucket_top = f * std::pow(10.0, 1.0 / 12.0);
        }
        if (dm > bucket_dm) { bucket_dm = dm; bucket_f = f; }
        if (cm > bucket_cm) bucket_cm = cm;
    }
    flush();
    if (out.f_hz.empty())
        throw std::invalid_argument(
            "conducted: no switching harmonic falls inside the band — at this "
            "f_sw the conducted story starts above 30 MHz");
    return out;
}

// ---------------------------------------------------------------------------
// Conducted limit lines — and the verdict the estimate is worth
// ---------------------------------------------------------------------------
// A spectrum without a limit line cannot answer the three questions a filter
// designer actually asks: does it fail, AT WHAT FREQUENCY, and WHICH MODE is
// driving it. The estimate above produced the two mode spectra; this turns
// them into that answer, on the same board, before the hop to Hertz.
//
// The values are CISPR 32 / EN 55032 AC-mains conducted and they are the SAME
// numbers Hertz carries (hertz.limits.CISPR32_CLASS_*_MAINS_*) — Hertz stays
// the authority for conducted work (LISN models, the CISPR 16-1-1 detector,
// filter synthesis); this table exists so Faraday's own panel can say pass or
// fail before the handoff, and a test pins it against Hertz's numbers.
//
// Levels interpolate linearly in log10(f) inside a segment, which is the shape
// CISPR limits are defined with (the 150-500 kHz mains segment slopes 10 dB).
// Where two segments meet the LOWER limit applies — the standard's own
// transition rule, so Class A at exactly 500 kHz is 73 dBuV and not 79.
//
// DETECTOR. The comb here is a peak envelope of an ideal trapezoid. A
// quasi-peak reading is equal or lower and an average reading lower still, so
// judging a peak estimate against the QP line errs toward pessimism — the
// right direction for a screening number, and the same convention the radiated
// side uses.

struct ConductedSegment {
    double f_lo_hz, f_hi_hz, lo_dbuv, hi_dbuv;
};

struct ConductedLimit {
    std::string id, label, detector;   // detector: "quasi-peak" | "average"
    std::vector<ConductedSegment> segs;
};

inline const std::vector<ConductedLimit>& conducted_limits() {
    static const std::vector<ConductedLimit> L = {
        {"cispr32b-qp", "CISPR 32 / EN 55032 Class B mains (QP)", "quasi-peak",
         {{150e3, 500e3, 66.0, 56.0}, {500e3, 5e6, 56.0, 56.0}, {5e6, 30e6, 60.0, 60.0}}},
        {"cispr32b-av", "CISPR 32 / EN 55032 Class B mains (AV)", "average",
         {{150e3, 500e3, 56.0, 46.0}, {500e3, 5e6, 46.0, 46.0}, {5e6, 30e6, 50.0, 50.0}}},
        {"cispr32a-qp", "CISPR 32 / EN 55032 Class A mains (QP)", "quasi-peak",
         {{150e3, 500e3, 79.0, 79.0}, {500e3, 30e6, 73.0, 73.0}}},
        {"cispr32a-av", "CISPR 32 / EN 55032 Class A mains (AV)", "average",
         {{150e3, 500e3, 66.0, 66.0}, {500e3, 30e6, 60.0, 60.0}}},
    };
    return L;
}

inline const ConductedLimit& conducted_limit_by_id(const std::string& id) {
    for (const auto& l : conducted_limits())
        if (l.id == id) return l;
    throw std::invalid_argument(
        "conducted: unknown limit line '" + id +
        "' — a margin means nothing without the standard it is against");
}

inline std::optional<double> conducted_limit_at(const ConductedLimit& l, double f_hz) {
    if (!(f_hz > 0)) throw std::invalid_argument("conducted: frequency must be > 0");
    std::optional<double> best;
    for (const auto& s : l.segs) {
        if (f_hz < s.f_lo_hz || f_hz > s.f_hi_hz) continue;
        const double span = std::log10(s.f_hi_hz) - std::log10(s.f_lo_hz);
        const double frac = (std::log10(f_hz) - std::log10(s.f_lo_hz)) / span;
        const double v = s.lo_dbuv + frac * (s.hi_dbuv - s.lo_dbuv);
        best = best ? std::min(*best, v) : v;
    }
    return best;
}

// ANP015's design frequency: f_sw itself, or its first harmonic that lands in
// the measured band. Below 150 kHz nothing is measured, so designing at f_sw
// would size the filter for a frequency no receiver looks at.
inline double conducted_design_frequency(double f_sw_hz) {
    if (!(f_sw_hz > 0))
        throw std::invalid_argument("conducted: switching frequency must be > 0");
    if (f_sw_hz >= 150e3) return f_sw_hz;
    return std::ceil(150e3 / f_sw_hz) * f_sw_hz;
}

struct ConductedPoint {
    double f_hz = 0;
    double dm_dbuv = 0, cm_dbuv = 0;
    double limit_dbuv = 0;
    double dm_margin_db = 0, cm_margin_db = 0;   // positive = under the limit
};

// WHICH MODE. Each mode is judged against the FULL limit line, which is what
// ANP015 does when it sizes a CM stage and a DM stage separately. The limit
// strictly applies to the line voltage a LISN measures, and that carries both
// modes at once — but they are not phase-coherent, and when they are equal the
// sum is only 6 dB above either, which the +10 dB design margin covers. What
// this must never do is invent a phase relationship to produce one prettier
// number: the mode split IS the actionable output, because a CM problem and a
// DM problem are fixed by different components.
struct ConductedVerdict {
    std::vector<ConductedPoint> points;
    std::string limit_id, limit_label, detector;
    // headline: the worse of the two modes, and where
    double worst_margin_db = 0, worst_f_hz = 0, worst_level_dbuv = 0;
    std::string worst_mode;                       // "CM" | "DM"
    double dm_worst_margin_db = 0, dm_worst_f_hz = 0;
    double cm_worst_margin_db = 0, cm_worst_f_hz = 0;
    double cm_dominant_fraction = 0;              // share of in-band points where CM >= DM
    // Lowest frequency above which CM stays at or above DM — "above 1.4 MHz
    // this is a common-mode problem". 0 when DM is still on top at the top of
    // the band (there is then no such frequency).
    double cm_crossover_hz = 0;
    double design_f_hz = 0, design_margin_db = 0;
    // ANP015 A_req = level - limit + margin, at the design frequency. Negative
    // means that mode already meets the limit and needs no stage.
    double required_dm_db = 0, required_cm_db = 0;
    // The same requirement taken over the WHOLE band rather than at one
    // frequency — a filter sized only at f_design can still be short higher up.
    double required_dm_band_db = 0, required_cm_band_db = 0;
};

inline ConductedVerdict conducted_verdict(const ConductedEstimate& est,
                                          double f_sw_hz,
                                          const std::string& limit_id = "cispr32b-qp",
                                          double design_margin_db = 10.0) {
    if (est.f_hz.size() != est.dm_dbuv.size() ||
        est.f_hz.size() != est.cm_dbuv.size())
        throw std::invalid_argument("conducted: malformed estimate");
    if (est.f_hz.empty())
        throw std::invalid_argument("conducted: empty estimate");
    if (!(design_margin_db >= 0))
        throw std::invalid_argument("conducted: design margin must be >= 0");
    const ConductedLimit& lim = conducted_limit_by_id(limit_id);

    ConductedVerdict v;
    v.limit_id = lim.id;
    v.limit_label = lim.label;
    v.detector = lim.detector;
    v.design_f_hz = conducted_design_frequency(f_sw_hz);
    v.design_margin_db = design_margin_db;

    bool have = false;
    size_t cm_over = 0;
    for (size_t i = 0; i < est.f_hz.size(); ++i) {
        auto lv = conducted_limit_at(lim, est.f_hz[i]);
        if (!lv) continue;                    // outside the line's coverage
        ConductedPoint p;
        p.f_hz = est.f_hz[i];
        p.dm_dbuv = est.dm_dbuv[i];
        p.cm_dbuv = est.cm_dbuv[i];
        p.limit_dbuv = *lv;
        p.dm_margin_db = *lv - p.dm_dbuv;
        p.cm_margin_db = *lv - p.cm_dbuv;
        if (p.cm_dbuv >= p.dm_dbuv) ++cm_over;
        if (!have || p.dm_margin_db < v.dm_worst_margin_db) {
            v.dm_worst_margin_db = p.dm_margin_db;
            v.dm_worst_f_hz = p.f_hz;
        }
        if (!have || p.cm_margin_db < v.cm_worst_margin_db) {
            v.cm_worst_margin_db = p.cm_margin_db;
            v.cm_worst_f_hz = p.f_hz;
        }
        have = true;
        v.points.push_back(p);
    }
    if (!have)
        throw std::invalid_argument(
            "conducted: no estimated point falls inside this limit line's "
            "frequency coverage");

    v.cm_dominant_fraction = (double)cm_over / (double)v.points.size();
    // scan down from the top: the crossover is the first frequency from which
    // CM never falls below DM again
    v.cm_crossover_hz = 0;
    for (size_t i = v.points.size(); i-- > 0;) {
        if (v.points[i].cm_dbuv < v.points[i].dm_dbuv) break;
        v.cm_crossover_hz = v.points[i].f_hz;
    }

    if (v.cm_worst_margin_db <= v.dm_worst_margin_db) {
        v.worst_mode = "CM";
        v.worst_margin_db = v.cm_worst_margin_db;
        v.worst_f_hz = v.cm_worst_f_hz;
    } else {
        v.worst_mode = "DM";
        v.worst_margin_db = v.dm_worst_margin_db;
        v.worst_f_hz = v.dm_worst_f_hz;
    }
    for (const auto& p : v.points)
        if (p.f_hz == v.worst_f_hz)
            v.worst_level_dbuv = v.worst_mode == "CM" ? p.cm_dbuv : p.dm_dbuv;

    // A_req at the design frequency: the comb's point nearest it in log f
    const ConductedPoint* at = &v.points.front();
    double best = 1e30;
    for (const auto& p : v.points) {
        const double d = std::abs(std::log10(p.f_hz / v.design_f_hz));
        if (d < best) { best = d; at = &p; }
    }
    v.required_dm_db = at->dm_dbuv - at->limit_dbuv + design_margin_db;
    v.required_cm_db = at->cm_dbuv - at->limit_dbuv + design_margin_db;
    v.required_dm_band_db = design_margin_db - v.dm_worst_margin_db;
    v.required_cm_band_db = design_margin_db - v.cm_worst_margin_db;
    return v;
}

// ---------------------------------------------------------------------------
// The common-mode source term, off the copper
// ---------------------------------------------------------------------------
// C_stray is what turns dV/dt into common-mode current, and it used to be a
// slider with an invented default — the one number in the conducted estimate
// that was pure assumption. Most of it is not assumption: the PLATE is the
// dv/dt copper, and Faraday measures that off the layout exactly the way it
// measures the commutation loop. What the layout cannot carry is how far the
// metalwork is, so that is the one thing left to state:
//
//     C = eps0 * eps_r * A / d
//
// Fringing is ignored, and fringing only ADDS, so this is a floor rather than
// a guess in the middle. Two mounting cases matter: a chassis or heatsink at
// an air gap (eps_r = 1), and one bolted against the board's own dielectric
// (eps_r of the laminate, a much larger capacitance). Neither is the whole
// story — a heatsink on the FET tab, the transformer's inter-winding
// capacitance and the cable harness all add paths this cannot see — so the
// derived value stays a stated lower bound on the geometry's contribution.
inline constexpr double EPS0 = 8.8541878128e-12;   // F/m

inline double chassis_stray_c_f(double area_mm2, double gap_mm, double eps_r = 1.0) {
    if (!(area_mm2 > 0))
        throw std::invalid_argument("conducted: dv/dt copper area must be > 0 mm^2");
    if (!(gap_mm > 0))
        throw std::invalid_argument(
            "conducted: the gap to the chassis must be > 0 — a plate at zero "
            "distance is a short, not a capacitance");
    if (!(eps_r >= 1.0))
        throw std::invalid_argument("conducted: epsilon_r must be >= 1");
    return EPS0 * eps_r * (area_mm2 * 1e-6) / (gap_mm * 1e-3);
}

}  // namespace faraday::emc
