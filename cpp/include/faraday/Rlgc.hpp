#pragma once
// RLGC matrices for a multiconductor line, and the ngspice deck that turns them
// into a real NEXT/FEXT waveform.
//
// Input is the pair of Maxwell capacitance matrices a 2D electrostatic solve
// produces: C with the real dielectrics, and C0 with everything replaced by
// vacuum. For a quasi-TEM line that is enough for both storage terms:
//
//     L = mu0 * eps0 * C0^-1          (Paul, Multiconductor Transmission Lines)
//
// The "signal" matrices used by a circuit model are the SHORT-CIRCUIT
// capacitances derived from the Maxwell matrix: C_ii(signal) = sum_j C_ij(Maxwell)
// (self to ground), C_ij(signal) = -C_ij(Maxwell) (mutual).
//
// The deck is an N-section lumped ladder with all-pairs mutual coupling rather
// than ngspice's CPL element: CPL takes RLGC directly but is convergence-flaky,
// while the ladder is robust and lets the caller trade sections against
// bandwidth. Same construction Hertz uses for coupled windings.

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace faraday {

inline constexpr double MU0_ = 4.0e-7 * 3.14159265358979323846;
inline constexpr double EPS0_ = 8.8541878128e-12;

// Dense symmetric inverse by Gauss-Jordan with partial pivoting. Matrices here
// are 2x2..8x8, so clarity beats sophistication.
inline std::vector<double> invert_matrix(std::vector<double> a, size_t n) {
    if (a.size() != n * n) throw std::invalid_argument("invert_matrix: size mismatch");
    std::vector<double> inv(n * n, 0.0);
    for (size_t i = 0; i < n; ++i) inv[i * n + i] = 1.0;
    for (size_t col = 0; col < n; ++col) {
        size_t piv = col;
        for (size_t r = col + 1; r < n; ++r)
            if (std::abs(a[r * n + col]) > std::abs(a[piv * n + col])) piv = r;
        if (std::abs(a[piv * n + col]) < 1e-300)
            throw std::invalid_argument(
                "invert_matrix: matrix is singular — the cross-section probably "
                "has a conductor that touches another or carries no field");
        if (piv != col)
            for (size_t k = 0; k < n; ++k) {
                std::swap(a[col * n + k], a[piv * n + k]);
                std::swap(inv[col * n + k], inv[piv * n + k]);
            }
        const double d = a[col * n + col];
        for (size_t k = 0; k < n; ++k) { a[col * n + k] /= d; inv[col * n + k] /= d; }
        for (size_t r = 0; r < n; ++r) {
            if (r == col) continue;
            const double f = a[r * n + col];
            if (f == 0.0) continue;
            for (size_t k = 0; k < n; ++k) {
                a[r * n + k] -= f * a[col * n + k];
                inv[r * n + k] -= f * inv[col * n + k];
            }
        }
    }
    return inv;
}

// Per-unit-length line parameters for n signal conductors (the reference
// conductor is NOT one of them — it is the return).
struct Rlgc {
    size_t n = 0;
    std::vector<double> L;   // H/m, n x n
    std::vector<double> C;   // F/m, n x n signal capacitances (self-to-ground + mutual)
    std::vector<double> R;   // ohm/m, diagonal (DC; skin effect needs HarmonicEddy)
    double at(const std::vector<double>& m, size_t i, size_t j) const {
        return m[i * n + j];
    }
    // Physical two-terminal capacitances recovered from the matrix:
    //   mutual i-j, and line i's own capacitance to the reference.
    double c_mutual(size_t i, size_t j) const { return -at(C, i, j); }
    double c_to_ref(size_t i) const {
        double c = at(C, i, i);
        for (size_t j = 0; j < n; ++j)
            if (j != i) c += at(C, i, j);   // off-diagonals are negative
        return c;
    }
    // characteristic impedance of line i with the others grounded
    double z0(size_t i) const { return std::sqrt(at(L, i, i) / at(C, i, i)); }
    double velocity(size_t i) const {
        return 1.0 / std::sqrt(at(L, i, i) * at(C, i, i));
    }
    // Backward (near-end) coupling coefficient, Paul eq. 5: the average of the
    // normalised mutual inductance and mutual capacitance, quartered.
    double kb(size_t i, size_t j) const {
        const double lm = at(L, i, j) / std::sqrt(at(L, i, i) * at(L, j, j));
        const double cm = c_mutual(i, j) / at(C, i, i);
        return 0.25 * (lm + cm);
    }
};

// Build RLGC from the two Maxwell matrices produced by the field solver.
// `maxwell` and `maxwell_vacuum` are (n+1)x(n+1) over ALL conductors, with
// `ref` the index of the reference/return conductor.
inline Rlgc rlgc_from_maxwell(const std::vector<double>& maxwell,
                              const std::vector<double>& maxwell_vacuum,
                              size_t n_total, size_t ref,
                              const std::vector<double>& r_dc_per_m = {}) {
    if (n_total < 2) throw std::invalid_argument("rlgc: need at least 2 conductors");
    if (ref >= n_total) throw std::invalid_argument("rlgc: reference index out of range");
    if (maxwell.size() != n_total * n_total ||
        maxwell_vacuum.size() != n_total * n_total)
        throw std::invalid_argument("rlgc: matrix size mismatch");

    // signal conductors, in order, skipping the reference
    std::vector<size_t> sig;
    for (size_t i = 0; i < n_total; ++i)
        if (i != ref) sig.push_back(i);
    const size_t n = sig.size();

    // The transmission-line capacitance matrix is the Maxwell matrix with the
    // REFERENCE row and column deleted (Paul, ch. 5):
    //     C_ii = c_i0 + sum_{j!=i} c_ij       (positive)
    //     C_ij = -c_ij                        (negative)
    // where c_ij are the physical two-terminal capacitances. Do NOT take row
    // sums of the Maxwell matrix: with the return conductor present every field
    // line terminates on another conductor, so those sums are zero.
    auto to_signal = [&](const std::vector<double>& mx) {
        std::vector<double> c(n * n, 0.0);
        for (size_t a = 0; a < n; ++a)
            for (size_t b = 0; b < n; ++b)
                c[a * n + b] = mx[sig[a] * n_total + sig[b]];
        return c;
    };

    Rlgc out;
    out.n = n;
    out.C = to_signal(maxwell);
    const std::vector<double> C0 = to_signal(maxwell_vacuum);
    // L = mu0 eps0 C0^-1 — the whole reason the vacuum solve exists
    std::vector<double> Linv = invert_matrix(C0, n);
    out.L.assign(n * n, 0.0);
    for (size_t i = 0; i < n * n; ++i) out.L[i] = MU0_ * EPS0_ * Linv[i];
    // keep L symmetric against round-off rather than shipping a lopsided matrix
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            const double s = 0.5 * (out.L[i * n + j] + out.L[j * n + i]);
            out.L[i * n + j] = out.L[j * n + i] = s;
        }
    out.R.assign(n * n, 0.0);
    for (size_t i = 0; i < n && i < r_dc_per_m.size(); ++i)
        out.R[i * n + i] = r_dc_per_m[i];
    return out;
}

