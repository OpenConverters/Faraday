#pragma once
// Transient circuit solver for a coupled-line ladder — SPICE's inner loop,
// specialised to the one topology Faraday needs and therefore fast enough to
// sit behind a slider.
//
// WHY NOT SHIP ngspice. ngspice compiles to WebAssembly (EEcircuit and others
// do exactly that) and Faraday already drives the real thing natively through
// Kirchhoff. But a general simulator re-parses a netlist, rebuilds its matrix
// and re-factors it on every run, and a coupled-line ladder is a LINEAR circuit
// at a FIXED timestep: its MNA matrix never changes. Factor it once, and each
// timestep costs one triangular solve instead of a Newton iteration. That is
// the difference between "press run and wait" and "drag the separation slider
// and watch the noise move".
//
// Correctness is not traded away for that speed. spice_ladder_deck() in
// Rlgc.hpp emits the identical circuit for ngspice, and tools/faraday_xcheck
// runs both — this stepper and Kirchhoff's in-process libngspice — over the
// same four cross-sections. They agree on near-end crosstalk to 0.01 dB and on
// far-end to 0.05 dB. That is a native check (libngspice does not go in the
// browser); the Catch2 suite pins this solver against transmission-line theory
// directly, which is the stronger of the two claims anyway.
//
// FORMULATION. Modified nodal analysis with node voltages and inductor
// currents as unknowns (inductor currents have to be explicit — mutual
// coupling has no admittance form). Integration is trapezoidal, which for a
// lossless LC ladder is the right choice: it is A-stable and, unlike backward
// Euler, does not artificially damp the very ringing we are trying to show.
//
//   inductor bank:  v = R i + L di/dt
//                   v^n+1 - (R + 2L/h) i^n+1 = -v^n + (R - 2L/h) i^n
//   capacitor bank: i = C dv/dt
//                   i^n+1 = (2C/h) v^n+1 - [ (2C/h) v^n + i^n ]
//
// The capacitance stamp uses the full transmission-line matrix C rather than
// separate self and mutual lumps: Q = C V is what that matrix means, so the
// nodal stamp IS C, and no decomposition can go out of step with it.

#include "Dense.hpp"
#include "Rlgc.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace faraday::mna {

struct DriveOptions {
    double length_m = 0.04;      // coupled length
    double z_src = 50.0;         // aggressor driver output impedance, ohm
    double z_term = 1e6;         // far-end termination, ohm (open by default:
                                 // CMOS inputs are capacitive, not 50 ohm)
    double z_victim_near = 1e6;  // victim near-end load, ohm
    double rise_s = 1e-9;        // 0-100% edge
    double amplitude_v = 3.3;
    size_t aggressor = 0;
    int sections = 0;            // 0 = choose from the rise time
    int max_steps = 2400;        // ceiling on transient steps
};

struct Waveforms {
    std::vector<float> t;            // seconds
    std::vector<float> agg_near, agg_far;
    std::vector<float> vic_near;     // NEXT — backward crosstalk
    std::vector<float> vic_far;      // FEXT — forward crosstalk
    int sections = 0;
    double dt = 0;
    double delay_s = 0;              // one-way propagation delay
    double next_peak_v = 0, fext_peak_v = 0;
    double launched_v = 0;           // what the driver actually put on the line
};

// Sections needed so the ladder still behaves like a distributed line at the
// bandwidth the edge contains: a lumped section must be short compared with
// the rise time, the usual rule being ten sections per edge length.
inline int sections_for(double delay_s, double rise_s) {
    if (!(delay_s > 0) || !(rise_s > 0))
        throw std::invalid_argument("mna: delay and rise time must be positive");
    // The floor of 16 is set by accuracy, not by bandwidth: in a homogeneous
    // medium forward crosstalk is exactly zero, and the residual FEXT a finite
    // ladder produces is a direct readout of the discretisation error. It runs
    // 1.7% of NEXT at 8 sections and 0.3% at 32, so the floor buys a real
    // digit for a fraction of a millisecond.
    return std::clamp((int)std::ceil(15.0 * delay_s / rise_s), 16, 40);
}

