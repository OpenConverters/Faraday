#pragma once
// The bench: turns a screening finding into a real answer.
//
// One call runs the whole chain — 2D boundary-element extraction of the actual
// cross-section, RLGC, a transient of the coupled pair, and a verdict against
// the receiver's noise budget — and returns it as JSON the browser can draw.
// The whole thing is a couple of milliseconds, which is what lets the UI
// re-run it while a slider is moving instead of behind a "Simulate" button.
//
// The answer the user is owed is not a dB figure. It is "your victim sees
// 340 mV, your receiver tolerates 800 mV, you are using 43% of the budget",
// and — when that is too much — the separation that would fix it.

#include "Bem2d.hpp"
#include "Emissions.hpp"
#include "NearFieldMap.hpp"
#include "Shielding.hpp"
#include "Pdn.hpp"
#include "ReturnPath.hpp"
#include "Mna.hpp"
#include "Rlgc.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <cmath>
#include <string>
#include <vector>

namespace faraday::bench {

// ---------------------------------------------------------------------------
// Receiver noise budget
// ---------------------------------------------------------------------------
//
// The usable budget is the smaller of the two DC input margins: how far a low
// may rise before it stops reading as a low (VIL), and how far a high may fall
// (VDD - VIH). Thresholds are the JEDEC/vendor datasheet numbers for each
// family. Crosstalk is only one contributor, so consuming the entire margin is
// already a failure — the bands below put the alarm at half.
struct LogicFamily {
    const char* id;
    const char* label;
    double vdd, vil_max, vih_min;
    double budget() const { return std::min(vil_max, vdd - vih_min); }
};

inline const std::vector<LogicFamily>& logic_families() {
    static const std::vector<LogicFamily> f = {
        {"lvcmos33", "LVCMOS 3.3 V", 3.3, 0.8, 2.0},
        {"lvcmos25", "LVCMOS 2.5 V", 2.5, 0.7, 1.7},
        {"lvcmos18", "LVCMOS 1.8 V", 1.8, 0.63, 1.17},
        {"lvcmos12", "LVCMOS 1.2 V", 1.2, 0.42, 0.78},
        // Differential receivers judge against a threshold either side of the
        // switching point rather than against the rails.
        {"lvds", "LVDS (100 mV)", 1.2, 0.1, 1.1},
        {"sstl15", "DDR3 / SSTL-1.5", 1.5, 0.65, 0.85},
    };
    return f;
}

inline const LogicFamily& family_by_id(const std::string& id) {
    for (const auto& f : logic_families())
        if (id == f.id) return f;
    throw std::invalid_argument(
        "bench: unknown logic family '" + id +
        "' — the receiver's threshold is what the verdict is measured against, "
        "so it cannot be guessed");
}

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------

struct Request {
    bem::PairSection section;
    mna::DriveOptions drive;
    std::string family = "lvcmos33";
    bool want_field = true;
    int field_nx = 168, field_ny = 84;
    bool want_fix = true;
};

inline Request request_from_json(const nlohmann::json& j) {
    Request r;
    auto need = [&](const char* k) -> double {
        if (!j.contains(k) || !j.at(k).is_number())
            throw std::invalid_argument(
                std::string("bench: missing required number '") + k + "'");
        return j.at(k).get<double>();
    };
    auto opt = [&](const char* k, double d) {
        return (j.contains(k) && j.at(k).is_number()) ? j.at(k).get<double>() : d;
    };
    const std::string mode = j.value("mode", "microstrip");
    r.section.broadside = (mode == "broadside");
    r.section.stripline = (mode == "stripline");
    r.section.triple = (mode == "triple");
    r.section.w1 = need("w1Mm") * 1e-3;
    r.section.w2 = need("w2Mm") * 1e-3;
    r.section.t = opt("tMm", 0.035) * 1e-3;
    r.section.eps_r = need("epsR");
    r.section.gap = opt("gapMm", 0.2) * 1e-3;
    r.section.h = opt("hMm", 0.2) * 1e-3;
    r.section.h_v = opt("hvMm", 0.2) * 1e-3;
    r.section.lateral = opt("lateralMm", 0.0) * 1e-3;
    r.section.b = opt("bMm", 0.6) * 1e-3;
    r.section.w3 = opt("w3Mm", opt("w1Mm", 0.2)) * 1e-3;
    r.section.gap2 = opt("gap2Mm", opt("gapMm", 0.2)) * 1e-3;

    r.drive.length_m = need("lengthMm") * 1e-3;
    r.drive.rise_s = opt("riseNs", 1.0) * 1e-9;
    r.drive.amplitude_v = opt("amplitudeV", 3.3);
    r.drive.z_src = opt("zSrcOhm", 30.0);
    r.drive.z_term = opt("zTermOhm", 10000.0);
    r.drive.z_victim_near = opt("zVictimOhm", 30.0);
    if (r.section.triple) r.drive.aggressors = {0, 2};   // victim in the middle
    r.family = j.value("family", std::string("lvcmos33"));
    r.want_field = j.value("field", true);
    r.want_fix = j.value("fix", true);
    return r;
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

struct Extraction {
    Rlgc p;
    bem::Solution solved;
    size_t panels = 0;
};

// Skin-effect resistance per metre at frequency f for a rectangular trace
// over a plane: DC below the crossover, one current sheet of one skin depth on
// the plane-facing side above it (proximity crowds the current there). The
// larger of the two, so the model never under-reports either regime.
inline double r_ac_per_m(double w_m, double t_m, double f_hz) {
    if (!(w_m > 0) || !(t_m > 0))
        throw std::invalid_argument("bench: trace section must be positive");
    constexpr double RHO_CU = 1.724e-8;              // ohm*m
    const double r_dc = RHO_CU / (w_m * t_m);
    if (!(f_hz > 0)) return r_dc;
    const double delta = 66100.0e-6 / std::sqrt(f_hz);   // m, copper
    return std::max(r_dc, RHO_CU / (w_m * std::min(t_m, delta)));
}

inline Extraction extract(const bem::PairSection& s, double f_knee_hz = 0) {
    bem::Geometry g = bem::geometry_for(s);
    bem::Solution sd = bem::solve(g, false);
    bem::Solution sv = bem::solve(g, true);
    // n-signal aware: the solver was N-conductor all along, and the triple
    // section exercises it with three
    const size_t ns = (size_t)sd.n_signal;
    const size_t nt = ns + 1;
    std::vector<double> M(nt * nt, 0.0), M0(nt * nt, 0.0);
    for (size_t i = 0; i < ns; ++i)
        for (size_t j = 0; j < ns; ++j) {
            M[i * nt + j] = sd.at(i, j);
            M0[i * nt + j] = sv.at(i, j);
        }
    Extraction e;
    // Copper loss at the edge's knee frequency. Small against 50 ohm on short
    // runs, but it is what damps the ringing on unterminated lines, and a
    // lossless model was reporting the stated-pessimistic peak (#325).
    std::vector<double> r = {r_ac_per_m(s.w1, s.t, f_knee_hz),
                             r_ac_per_m(s.w2, s.t, f_knee_hz)};
    if (ns >= 3) r.push_back(r_ac_per_m(s.w3, s.t, f_knee_hz));
    e.p = rlgc_from_maxwell(M, M0, nt, ns, r);
    e.panels = sd.panels.size();
    e.solved = std::move(sd);
    return e;
}

// ---------------------------------------------------------------------------
// Field map transport
// ---------------------------------------------------------------------------

inline std::string base64(const std::vector<unsigned char>& in) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned a = in[i];
        const unsigned b = (i + 1 < in.size()) ? in[i + 1] : 0;
        const unsigned c = (i + 2 < in.size()) ? in[i + 2] : 0;
        const unsigned v = (a << 16) | (b << 8) | c;
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += (i + 1 < in.size()) ? T[(v >> 6) & 63] : '=';
        out += (i + 2 < in.size()) ? T[v & 63] : '=';
    }
    return out;
}