struct DeckOptions {
    int sections = 24;          // >= ~10 per wavelength at f_max
    double length_m = 0.04;     // coupled length
    double z_src = 50.0;        // driver output impedance, ohm
    double z_term = 50.0;       // far-end termination, ohm
    double rise_s = 1e-9;       // aggressor edge rate
    double amplitude_v = 3.3;
    double tstop_s = 2e-8;
    size_t aggressor = 0;       // index of the driven line
};

// N-section lumped ladder with all-pairs K coupling. Line i node names are
// n<i>_<k>; the aggressor is driven through z_src, every line is terminated at
// both ends, and NEXT/FEXT appear at the victim's near and far nodes.
inline std::string spice_ladder_deck(const Rlgc& p, const DeckOptions& o) {
    if (p.n == 0) throw std::invalid_argument("deck: no signal conductors");
    if (o.aggressor >= p.n) throw std::invalid_argument("deck: aggressor out of range");
    if (o.sections < 1) throw std::invalid_argument("deck: need at least one section");
    const double dl = o.length_m / o.sections;

    std::ostringstream s;
    s.setf(std::ios::scientific);
    s.precision(9);
    s << "* Faraday coupled-line ladder — " << p.n << " signal lines, "
      << o.sections << " sections over " << o.length_m << " m\n";
    s << "* L and C per metre come from a 2D field solve; L via mu0*eps0*C0^-1.\n";

    // aggressor drive: trapezoidal edge through the source impedance
    s << "Vagg src 0 PWL(0 0 " << o.rise_s << " " << o.amplitude_v
      << " " << o.tstop_s << " " << o.amplitude_v << ")\n";
    s << "Rsrc src n" << o.aggressor << "_0 " << o.z_src << "\n";
    for (size_t i = 0; i < p.n; ++i) {
        if (i == o.aggressor) continue;
        s << "Rnear" << i << " n" << i << "_0 0 " << o.z_src << "\n";
    }
    for (size_t i = 0; i < p.n; ++i)
        s << "Rfar" << i << " n" << i << "_" << o.sections << " 0 " << o.z_term << "\n";

    for (int k = 0; k < o.sections; ++k) {
        for (size_t i = 0; i < p.n; ++i) {
            const double Ri = p.at(p.R, i, i) * dl;
            std::string a = "n" + std::to_string(i) + "_" + std::to_string(k);
            std::string b = "n" + std::to_string(i) + "_" + std::to_string(k + 1);
            std::string mid = a;
            if (Ri > 0) {
                mid = "r" + std::to_string(i) + "_" + std::to_string(k);
                s << "R" << i << "_" << k << " " << a << " " << mid << " " << Ri << "\n";
            }
            s << "L" << i << "_" << k << " " << mid << " " << b << " "
              << p.at(p.L, i, i) * dl << "\n";
            // capacitance to the reference — the PHYSICAL c_i0, i.e. the
            // diagonal minus the mutuals the matrix folds into it
            s << "C" << i << "_" << k << " " << b << " 0 "
              << p.c_to_ref(i) * dl << "\n";
        }
        // mutual capacitance and inductance, all pairs
        for (size_t i = 0; i < p.n; ++i)
            for (size_t j = i + 1; j < p.n; ++j) {
                const double cm = p.c_mutual(i, j) * dl;
                if (cm != 0.0)
                    s << "Cm" << i << j << "_" << k << " n" << i << "_" << (k + 1)
                      << " n" << j << "_" << (k + 1) << " " << cm << "\n";
                const double lii = p.at(p.L, i, i), ljj = p.at(p.L, j, j);
                const double kc = p.at(p.L, i, j) / std::sqrt(lii * ljj);
                if (std::abs(kc) > 1e-9) {
                    if (std::abs(kc) >= 1.0)
                        throw std::invalid_argument(
                            "deck: coupling coefficient |k| >= 1 between lines " +
                            std::to_string(i) + " and " + std::to_string(j) +
                            " — the inductance matrix is not physical");
                    s << "K" << i << j << "_" << k << " L" << i << "_" << k
                      << " L" << j << "_" << k << " " << kc << "\n";
                }
            }
    }

    s << ".tran " << (o.rise_s / 50.0) << " " << o.tstop_s << " 0 "
      << (o.rise_s / 50.0) << "\n";
    s << ".end\n";
    return s.str();
}

