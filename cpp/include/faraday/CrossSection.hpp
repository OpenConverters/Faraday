#pragma once
// Extract a 2D cross-section through a coupled pair, perpendicular to the run,
// and write it as a gmsh mesh for the field solver.
//
// The reference plane MUST be in the section: without the return path in the
// picture the inductance is meaningless. Every conductor that carries the
// forward or return current appears as its own electrode so the solve produces
// a full Maxwell matrix, not a single number.
//
// Geometry (all mm in, metres out — the solver works in SI):
//
//     y
//     ^   [ trace A ]   [ trace B ]        <- signal layer, at height h
//     |  ---------------------------       <- dielectric
//     |  ###########################       <- reference plane
//     +--------------------------------> x
//
// The domain extends sideways and upwards by `margin_factor` x the dielectric
// height so the fringing field decays before it meets the boundary. That outer
// boundary is left NATURAL (no charge sink), matching the axisymmetric solver's
// default: field lines close between electrodes, which is what a transmission
// line does.

#include "BoardIR.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace faraday {

struct SectionConductor {
    std::string name;      // conductor_a / conductor_b / conductor_gnd
    double x0, y0, x1, y1; // rectangle, metres
};

struct CrossSection {
    // metres throughout
    double width = 0, height = 0;
    std::vector<SectionConductor> conductors;
    // dielectric slabs, bottom-to-top: (y0, y1, eps_r)
    struct Slab { double y0, y1, eps_r; };
    std::vector<Slab> slabs;
    int nx = 240, ny = 120;

    double eps_at(double y) const {
        for (const auto& s : slabs)
            if (y >= s.y0 && y < s.y1) return s.eps_r;
        return 1.0;   // above the board: air
    }

    // The eddy-current formulation names conductors turn_<role>_<leg>, with the
    // leg fixing the current's sign: the driven trace is the forward path, the
    // reference plane the return, and the victim carries no NET current but
    // still develops proximity currents (which is exactly what makes R(f) rise).
    static std::string eddy_name(const std::string& electrostatic_name) {
        if (electrostatic_name == "conductor_a") return "turn_sig_plus";
        if (electrostatic_name == "conductor_b") return "turn_vic_plus";
        if (electrostatic_name == "conductor_gnd") return "turn_ret_minus";
        return electrostatic_name;
    }

    // Which region a point belongs to. Conductors win over dielectric.
    std::string region_at(double x, double y) const {
        for (const auto& c : conductors)
            if (x >= c.x0 && x <= c.x1 && y >= c.y0 && y <= c.y1) return c.name;
        double e = eps_at(y);
        // one region per distinct permittivity keeps the mesh attribute count low
        char buf[32];
        std::snprintf(buf, sizeof buf, "dielectric_%.3f", e);
        return buf;
    }

    // Permittivity per region name, for the solver's Problem.
    std::vector<std::pair<std::string, double>> region_permittivity() const {
        std::vector<std::pair<std::string, double>> out;
        std::vector<double> seen;
        auto add = [&](double e) {
            for (double s : seen) if (std::abs(s - e) < 1e-9) return;
            seen.push_back(e);
            char buf[32];
            std::snprintf(buf, sizeof buf, "dielectric_%.3f", e);
            out.push_back({buf, e});
        };
        for (const auto& s : slabs) add(s.eps_r);
        add(1.0);   // air above the stack
        return out;
    }

    // Optional non-uniform node coordinates. Empty => the uniform nx/ny grid.
    // A GRADED axis puts small cells at the conductor surfaces (where the skin
    // effect crowds the current and where the charge singularity lives) and
    // lets them grow geometrically away, so GHz solves become affordable: a
    // uniform grid fine enough for a 2 um skin depth over a 3 mm section would
    // need millions of cells, while grading needs thousands.
    std::vector<double> xs, ys;

