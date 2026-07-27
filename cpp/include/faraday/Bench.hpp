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
#include "Mna.hpp"
#include "Rlgc.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
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
    r.section.w1 = need("w1Mm") * 1e-3;
    r.section.w2 = need("w2Mm") * 1e-3;
    r.section.t = opt("tMm", 0.035) * 1e-3;
    r.section.eps_r = need("epsR");
    r.section.gap = opt("gapMm", 0.2) * 1e-3;
    r.section.h = opt("hMm", 0.2) * 1e-3;
    r.section.h_v = opt("hvMm", 0.2) * 1e-3;
    r.section.lateral = opt("lateralMm", 0.0) * 1e-3;
    r.section.b = opt("bMm", 0.6) * 1e-3;

    r.drive.length_m = need("lengthMm") * 1e-3;
    r.drive.rise_s = opt("riseNs", 1.0) * 1e-9;
    r.drive.amplitude_v = opt("amplitudeV", 3.3);
    r.drive.z_src = opt("zSrcOhm", 30.0);
    r.drive.z_term = opt("zTermOhm", 10000.0);
    r.drive.z_victim_near = opt("zVictimOhm", 30.0);
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

inline Extraction extract(const bem::PairSection& s) {
    bem::Geometry g = bem::geometry_for(s);
    bem::Solution sd = bem::solve(g, false);
    bem::Solution sv = bem::solve(g, true);
    const size_t nt = 3;
    std::vector<double> M(nt * nt, 0.0), M0(nt * nt, 0.0);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            M[i * nt + j] = sd.at(i, j);
            M0[i * nt + j] = sv.at(i, j);
        }
    Extraction e;
    e.p = rlgc_from_maxwell(M, M0, nt, 2);
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
    const double lo0 = base.gap;
    double hi = std::max(base.gap, base.h) * 12.0;
    s.gap = hi;
    if (peak_noise(extract(s).p, o) > target_v) return 0.0;
    double lo = lo0;
    for (int i = 0; i < iters; ++i) {
        const double mid = 0.5 * (lo + hi);
        s.gap = mid;
        if (peak_noise(extract(s).p, o) > target_v) lo = mid;
        else hi = mid;
    }
    return hi;
}

inline nlohmann::json run(const Request& r) {
    using clk = std::chrono::steady_clock;
    const LogicFamily& fam = family_by_id(r.family);

    const auto t0 = clk::now();
    Extraction ex = extract(r.section);
    const auto t1 = clk::now();
    mna::Waveforms w = mna::simulate(ex.p, r.drive);
    const auto t2 = clk::now();

    const Rlgc& p = ex.p;
    const double z0 = p.z0(0);
    // Effective permittivity is a MODAL quantity. Taking it from the diagonal
    // of L and C ignores the off-diagonal terms and reports a value above the
    // laminate's own eps_r for a buried pair, which is not a thing that can
    // happen. The even and odd modes are the ones that propagate.
    const double eps_even = std::pow(bem::C_LIGHT / p.v_even(), 2.0);
    const double eps_odd = std::pow(bem::C_LIGHT / p.v_odd(), 2.0);
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
        {"zEven", p.z_even()},
        {"zOdd", p.z_odd()},
        {"zDiff", 2.0 * p.z_odd()},
        {"velocity", 0.5 * (p.v_even() + p.v_odd())},
        {"epsEff", eps_eff},
        {"epsEffEven", eps_even},
        {"epsEffOdd", eps_odd},
        {"delayPsPerMm", 2e12 / (p.v_even() + p.v_odd()) * 1e-3},
        {"lSelfNhPerMm", p.at(p.L, 0, 0) * 1e9 * 1e-3},
        {"cSelfPfPerMm", p.c_to_ref(0) * 1e12 * 1e-3},
        {"lMutualNhPerMm", p.at(p.L, 0, 1) * 1e9 * 1e-3},
        {"cMutualPfPerMm", p.c_mutual(0, 1) * 1e12 * 1e-3},
        {"kb", p.kb(0, 1)},
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
            const double after = peak_noise(extract(s2).p, o);
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

inline nlohmann::json families_json() {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& f : logic_families())
        a.push_back({{"id", f.id}, {"label", f.label},
                     {"vdd", f.vdd}, {"budgetV", f.budget()}});
    return a;
}

}  // namespace faraday::bench