// Quantise the two scalar fields to bytes and ship them base64. A 168x84 grid
// is 14 kB per channel that way, against roughly 200 kB as JSON numbers, and
// the browser turns it straight into ImageData.
inline nlohmann::json field_json(const bem::FieldMap& f) {
    std::vector<unsigned char> v(f.v.size()), e(f.e.size());
    // potential is bounded by the excitation, so a linear 0..1 V map is exact
    // to 4 mV; |E| spans decades and is quantised on a log scale
    const double efloor = f.e_max > 0 ? f.e_max * 1e-4 : 1.0;
    for (size_t i = 0; i < f.v.size(); ++i) {
        v[i] = (unsigned char)std::clamp(f.v[i] * 255.0f, 0.0f, 255.0f);
        const double em = std::max((double)f.e[i], efloor);
        const double s = std::log(em / efloor) / std::log(f.e_max / efloor);
        e[i] = (unsigned char)std::clamp(s * 255.0, 0.0, 255.0);
    }
    return {{"nx", f.nx}, {"ny", f.ny},
            {"x0Mm", f.x0 * 1e3}, {"y0Mm", f.y0 * 1e3},
            {"x1Mm", f.x1 * 1e3}, {"y1Mm", f.y1 * 1e3},
            {"eMax", f.e_max}, {"eFloor", efloor},
            {"v", base64(v)}, {"e", base64(e)}};
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

inline double peak_noise(const Rlgc& p, const mna::DriveOptions& o) {
    mna::Waveforms w = mna::simulate(p, o);
    return std::max(std::abs(w.next_peak_v), std::abs(w.fext_peak_v));
}

// Smallest edge-to-edge gap that brings the victim under `target` volts.
// Coupling falls monotonically with separation, so a bisection is safe; each
// probe is a full extraction plus a transient, which is why it can be done at
// all. Returns 0 when even a very wide gap is not enough — at which point the
// honest advice is not "move it further" but "change something else".
inline double gap_to_meet(const bem::PairSection& base, mna::DriveOptions o,
                          double target_v, int iters = 11) {
    bem::PairSection s = base;
    const double f_knee = 0.35 / o.rise_s;
    const double lo0 = base.gap;
    double hi = std::max(base.gap, base.h) * 12.0;
    s.gap = hi;
    if (peak_noise(extract(s, f_knee).p, o) > target_v) return 0.0;
    double lo = lo0;
    for (int i = 0; i < iters; ++i) {
        const double mid = 0.5 * (lo + hi);
        s.gap = mid;
        if (peak_noise(extract(s, f_knee).p, o) > target_v) lo = mid;
        else hi = mid;
    }
    return hi;
}

inline nlohmann::json run(const Request& r) {
    using clk = std::chrono::steady_clock;
    const LogicFamily& fam = family_by_id(r.family);

    const auto t0 = clk::now();
    // 0.35 / t_r is the classic knee of a trapezoid's spectrum — the highest
    // frequency the edge meaningfully contains, and where the copper loss is
    // evaluated.
    const double f_knee = 0.35 / r.drive.rise_s;
    Extraction ex = extract(r.section, f_knee);
    const auto t1 = clk::now();
    mna::Waveforms w = mna::simulate(ex.p, r.drive);
    const auto t2 = clk::now();

    const Rlgc& p = ex.p;
    const bool pair = (p.n == 2);
    const size_t vic = pair ? 1 : 1;   // triple: victim is the middle line
    const double z0 = p.z0(0);
    // Effective permittivity is a MODAL quantity. Taking it from the diagonal
    // of L and C ignores the off-diagonal terms and reports a value above the
    // laminate's own eps_r for a buried pair, which is not a thing that can
    // happen. The even and odd modes are the ones that propagate.
    // Even/odd are PAIR quantities; a triple reports the victim line's own
    // velocity instead of pretending two modes describe three conductors.
    const double eps_even = pair ? std::pow(bem::C_LIGHT / p.v_even(), 2.0)
                                 : std::pow(bem::C_LIGHT / p.velocity(vic), 2.0);
    const double eps_odd = pair ? std::pow(bem::C_LIGHT / p.v_odd(), 2.0) : eps_even;
    const double eps_eff = 0.5 * (eps_even + eps_odd);

    const double peak = std::max(std::abs(w.next_peak_v), std::abs(w.fext_peak_v));
    const double budget = fam.budget();
    const double pct = 100.0 * peak / budget;

    nlohmann::json out;
    out["geometry"] = {
        {"mode", r.section.broadside ? "broadside"
                                     : (r.section.stripline ? "stripline" : "microstrip")},
        {"w1Mm", r.section.w1 * 1e3}, {"w2Mm", r.section.w2 * 1e3},
        {"gapMm", r.section.gap * 1e3}, {"hMm", r.section.h * 1e3},
        {"tMm", r.section.t * 1e3}, {"epsR", r.section.eps_r},
        {"hvMm", r.section.h_v * 1e3}, {"bMm", r.section.b * 1e3},
        {"panels", (int)ex.panels}};
    out["rlgc"] = {
        {"z0", z0},
        {"zEven", pair ? p.z_even() : p.z0(vic)},
        {"zOdd", pair ? p.z_odd() : p.z0(vic)},
        {"zDiff", pair ? 2.0 * p.z_odd() : 0.0},
        {"velocity", pair ? 0.5 * (p.v_even() + p.v_odd()) : p.velocity(vic)},
        {"epsEff", eps_eff},
        {"epsEffEven", eps_even},
        {"epsEffOdd", eps_odd},
        {"delayPsPerMm", pair ? 2e12 / (p.v_even() + p.v_odd()) * 1e-3
                              : 1e12 / p.velocity(vic) * 1e-3},
        {"lSelfNhPerMm", p.at(p.L, 0, 0) * 1e9 * 1e-3},
        {"cSelfPfPerMm", p.c_to_ref(0) * 1e12 * 1e-3},
        {"lMutualNhPerMm", p.at(p.L, 0, 1) * 1e9 * 1e-3},
        {"cMutualPfPerMm", p.c_mutual(0, 1) * 1e12 * 1e-3},
        {"kb", p.kb(0, 1)},
        {"rAcOhmPerM", p.at(p.R, 0, 0)},
        {"fKneeMhz", f_knee * 1e-6},
        {"kbDb", 20.0 * std::log10(std::max(p.kb(0, 1), 1e-12))}};
    out["spice"] = {
        {"sections", w.sections}, {"steps", (int)w.t.size()},
        {"delayNs", w.delay_s * 1e9}, {"dtPs", w.dt * 1e12},
        {"nextMv", w.next_peak_v * 1e3}, {"fextMv", w.fext_peak_v * 1e3},
        {"launchedV", w.launched_v},
        {"t", w.t}, {"aggFar", w.agg_far},
        {"vicNear", w.vic_near}, {"vicFar", w.vic_far}};
    out["verdict"] = {
        {"family", fam.id}, {"familyLabel", fam.label},
        {"budgetV", budget}, {"peakMv", peak * 1e3},
        {"pctOfBudget", pct},
        {"level", pct >= 50.0 ? "fail" : (pct >= 25.0 ? "watch" : "ok")}};

    if (r.want_fix && pct >= 25.0) {
        mna::DriveOptions o = r.drive;
        const double want = 0.25 * budget;
        const double g = gap_to_meet(r.section, o, want, 10);
        if (g > 0) {
            bem::PairSection s2 = r.section;
            s2.gap = g;
            const double after = peak_noise(extract(s2, f_knee).p, o);
            out["fix"] = {{"gapMm", g * 1e3},
                          {"fromMm", r.section.gap * 1e3},
                          {"peakMvAfter", after * 1e3},
                          {"pctAfter", 100.0 * after / budget}};
        } else {
            out["fix"] = nullptr;
        }
    } else {
        out["fix"] = nullptr;
    }

    const auto t3 = clk::now();
    if (r.want_field) {
        // frame the window on the structure with a margin, and always include
        // the reference plane at y = 0 so the return path is visible
        const double span = r.section.w1 + r.section.w2 + r.section.gap;
        const double top = r.section.stripline
                               ? r.section.b * 1.12
                               : (r.section.broadside
                                      ? r.section.h + r.section.h_v + 2.0 * r.section.h
                                      : r.section.h * 3.0);
        const double halfx = std::max(span * 0.9, top * 0.85);
        out["field"] = field_json(bem::sample_field(ex.solved, 0, -halfx, 0.0,
                                                    halfx, top, r.field_nx,
                                                    r.field_ny));
    }
    const auto t4 = clk::now();
    auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    out["timingMs"] = {{"extract", ms(t0, t1)}, {"transient", ms(t1, t2)},
                       {"fix", ms(t2, t3)}, {"field", ms(t3, t4)},
                       {"total", ms(t0, t4)}};
    return out;
}

// ---------------------------------------------------------------------------
// Radiated emissions
// ---------------------------------------------------------------------------

inline emc::Trapezoid trapezoid_from_json(const nlohmann::json& j) {
    emc::Trapezoid t;
    auto opt = [&](const char* k, double d) {
        return (j.contains(k) && j.at(k).is_number()) ? j.at(k).get<double>() : d;
    };
    t.amplitude_a = opt("currentA", 5.0);
    t.f_sw_hz = opt("fSwKhz", 500.0) * 1e3;
    t.duty = opt("duty", 0.4);
    t.rise_s = opt("riseNs", 20.0) * 1e-9;
    return t;
}

// A 50 kHz switcher puts 19 000 harmonics under 1 GHz, which is neither
// shippable as JSON nor drawable. Peak-hold into a bounded number of bins is
// what a spectrum analyser does anyway, so the shape and every excursion
// survive while the payload stays fixed.
inline nlohmann::json emissions_json(const emc::Prediction& p, int max_points) {
    if (max_points < 16) throw std::invalid_argument("emissions: need at least 16 bins");
    const size_t stride =
        std::max<size_t>(1, (p.harmonics.size() + max_points - 1) / max_points);
    nlohmann::json f = nlohmann::json::array(), line = nlohmann::json::array(),
                   env = nlohmann::json::array(), lim = nlohmann::json::array();
    for (size_t i = 0; i < p.harmonics.size(); i += stride) {
        const size_t end = std::min(i + stride, p.harmonics.size());
        const emc::Harmonic* peak = &p.harmonics[i];
        double e_env = -400;
        for (size_t k = i; k < end; ++k) {
            if (p.harmonics[k].e_dbuv_m > peak->e_dbuv_m) peak = &p.harmonics[k];
            e_env = std::max(e_env, p.harmonics[k].envelope_dbuv_m);
        }
        f.push_back(peak->f_hz * 1e-6);
        line.push_back(peak->e_dbuv_m);
        env.push_back(e_env);
        lim.push_back(peak->limit_dbuv_m);
    }
    return {{"fMhz", f}, {"lineDbuvM", line}, {"envelopeDbuvM", env},
            {"limitDbuvM", lim},
            {"worstMarginDb", p.worst_margin_db},
            {"worstHarmonicMarginDb", p.worst_harmonic_margin_db},
            {"worstFMhz", p.worst_f_hz * 1e-6},
            {"worstLevelDbuvM", p.worst_level_dbuv_m},
            {"plateauDbuvM", p.plateau_dbuv_m},
            {"kneeMhz", p.knee_hz * 1e-6},
            {"smallLoopMaxMhz", p.small_loop_max_hz * 1e-6},
            {"beyondModelCount", p.beyond_model_count},
            {"harmonicsUnresolved", p.harmonics_unresolved},
            {"binnedFrom", (int)p.harmonics.size()},
            {"limitId", p.limit_id}, {"limitLabel", p.limit_label},
            {"distanceM", p.distance_m}, {"areaMm2", p.area_mm2},
            {"level", p.worst_margin_db < 0 ? "fail"
                      : (p.worst_margin_db < 6 ? "watch" : "ok")}};
}

inline nlohmann::json predict_emissions(const nlohmann::json& j) {
    if (!j.contains("areaMm2") || !j.at("areaMm2").is_number())
        throw std::invalid_argument(
            "emissions: 'areaMm2' is required — the enclosed loop area is the "
            "whole point, and it is what the layout supplies");
    emc::PredictOptions o;
    o.limit_id = j.value("limit", std::string("cispr32b"));
    o.ground_reflection = j.value("groundReflection", true);
    return emissions_json(
        emc::predict_loop(j.at("areaMm2").get<double>(), trapezoid_from_json(j), o),
        j.value("maxPoints", 900));
}

// The common-mode budget. Faraday cannot know the common-mode current — it
// comes from ground-plane impedance and return-path detours, not from geometry
// — so the shipped answer is the inverse: how much current this cable may carry
// and still pass. That needs no unknowns.
inline nlohmann::json cm_budget_json(const nlohmann::json& j) {
    const double cable = j.value("cableM", 1.0);
    const emc::CmBudget b = emc::cm_budget(
        cable, j.value("limit", std::string("cispr32b")),
        j.value("groundReflection", true), j.value("points", 140));
    nlohmann::json f = nlohmann::json::array(), ua = nlohmann::json::array();
    for (const auto& p : b.points) {
        f.push_back(p.f_hz * 1e-6);
        ua.push_back(p.budget_a * 1e6);
    }
    return {{"fMhz", f}, {"budgetUa", ua},
            {"tightestUa", b.tightest_a * 1e6},
            {"tightestFMhz", b.tightest_f_hz * 1e-6},
            {"cableM", b.cable_m}, {"distanceM", b.distance_m},
            {"quarterWaveMhz", b.quarter_wave_hz * 1e-6},
            {"limitId", b.limit_id}, {"limitLabel", b.limit_label}};
}

// The return-path map: effective loop height per segment, quantised to one
// byte on a log scale over the span actually present. Geometry only — there is
// no current, no field and no dB anywhere in it, because none of those are
// derivable from a layout alone. The emissions panel carries the defensible
// far-field number for the loops it covers.
inline nlohmann::json return_path_json(const BoardIR& board,
                                       const Screener& screener,
                                       const rp::MapParams& p) {
    const rp::MapResult r = rp::compute(board, screener, p);
    const double lo = r.min_eff_mm, hi = r.max_eff_mm;
    const double span = (hi > lo && lo > 0) ? std::log(hi / lo) : 0.0;
    std::vector<unsigned char> heat(r.segments.size(), 0);
    for (size_t i = 0; i < r.segments.size(); ++i) {
        const double e = r.segments[i].eff_height_mm;
        if (!(e > 0)) continue;
        heat[i] = span > 1e-9
            ? (unsigned char)std::clamp(255.0 * std::log(e / lo) / span, 0.0, 255.0)
            : (unsigned char)128;
    }
    nlohmann::json worst = nlohmann::json::array();
    for (const auto& w : r.worst)
        worst.push_back({{"net", board.net_name(w.net)},
                         {"areaMm2", w.area_mm2},
                         {"worstEffMm", w.worst_eff_mm},
                         {"overVoid", w.over_void},
                         {"unstitched", w.unstitched},
                         {"noReference", w.no_reference}});
    return {{"heat", base64(heat)},
            {"segments", (int)r.segments.size()},
            {"counted", (int)r.counted},
            {"minEffHeightMm", r.min_eff_mm},
            {"maxEffHeightMm", r.max_eff_mm},
            {"noReferenceCount", (int)r.no_reference_count},
            {"overVoidCount", (int)r.over_void_count},
            {"layerChangeCount", (int)r.layer_change_count},
            {"unstitchedCount", (int)r.unstitched_count},
            {"worst", worst}};
}

inline rp::MapParams rp_params_from_json(const nlohmann::json& j) {
    rp::MapParams p;
    auto opt = [&](const char* k, double d) {
        return (j.contains(k) && j.at(k).is_number()) ? j.at(k).get<double>() : d;
    };
    p.no_reference_height_mm = opt("noReferenceHeightMm", 1.6);
    p.max_return_detour_mm = opt("maxReturnDetourMm", 25.0);
    return p;
}

// The component near-field map. Units are A/m and volts induced — never
// dBuV/m, never a limit line, never a margin. There is no reliable near-field
// to far-field transform and the UI has to say so.
inline nlohmann::json near_field_json(const BoardIR& board,
                                      const Screener& screener,
                                      const nfmap::MapParams& p) {
    const nfmap::MapResult r = nfmap::compute(board, screener, p);
    nlohmann::json ag = nlohmann::json::array();
    for (const auto& a : r.aggressors)
        ag.push_back({{"kind", a.kind},
                      {"net", a.net}, {"xMm", a.x_mm}, {"yMm", a.y_mm},
                      {"areaMm2", a.area_mm2}, {"momentAm2", a.moment_am2},
                      {"aEffMm", a.a_eff_mm}, {"validFromMm", a.valid_from_mm},
                      {"hull", [&] {
                          nlohmann::json h = nlohmann::json::array();
                          for (const auto& pt : a.hull)
                              h.push_back({pt.x, pt.y});
                          return h;
                      }()}});
    nlohmann::json vi = nlohmann::json::array();
    for (const auto& v : r.victims) {
        const auto& vc = nf::victim_by_id(v.victim_class);
        vi.push_back({{"component", v.component}, {"net", v.net},
                      {"class", v.victim_class}, {"classLabel", vc.label},
                      {"why", vc.why},
                      {"kind", vc.kind == nf::VictimKind::DcAccuracy
                                   ? "dc-accuracy" : "peak-volts"},
                      {"xMm", v.x_mm}, {"yMm", v.y_mm},
                      {"distanceMm", v.distance_mm},
                      {"hAPerM", v.h_a_per_m},
                      {"hDbuaM", nf::to_dbua_m(v.h_a_per_m)},
                      {"bMicroT", v.b_tesla * 1e6},
                      {"inducedMv", v.induced_v * 1e3},
                      {"thresholdMv", v.threshold_v * 1e3},
                      {"ratio", v.ratio},
                      {"dipoleValid", v.dipole_valid},
                      {"cosTheta", v.cos_theta},
                      {"oriented", v.oriented},
                      {"shieldDb", v.shield_db},
                      {"aggressor", v.aggressor},
                      {"level", v.ratio >= 1.0 ? "over"
                                : (v.ratio >= 0.25 ? "watch" : "ok")}});
    }
    nlohmann::json caps = nlohmann::json::array();
    for (const auto& ch : r.cap_hits)
        caps.push_back({{"component", ch.component}, {"net", ch.net},
                        {"class", ch.victim_class},
                        {"overlapMm2", ch.overlap_mm2},
                        {"c12Pf", ch.c12_f * 1e12},
                        {"dvMv", ch.dv_v * 1e3},
                        {"thresholdMv", ch.threshold_v * 1e3},
                        {"ratio", ch.ratio},
                        {"level", ch.ratio >= 1.0 ? "over"
                                  : (ch.ratio >= 0.25 ? "watch" : "ok")}});
    // Echo the cans with the SE each one earned at the ring frequency, so the
    // COLOUR MAP can apply the same attenuation as the victim table — the two
    // must never disagree about what a can does.
    nlohmann::json shj = nlohmann::json::array();
    for (const auto& sh : p.shields)
        shj.push_back({{"x1", sh.x1}, {"y1", sh.y1}, {"x2", sh.x2},
                       {"y2", sh.y2}, {"seDb", sh.se_db}});
    return {{"aggressors", ag}, {"victims", vi}, {"capacitive", caps},
            {"shields", shj},
            {"maxHAPerM", r.max_h},
            {"tooCloseCount", (int)r.too_close_count},
            {"shieldedVictims", (int)r.shielded_victims},
            {"probeHeightMm", r.probe_height_mm},
            {"ringMhz", r.ring_hz * 1e-6},
            {"ringCurrentA", p.ring_current_a},
            {"lambdaOver2PiMm", r.lambda_over_2pi_mm}};
}

inline nfmap::MapParams nfmap_params_from_json(const nlohmann::json& j) {
    nfmap::MapParams p;
    auto opt = [&](const char* k, double d) {
        return (j.contains(k) && j.at(k).is_number()) ? j.at(k).get<double>() : d;
    };
    p.sw_current_a = opt("currentA", 10.0);
    p.ring_current_a = opt("ringCurrentA", 2.0);
    p.ring_hz = opt("ringMhz", 130.0) * 1e6;
    p.f_sw_hz = opt("fSwKhz", 500.0) * 1e3;
    p.probe_height_mm = opt("probeHeightMm", 3.0);
    p.default_victim_area_mm2 = opt("victimAreaMm2", 4.0);
    // Inductor construction: the one attribute geometry cannot see. Factors
    // are conservative ring-frequency figures from the measured anchors.
    p.inductor_type = j.value("inductorType", std::string("unshielded"));
    p.inductor_k = p.inductor_type == "composite" ? 0.3
                 : p.inductor_type == "shielded" ? 0.35
                 : p.inductor_type == "semi" ? 0.65 : 1.0;
    // Shield cans, each with an SE its own material, wall and contact pitch
    // earn at the RING frequency — the dominant coupling case.
    if (j.contains("shields") && j.at("shields").is_array()) {
        for (const auto& sj : j.at("shields")) {
            shield::Rect r;
            r.x1 = sj.value("x1", 0.0); r.y1 = sj.value("y1", 0.0);
            r.x2 = sj.value("x2", 0.0); r.y2 = sj.value("y2", 0.0);
            shield::Can can;
            can.material = sj.value("material", std::string("tinsteel"));
            can.wall_mm = sj.value("wallMm", 0.2);
            can.seam_pitch_mm = sj.value("seamPitchMm", 5.0);
            r.se_db = shield::evaluate(can, p.ring_hz,
                                       shield::FieldKind::MagneticNear).se_db;
            p.shields.push_back(r);
        }
    }
    return p;
}

// What a shield can would actually buy, at the frequency in question. Never a
// single number: the answer is set by the WALL at low frequency and by the
// SEAM at high frequency, and quoting one figure hides which lever matters.
inline nlohmann::json shielding_json(const nlohmann::json& j) {
    shield::Can can;
    can.material = j.value("material", std::string("tinsteel"));
    can.wall_mm = j.value("wallMm", 0.2);
    can.seam_pitch_mm = j.value("seamPitchMm", 5.0);
    can.five_sided = j.value("fiveSided", true);
    const double f = j.value("fMhz", 130.0) * 1e6;

    nlohmann::json out = nlohmann::json::array();
    for (auto [kind, id] : {std::pair{shield::FieldKind::MagneticNear, "magnetic"},
                            std::pair{shield::FieldKind::ElectricNear, "electric"}}) {
        const shield::Verdict v = shield::evaluate(can, f, kind);
        out.push_back({{"field", id},
                       {"seDb", v.se_db},
                       {"absorptionDb", v.absorption_db},
                       {"apertureDb", v.aperture_db},
                       {"limitedBy", v.limited_by},
                       {"skinDepthUm", v.skin_depth_um},
                       {"wallsPerSkin", v.walls_per_skin},
                       {"permeabilityExtrapolated", v.permeability_extrapolated},
                       {"caveat", v.caveat}});
    }
    const shield::Material& m = shield::material_by_id(can.material);
    return {{"fMhz", f * 1e-6}, {"material", m.id}, {"materialLabel", m.label},
            {"materialNote", m.note}, {"wallMm", can.wall_mm},
            {"seamPitchMm", can.seam_pitch_mm}, {"fiveSided", can.five_sided},
            {"seamResonanceMhz", shield::seam_resonance_hz(can.seam_pitch_mm) * 1e-6},
            {"results", out}};
}

inline nlohmann::json shield_materials_json() {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& m : shield::materials())
        a.push_back({{"id", m.id}, {"label", m.label}, {"note", m.note},
                     {"muValidToMhz", m.mu_valid_to_hz * 1e-6}});
    return a;
}