    // Build a graded 1-D axis over [lo,hi] with `fine` spacing at each feature
    // coordinate, growing by `growth` per cell away from it, capped at n_max.
    static std::vector<double> graded_axis(std::vector<double> features,
                                           double lo, double hi, double fine,
                                           double growth, size_t n_max) {
        if (!(hi > lo)) throw BoardError("graded_axis: empty interval");
        if (fine <= 0 || growth <= 1.0)
            throw BoardError("graded_axis: need fine > 0 and growth > 1");
        std::sort(features.begin(), features.end());
        features.erase(std::unique(features.begin(), features.end(),
                                   [&](double a, double b) {
                                       return std::abs(a - b) < fine * 0.25;
                                   }),
                       features.end());
        // Local target size = fine, relaxed geometrically with distance to the
        // nearest feature. March across the interval placing nodes at that size.
        // Cell size as a function of distance from the nearest feature. Growing
        // geometrically PER CELL means that after k cells the size is
        // fine*growth^k and the distance covered is fine*(growth^k-1)/(growth-1);
        // eliminating k gives the closed form
        //     size(d) = fine + d*(growth-1)
        // which is linear and well behaved. Writing it as growth^(d/fine)
        // instead overflows immediately — d/fine is in the hundreds — and
        // collapses to whatever clamp follows, which is how a "graded" mesh
        // came out at 19x19 cells.
        auto target = [&](double x) {
            double d = 1e300;
            for (double f : features) d = std::min(d, std::abs(x - f));
            return fine + d * (growth - 1.0);
        };
        std::vector<double> nodes{lo};
        while (nodes.back() < hi && nodes.size() < n_max) {
            const double x = nodes.back();
            nodes.push_back(x + std::max(target(x), fine * 0.5));
        }
        if (nodes.size() >= n_max)
            throw BoardError("graded_axis: " + std::to_string(n_max) +
                             " nodes were not enough to span the section at " +
                             std::to_string(fine * 1e6) +
                             " um resolution — coarsen `fine` or raise the cap");
        nodes.back() = hi;
        // BOUNDARY LAYER. The march arrives at a feature with a partly-grown
        // cell, so the cell touching a conductor ends up ~2x `fine` — which is
        // exactly the cell that has to resolve the skin depth. Insert nodes at
        // fixed `fine` steps either side of every feature so the surface cells
        // really are `fine`, and put a node ON the feature so material
        // boundaries land on a cell edge rather than being smeared across one.
        const int layers = 6;
        for (double f : features) {
            for (int k = -layers; k <= layers; ++k) {
                const double x = f + k * fine;
                if (x > lo && x < hi) nodes.push_back(x);
            }
        }
        std::sort(nodes.begin(), nodes.end());
        // drop duplicates and slivers (a node closer than a tenth of `fine` to
        // its neighbour only makes the matrix worse)
        std::vector<double> out{nodes.front()};
        for (double x : nodes)
            if (x - out.back() > fine * 0.1) out.push_back(x);
        if (out.back() < hi) out.back() = hi;
        if (out.size() >= n_max)
            throw BoardError("graded_axis: boundary layers pushed the node count "
                             "past the cap — coarsen `fine` or raise it");
        return out;
    }

    // Largest cell that touches any conductor boundary, in metres. This is what
    // actually limits a skin-effect solve — NOT the size that was requested, so
    // callers should check the mesh they got rather than the one they asked for.
    double max_cell_at_conductors() const {
        std::vector<double> X = xs, Y = ys;
        if (X.empty()) { X.resize(nx + 1); for (int i = 0; i <= nx; ++i) X[i] = width * i / nx; }
        if (Y.empty()) { Y.resize(ny + 1); for (int j = 0; j <= ny; ++j) Y[j] = height * j / ny; }
        double worst = 0.0;
        auto scan = [&](const std::vector<double>& A, double f) {
            for (size_t i = 0; i + 1 < A.size(); ++i)
                if (f >= A[i] && f <= A[i + 1])
                    worst = std::max(worst, A[i + 1] - A[i]);
        };
        for (const auto& c : conductors) {
            scan(X, c.x0); scan(X, c.x1);
            scan(Y, c.y0); scan(Y, c.y1);
        }
        return worst;
    }

    // Grade both axes so cells at conductor boundaries are `fine` metres.
    void grade_for(double fine, double growth = 1.25, size_t n_max = 4000) {
        std::vector<double> fx, fy;
        for (const auto& c : conductors) {
            fx.push_back(c.x0); fx.push_back(c.x1);
            fy.push_back(c.y0); fy.push_back(c.y1);
        }
        for (const auto& s : slabs) { fy.push_back(s.y0); fy.push_back(s.y1); }
        xs = graded_axis(fx, 0.0, width, fine, growth, n_max);
        ys = graded_axis(fy, 0.0, height, fine, growth, n_max);
        nx = (int)xs.size() - 1;
        ny = (int)ys.size() - 1;
    }