// Peak crosstalk read off a transient run of the ladder above.
//   near  = victim's driven end   (backward / NEXT)
//   far   = victim's far end      (forward / FEXT)
// Values are the peak EXCURSION from the quiescent level, signed so the
// direction of the disturbance is preserved.
struct CrosstalkPeaks {
    double next_v = 0, fext_v = 0;
    double next_db = 0, fext_db = 0;   // relative to the aggressor amplitude
};

inline CrosstalkPeaks crosstalk_from_waveforms(const std::vector<double>& near,
                                               const std::vector<double>& far,
                                               double aggressor_v) {
    if (near.empty() || far.empty())
        throw std::invalid_argument("crosstalk: empty waveform — the deck ran but "
                                    "the victim node was not captured");
    if (aggressor_v <= 0)
        throw std::invalid_argument("crosstalk: aggressor amplitude must be > 0");
    auto peak = [](const std::vector<double>& v) {
        double best = 0.0;
        for (double x : v) if (std::abs(x) > std::abs(best)) best = x;
        return best;
    };
    CrosstalkPeaks p;
    p.next_v = peak(near);
    p.fext_v = peak(far);
    // a genuinely zero excursion is -inf dB; report a floor rather than nan
    auto db = [&](double v) {
        const double r = std::abs(v) / aggressor_v;
        return r > 0 ? 20.0 * std::log10(r) : -300.0;
    };
    p.next_db = db(p.next_v);
    p.fext_db = db(p.fext_v);
    return p;
}

}  // namespace faraday