inline nlohmann::json victim_classes_json() {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& v : nf::victim_classes())
        a.push_back({{"id", v.id}, {"label", v.label},
                     {"thresholdMv", v.threshold_v * 1e3},
                     {"kind", v.kind == nf::VictimKind::DcAccuracy
                                  ? "dc-accuracy" : "peak-volts"},
                     {"why", v.why}});
    return a;
}

// The PDN: rails, per-cap branches with the MEASURED mounting inductance, the
// impedance curve, and its anti-resonances. The target line is drawn by the
// UI from the user's transient current and allowed ripple — the model has no
// business inventing either.
inline nlohmann::json pdn_json(const BoardIR& board, const Screener& screener,
                               const nlohmann::json& j) {
    pdn::Params p;
    p.vrm_r_ohm = j.value("vrmROhm", 0.01);
    p.vrm_l_h = j.value("vrmLnH", 20.0) * 1e-9;
    const pdn::Result d = pdn::discover(board, screener, p);
    nlohmann::json rails = nlohmann::json::array();
    for (const auto& rail : d.rails) {
        const pdn::Curve c = pdn::curve(rail, p);
        nlohmann::json caps = nlohmann::json::array();
        for (const auto& b : rail.caps)
            caps.push_back({{"ref", b.ref},
                            {"cF", b.c_f},
                            {"cLabel", b.c_f >= 1e-6
                                 ? std::to_string(b.c_f * 1e6).substr(0, 4) + " uF"
                                 : std::to_string(b.c_f * 1e9).substr(0, 4) + " nF"},
                            {"eslNh", b.esl_h * 1e9},
                            {"lMountNh", b.l_mount_h * 1e9},
                            {"viaD1Mm", b.via_d1_mm}, {"viaD2Mm", b.via_d2_mm},
                            {"noVia", b.no_via},
                            {"fResMhz", b.f_res_hz * 1e-6}});
        nlohmann::json f = nlohmann::json::array(), z = nlohmann::json::array();
        for (size_t i = 0; i < c.f_hz.size(); ++i) {
            f.push_back(c.f_hz[i] * 1e-6);
            z.push_back(c.z_ohm[i]);
        }
        nlohmann::json ar = nlohmann::json::array();
        for (auto& [fa, za] : c.antires)
            ar.push_back({{"fMhz", fa * 1e-6}, {"zOhm", za}});
        rails.push_back({{"net", rail.name},
                         {"caps", caps},
                         {"skippedUnparsed", rail.skipped_unparsed},
                         {"planeCpF", rail.plane_c_f * 1e12},
                         {"planeOverlapMm2", rail.plane_overlap_mm2},
                         {"fMhz", f}, {"zOhm", z},
                         {"zMaxOhm", c.z_max_ohm},
                         {"zMaxMhz", c.z_max_hz * 1e-6},
                         {"antires", ar}});
    }
    return {{"gnd", d.gnd_name}, {"rails", rails},
            {"vrmROhm", p.vrm_r_ohm}, {"vrmLnH", p.vrm_l_h * 1e9}};
}

inline nlohmann::json limit_lines_json() {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& l : emc::limit_lines())
        a.push_back({{"id", l.id}, {"label", l.label}, {"distanceM", l.distance_m}});
    return a;
}

inline nlohmann::json families_json() {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& f : logic_families())
        a.push_back({{"id", f.id}, {"label", f.label},
                     {"vdd", f.vdd}, {"budgetV", f.budget()}});
    return a;
}

}  // namespace faraday::bench
