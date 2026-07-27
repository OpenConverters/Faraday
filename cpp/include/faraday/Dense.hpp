#pragma once
// Small dense linear algebra shared by the field solver and the circuit
// solver. Both factor a matrix once and then solve against it many times —
// the field solver for one right-hand side per conductor, the circuit solver
// for one per timestep — so the split between factor and solve is the whole
// point of the interface.

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace faraday {

// ---------------------------------------------------------------------------
// Dense LU with partial pivoting, factor once / solve many
// ---------------------------------------------------------------------------

struct Lu {
    size_t n = 0;
    std::vector<double> a;
    std::vector<size_t> piv;

    Lu(std::vector<double> m, size_t dim) : n(dim), a(std::move(m)), piv(dim) {
        if (a.size() != n * n)
            throw std::invalid_argument("lu: matrix is not square");
        for (size_t i = 0; i < n; ++i) piv[i] = i;
        for (size_t k = 0; k < n; ++k) {
            size_t p = k;
            for (size_t r = k + 1; r < n; ++r)
                if (std::abs(a[r * n + k]) > std::abs(a[p * n + k])) p = r;
            if (std::abs(a[p * n + k]) < 1e-300)
                throw std::invalid_argument(
                    "lu: matrix is singular — two conductors probably touch, or a "
                    "conductor carries no field");
            if (p != k) {
                for (size_t c = 0; c < n; ++c) std::swap(a[k * n + c], a[p * n + c]);
                std::swap(piv[k], piv[p]);
            }
            const double d = a[k * n + k];
            for (size_t r = k + 1; r < n; ++r) {
                const double f = a[r * n + k] / d;
                if (f == 0.0) continue;
                a[r * n + k] = f;
                for (size_t c = k + 1; c < n; ++c) a[r * n + c] -= f * a[k * n + c];
            }
        }
    }

    void solve(std::vector<double>& b) const {
        if (b.size() != n) throw std::invalid_argument("lu: rhs size mismatch");
        std::vector<double> x(n);
        for (size_t i = 0; i < n; ++i) x[i] = b[piv[i]];
        for (size_t i = 1; i < n; ++i)
            for (size_t j = 0; j < i; ++j) x[i] -= a[i * n + j] * x[j];
        for (size_t i = n; i-- > 0;) {
            for (size_t j = i + 1; j < n; ++j) x[i] -= a[i * n + j] * x[j];
            x[i] /= a[i * n + i];
        }
        b.swap(x);
    }
};

}  // namespace faraday
