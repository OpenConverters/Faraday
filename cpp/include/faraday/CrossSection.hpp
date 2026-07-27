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
        std::vector<int> tag((size_t)nx * ny);
        const double dx = width / nx, dy = height / ny;
        std::vector<int> cells_per_conductor(conductors.size(), 0);
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const std::string r = region_at((i + 0.5) * dx, (j + 0.5) * dy);
                tag[(size_t)j * nx + i] = region_id(emit(r));
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
        const int nvx = nx + 1, nvy = ny + 1;
        os << "$Nodes\n" << (nvx * nvy) << "\n";
        for (int j = 0; j < nvy; ++j)
            for (int i = 0; i < nvx; ++i)
                os << (j * nvx + i + 1) << " " << (i * dx) << " " << (j * dy) << " 0\n";
        os << "$EndNodes\n";
        os << "$Elements\n" << (nx * ny) << "\n";
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const int e = j * nx + i, v0 = j * nvx + i + 1;
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

}  // namespace faraday