    // gmsh 2.2 quad mesh with named physical groups (MFEM reads those as
    // attribute sets, which is how the formulation finds its electrodes).
    // `eddy` switches the conductor region names to the turn_* convention the
    // harmonic eddy-current solver expects; the geometry is identical.
    void write_gmsh(const std::string& path, bool eddy = false) const {
        auto emit = [&](const std::string& n) { return eddy ? eddy_name(n) : n; };
        // conductor names are registered first so their physical-group ids are
        // stable regardless of which cell happens to be visited first
        std::vector<std::string> names;
        for (const auto& c : conductors) names.push_back(emit(c.name));
        auto region_id = [&](const std::string& n) {
            for (size_t i = 0; i < names.size(); ++i)
                if (names[i] == n) return (int)i + 1;
            names.push_back(n);
            return (int)names.size();
        };
        // node coordinates: graded when grade_for() was called, else uniform
        std::vector<double> X = xs, Y = ys;
        if (X.empty()) { X.resize(nx + 1); for (int i = 0; i <= nx; ++i) X[i] = width * i / nx; }
        if (Y.empty()) { Y.resize(ny + 1); for (int j = 0; j <= ny; ++j) Y[j] = height * j / ny; }
        const int NX = (int)X.size() - 1, NY = (int)Y.size() - 1;
        std::vector<int> tag((size_t)NX * NY);
        std::vector<int> cells_per_conductor(conductors.size(), 0);
        for (int j = 0; j < NY; ++j)
            for (int i = 0; i < NX; ++i) {
                const std::string r = region_at(0.5 * (X[i] + X[i + 1]),
                                                0.5 * (Y[j] + Y[j + 1]));
                tag[(size_t)j * NX + i] = region_id(emit(r));
                for (size_t c = 0; c < conductors.size(); ++c)
                    if (conductors[c].name == r) ++cells_per_conductor[c];
            }
        // A conductor thinner than a cell falls between sample points and
        // disappears from the mesh — the solve would then return a capacitance
        // matrix that silently omits an electrode. Refuse instead of shipping a
        // wrong answer.
        for (size_t c = 0; c < conductors.size(); ++c)
            if (cells_per_conductor[c] == 0)
                throw BoardError(
                    "cross-section: conductor '" + conductors[c].name +
                    "' is thinner than one mesh cell and would vanish from the "
                    "mesh — increase nx/ny for this geometry");

        std::ofstream os(path);
        if (!os) throw BoardError("cross-section: cannot write " + path);
        os << "$MeshFormat\n2.2 0 8\n$EndMeshFormat\n";
        os << "$PhysicalNames\n" << names.size() << "\n";
        for (size_t i = 0; i < names.size(); ++i)
            os << "2 " << (i + 1) << " \"" << names[i] << "\"\n";
        os << "$EndPhysicalNames\n";
        const int nvx = NX + 1, nvy = NY + 1;
        os << "$Nodes\n" << (nvx * nvy) << "\n";
        for (int j = 0; j < nvy; ++j)
            for (int i = 0; i < nvx; ++i)
                os << (j * nvx + i + 1) << " " << X[i] << " " << Y[j] << " 0\n";
        os << "$EndNodes\n";
        os << "$Elements\n" << (NX * NY) << "\n";
        for (int j = 0; j < NY; ++j)
            for (int i = 0; i < NX; ++i) {
                const int e = j * NX + i, v0 = j * nvx + i + 1;
                os << (e + 1) << " 3 2 " << tag[e] << " " << tag[e] << " " << v0
                   << " " << (v0 + 1) << " " << (v0 + nvx + 1) << " " << (v0 + nvx)
                   << "\n";
            }
        os << "$EndElements\n";
    }
};