inline Waveforms simulate(const Rlgc& p, const DriveOptions& o) {
    const size_t n = p.n;
    if (n < 2) throw std::invalid_argument("mna: need an aggressor and a victim");
    if (o.aggressor >= n) throw std::invalid_argument("mna: aggressor out of range");
    if (!(o.length_m > 0) || !(o.rise_s > 0) || !(o.amplitude_v > 0))
        throw std::invalid_argument("mna: length, rise time and amplitude must be > 0");
    if (!(o.z_src > 0) || !(o.z_term > 0) || !(o.z_victim_near > 0))
        throw std::invalid_argument("mna: terminations must be positive resistances");

    const size_t victim = (o.aggressor == 0) ? 1 : 0;
    // A single-ended edge on one line of a pair launches BOTH modes, which
    // travel at different speeds in mixed media. Size the ladder against the
    // slower of the two, so the section count is adequate for both.
    const double v_slow = (p.n == 2) ? std::min(p.v_even(), p.v_odd())
                                     : p.velocity(o.aggressor);
    const double delay = o.length_m / v_slow;
    const int N = o.sections > 0 ? o.sections : sections_for(delay, o.rise_s);
    const double dl = o.length_m / N;

    // How long to run for, and how finely.
    //
    // Length is set by reflections. On an unterminated line — what a CMOS
    // receiver actually presents — the first backward wave is not the worst
    // case: it re-reflects off the driver and adds to the next one, so the
    // envelope keeps growing for several round trips. On a matched line there
    // is nothing to wait for. Scaling the window by the loop gain |Gs*Gt|
    // therefore captures the true peak in the unterminated case without
    // spending steps on a matched one that settled after the first transit.
    const double z0m = p.z0(o.aggressor);
    auto gamma = [&](double z) { return std::abs((z - z0m) / (z + z0m)); };
    const double loop = gamma(o.z_src) * std::max(gamma(o.z_term), gamma(o.z_victim_near));
    const double rounds = 3.0 + 13.0 * std::clamp(loop, 0.0, 1.0);
    const double tstop = std::max(6.0 * o.rise_s, 2.0 * rounds * delay + 2.0 * o.rise_s);

    // Resolution is set by the EDGE, not by the window: near-end crosstalk is a
    // sharp feature lasting about one rise time, and sampling the window
    // uniformly would starve it whenever the line is long. Forty points per
    // edge matches the step ceiling spice_ladder_deck() hands ngspice.
    int steps = (int)std::ceil(tstop / (o.rise_s / 40.0));
    steps = std::clamp(steps, 200, o.max_steps);
    const double h = tstop / steps;

    // ---- unknown layout ---------------------------------------------------
    const size_t nn = n * (size_t)(N + 1);          // node voltages
    const size_t M = nn + n * (size_t)N;            // + inductor currents
    auto node = [&](size_t i, int k) { return (size_t)k * n + i; };
    auto cur = [&](size_t i, int k) { return nn + (size_t)k * n + i; };

    std::vector<double> A(M * M, 0.0);
    const double gs = 1.0 / o.z_src;
    const double gt = 1.0 / o.z_term;
    const double gvn = 1.0 / o.z_victim_near;

    // capacitor bank per node group: full dl*C, halved at the two ends so the
    // total is exactly length*C
    auto cscale = [&](int k) { return (k == 0 || k == N) ? 0.5 * dl : dl; };

    // KCL rows
    for (int k = 0; k <= N; ++k) {
        for (size_t i = 0; i < n; ++i) {
            const size_t r = node(i, k);
            if (k < N) A[r * M + cur(i, k)] += 1.0;        // leaves toward k+1
            if (k > 0) A[r * M + cur(i, k - 1)] -= 1.0;    // arrives from k-1
            for (size_t j = 0; j < n; ++j)
                A[r * M + node(j, k)] += 2.0 * p.at(p.C, i, j) * cscale(k) / h;
            if (k == 0) A[r * M + node(i, k)] += (i == o.aggressor) ? gs : gvn;
            if (k == N) A[r * M + node(i, k)] += gt;
        }
    }
    // inductor branch rows
    for (int k = 0; k < N; ++k) {
        for (size_t i = 0; i < n; ++i) {
            const size_t r = cur(i, k);
            A[r * M + node(i, k)] += 1.0;
            A[r * M + node(i, k + 1)] -= 1.0;
            for (size_t j = 0; j < n; ++j)
                A[r * M + cur(j, k)] -= (p.at(p.R, i, j) + 2.0 * p.at(p.L, i, j) / h) * dl;
        }
    }
    Lu lu(std::move(A), M);

    // ---- step -------------------------------------------------------------
    std::vector<double> x(M, 0.0), xprev(M, 0.0);
    std::vector<double> icap(nn, 0.0);              // capacitor branch history
    Waveforms w;
    w.sections = N;
    w.dt = h;
    w.delay_s = delay;
    w.t.reserve(steps + 1);

    auto src = [&](double t) {
        return o.amplitude_v * std::clamp(t / o.rise_s, 0.0, 1.0);
    };
    auto record = [&](double t) {
        w.t.push_back((float)t);
        w.agg_near.push_back((float)x[node(o.aggressor, 0)]);
        w.agg_far.push_back((float)x[node(o.aggressor, N)]);
        w.vic_near.push_back((float)x[node(victim, 0)]);
        w.vic_far.push_back((float)x[node(victim, N)]);
    };
    record(0.0);

    for (int s = 1; s <= steps; ++s) {
        const double t = s * h;
        std::vector<double> b(M, 0.0);
        for (int k = 0; k <= N; ++k)
            for (size_t i = 0; i < n; ++i) {
                const size_t r = node(i, k);
                double ieq = -icap[r];
                for (size_t j = 0; j < n; ++j)
                    ieq -= 2.0 * p.at(p.C, i, j) * cscale(k) / h * xprev[node(j, k)];
                b[r] = -ieq;
                if (k == 0 && i == o.aggressor) b[r] += gs * src(t);
            }
        for (int k = 0; k < N; ++k)
            for (size_t i = 0; i < n; ++i) {
                double rhs = -(xprev[node(i, k)] - xprev[node(i, k + 1)]);
                for (size_t j = 0; j < n; ++j)
                    rhs += (p.at(p.R, i, j) - 2.0 * p.at(p.L, i, j) / h) * dl *
                           xprev[cur(j, k)];
                b[cur(i, k)] = rhs;
            }
        lu.solve(b);
        x.swap(b);
        // advance the capacitor history: i^n+1 = (2C/h)(v^n+1 - v^n) - i^n
        for (int k = 0; k <= N; ++k)
            for (size_t i = 0; i < n; ++i) {
                const size_t r = node(i, k);
                double inew = -icap[r];
                for (size_t j = 0; j < n; ++j)
                    inew += 2.0 * p.at(p.C, i, j) * cscale(k) / h *
                            (x[node(j, k)] - xprev[node(j, k)]);
                icap[r] = inew;
            }
        xprev = x;
        record(t);
    }

    auto peak = [](const std::vector<float>& v) {
        float best = 0;
        for (float q : v) if (std::abs(q) > std::abs(best)) best = q;
        return (double)best;
    };
    w.next_peak_v = peak(w.vic_near);
    w.fext_peak_v = peak(w.vic_far);
    // What the driver actually launched, which is the only honest reference
    // for a crosstalk ratio: a 50 ohm driver into a 55 ohm line does not put
    // the full open-circuit swing on the trace.
    const double z0 = p.z0(o.aggressor);
    w.launched_v = o.amplitude_v * z0 / (z0 + o.z_src);
    return w;
}

}  // namespace faraday::mna