// Build the cross-section for two nets coupled on `cu`, referenced to the
// nearest plane. Separations and widths come from the screener's finding.
//
//   w_a, w_b   trace widths, mm
//   sep        centre-to-centre separation, mm
//   h          dielectric height to the reference plane, mm
//   t          copper thickness, mm
//   eps_r      dielectric permittivity
inline CrossSection make_coupled_section(double w_a, double w_b, double sep,
                                         double h, double t, double eps_r,
                                         double margin_factor = 6.0) {
    if (w_a <= 0 || w_b <= 0 || h <= 0 || t <= 0)
        throw BoardError("cross-section: widths, height and thickness must be > 0");
    if (sep < 0.5 * (w_a + w_b))
        throw BoardError("cross-section: separation " + std::to_string(sep) +
                         " mm is smaller than the two half-widths — the traces "
                         "would overlap");
    const double mm = 1e-3;
    CrossSection cs;
    const double margin = margin_factor * h * mm;
    const double span = sep * mm + 0.5 * (w_a + w_b) * mm;
    cs.width = span + 2 * margin;
    cs.height = t * mm + h * mm + t * mm + margin;   // plane + dielectric + trace + air

    const double y_plane0 = 0.0, y_plane1 = t * mm;
    const double y_trace0 = y_plane1 + h * mm, y_trace1 = y_trace0 + t * mm;
    cs.slabs.push_back({y_plane1, y_trace0, eps_r});

    // centre the pair in the domain
    const double xc = cs.width * 0.5;
    const double xa = xc - sep * mm * 0.5, xb = xc + sep * mm * 0.5;
    cs.conductors.push_back({"conductor_gnd", 0.0, y_plane0, cs.width, y_plane1});
    cs.conductors.push_back({"conductor_a", xa - w_a * mm * 0.5, y_trace0,
                             xa + w_a * mm * 0.5, y_trace1});
    cs.conductors.push_back({"conductor_b", xb - w_b * mm * 0.5, y_trace0,
                             xb + w_b * mm * 0.5, y_trace1});

    // resolve the thinnest feature (copper thickness) with a few cells
    const double target = std::min({t * mm, w_a * mm, w_b * mm, h * mm}) / 3.0;
    cs.nx = std::clamp((int)std::lround(cs.width / target), 120, 900);
    cs.ny = std::clamp((int)std::lround(cs.height / target), 80, 600);
    return cs;
}

// Symmetric coupled STRIPLINE: two traces centred between two planes, with one
// homogeneous dielectric filling everything. That homogeneity is the point —
// the mode is pure TEM, so Cohn's conformal mapping gives Z_even/Z_odd exactly
// and the numerical extraction's MUTUAL terms can be checked against a closed
// form rather than against another approximation.
//
//   w   trace width, mm      s   edge-to-edge gap, mm
//   b   plane-to-plane spacing, mm     t  copper thickness, mm
inline CrossSection make_coupled_stripline(double w, double s, double b,
                                           double t, double eps_r,
                                           double margin_factor = 6.0) {
    if (w <= 0 || s <= 0 || b <= 0 || t < 0)
        throw BoardError("stripline section: w, s, b must be > 0");
    if (t >= b) throw BoardError("stripline section: copper thicker than the gap");
    const double mm = 1e-3;
    CrossSection cs;
    const double margin = margin_factor * b * mm;
    const double span = (2 * w + s) * mm;
    cs.width = span + 2 * margin;
    // planes at y=0..t and y=b+t..b+2t, dielectric between, traces centred
    const double y_p0 = 0.0, y_p1 = t * mm;
    const double y_top0 = y_p1 + b * mm, y_top1 = y_top0 + t * mm;
    cs.height = y_top1;
    cs.slabs.push_back({y_p1, y_top0, eps_r});

    const double y_mid = 0.5 * (y_p1 + y_top0);
    const double yt0 = y_mid - 0.5 * t * mm, yt1 = y_mid + 0.5 * t * mm;
    const double xc = cs.width * 0.5;
    const double xa = xc - 0.5 * (w + s) * mm, xb = xc + 0.5 * (w + s) * mm;
    cs.conductors.push_back({"conductor_gnd", 0.0, y_p0, cs.width, y_p1});
    cs.conductors.push_back({"conductor_a", xa - w * mm * 0.5, yt0,
                             xa + w * mm * 0.5, yt1});
    cs.conductors.push_back({"conductor_b", xb - w * mm * 0.5, yt0,
                             xb + w * mm * 0.5, yt1});
    // the upper plane is the SAME net as the lower one: both are the return,
    // so they must be one electrode or the extraction sees three conductors
    cs.conductors.push_back({"conductor_gnd", 0.0, y_top0, cs.width, y_top1});

    const double target = std::min({t * mm, w * mm, s * mm, b * mm}) / 3.0;
    cs.nx = std::clamp((int)std::lround(cs.width / target), 120, 900);
    cs.ny = std::clamp((int)std::lround(cs.height / target), 80, 600);
    return cs;
}

}  // namespace faraday
