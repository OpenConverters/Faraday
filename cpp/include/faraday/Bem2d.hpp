#pragma once
// 2D boundary-element (method-of-moments) capacitance extraction — the same
// job FastCap does in 3D, specialised to a PCB cross-section so it fits in a
// browser and finishes in milliseconds.
//
// WHY BEM AND NOT FEM. A finite-element solve needs a box around the
// structure, and the box is a lie: microstrip fields leave through the top.
// Every FEM answer therefore carries a truncation error you can only bound by
// re-running on a bigger box. A boundary-element method discretises only the
// CONDUCTOR SURFACES and the DIELECTRIC INTERFACES; the free-space Green's
// function already satisfies the radiation condition, so open boundaries are
// exact. The matrix is small and dense (~150 unknowns) instead of large and
// sparse (~50k), which is what makes an interactive slider possible.
//
// FORMULATION (Nabors & White, "FastCap", IEEE TCAD 1991, §II).
// The unknown is the TOTAL surface charge density sigma (free + bound) and the
// kernel is the free-space one. In 2D the potential of a line charge is
//
//     phi(p) = -(1 / 2 pi eps0) * integral over source of sigma * ln|p - r'|
//
// Two kinds of equation:
//   conductor panel i:  phi(centre_i) = V_i
//   interface panel i:  E_pv,n + (eps_a + eps_b) / (2 (eps_a - eps_b)) * sigma_i / eps0 = 0
//
// where eps_a is the relative permittivity on the +n side, eps_b on the -n
// side, and E_pv is the principal-value normal field (the self panel excluded,
// its +-sigma/2eps0 jump being what the second term encodes). Multiplying
// through by 2 pi eps0 gives the assembled rows below.
//
// THE REFERENCE PLANE IS EXACT, NOT MESHED. A perfect conductor at y = 0 is
// imposed by the method of images: every panel is accompanied by a mirrored
// panel of opposite charge, which makes phi identically zero on y = 0 for any
// arrangement above it. This is legitimate with mixed dielectrics because we
// are using the Dirichlet Green's function of the half-space and representing
// ALL material response as bound charge. The payoff is large: the plane costs
// no unknowns, extends to infinity with no truncation, and the interface bound
// charge then decays like a dipole (1/x^3) so the interface itself can be
// truncated close in.
//
// FREE VERSUS TOTAL CHARGE. sigma is the total charge; at a conductor face
// bathed in a medium of permittivity eps_r the free charge is eps_r * sigma
// (from D_n = sigma_free and E_n = sigma_total / eps0). Capacitance is defined
// on the FREE charge, so every conductor panel carries the permittivity of the
// medium it faces and the sum is weighted by it. Getting this wrong shows up
// immediately as sqrt(L*C0) != 1/c0, which is one of the tests.

#include "Dense.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace faraday::bem {

inline constexpr double EPS0 = 8.8541878128e-12;
inline constexpr double MU0 = 4.0e-7 * 3.14159265358979323846;
inline constexpr double C_LIGHT = 299792458.0;
inline constexpr double PI_ = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

struct Panel {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    double cx = 0, cy = 0;      // collocation point (midpoint)
    double tx = 0, ty = 0;      // unit tangent
    double nx = 0, ny = 0;      // unit normal, tangent rotated +90 deg
    double len = 0;
    int conductor = -1;         // >= 0: conductor id.  -1: dielectric interface
    double eps_out = 1.0;       // conductor panels: medium the face touches
    double eps_pos = 1.0;       // interface panels: rel. permittivity on +n side
    double eps_neg = 1.0;       // interface panels: rel. permittivity on -n side
};

inline Panel make_panel(double x1, double y1, double x2, double y2) {
    Panel p;
    p.x1 = x1; p.y1 = y1; p.x2 = x2; p.y2 = y2;
    const double dx = x2 - x1, dy = y2 - y1;
    p.len = std::sqrt(dx * dx + dy * dy);
    if (!(p.len > 0))
        throw std::invalid_argument("bem: degenerate panel of zero length");
    p.tx = dx / p.len; p.ty = dy / p.len;
    p.nx = -p.ty; p.ny = p.tx;              // rotate tangent +90 degrees
    p.cx = 0.5 * (x1 + x2); p.cy = 0.5 * (y1 + y2);
    return p;
}

// ---------------------------------------------------------------------------
// Analytic panel integrals (uniform charge density over a straight segment)
// ---------------------------------------------------------------------------
//
// With the source panel running from A along its tangent for a length `a`, and
// the field point expressed in panel-local coordinates (u along t, v along n):
//
//   Phi(u,v) = integral_0^a ln sqrt((s-u)^2 + v^2) ds
//            = 0.5 w2 ln r2^2 - 0.5 w1 ln r1^2 - a + v * dtheta
//
// with w1 = -u, w2 = a - u the endpoint offsets along the panel, r1 and r2 the
// distances to the two endpoints, and dtheta the angle the panel SUBTENDS at
// the field point. The gradient follows directly:
//
//   dPhi/du = ln r1 - ln r2
//   dPhi/dv = dtheta
//
// dtheta must be evaluated as ONE continuous quantity,
//
//   dtheta = atan2(v * a, v*v + w1*w2)
//
// and not as atan2(w2,v) - atan2(w1,v). Those two agree only while the field
// point stays off the branch cut: for v < 0 with the projection falling INSIDE
// the panel, w1 and w2 straddle zero, atan2 jumps from -pi to +pi, and the
// subtended angle comes out 2*pi wrong. Collinear panels always have v = 0 and
// w1*w2 > 0, so a mesh of coplanar strips never trips it — but the moment a
// closed conductor contributes perpendicular faces the potential is wrong, the
// charge density goes negative in places, and the extracted capacitance
// converges cleanly to the wrong number. It cost an afternoon; the identity
// sqrt(L*C0) = 1/c0 is what finally pinned it.

inline void panel_local(const Panel& q, double px, double py, double& u, double& v) {
    const double dx = px - q.x1, dy = py - q.y1;
    u = dx * q.tx + dy * q.ty;
    v = dx * q.nx + dy * q.ny;
}

// integral of ln|p - r'| over the panel. Exact, including the self term when
// (px,py) is the panel midpoint.
inline double panel_phi(const Panel& q, double px, double py) {
    double u, v;
    panel_local(q, px, py, u, v);
    const double w1 = -u, w2 = q.len - u;
    const double r1 = w1 * w1 + v * v, r2 = w2 * w2 + v * v;
    double t = -q.len;
    if (r2 > 0.0) t += 0.5 * w2 * std::log(r2);
    if (r1 > 0.0) t -= 0.5 * w1 * std::log(r1);
    t += v * std::atan2(v * q.len, v * v + w1 * w2);
    return t;
}

// Potential AND gradient in one pass, with a far-field shortcut. Used by the
// field plot, which evaluates every panel at every pixel and is the only part
// of the solver where the kernel cost actually shows up. Beyond a few panel
// lengths the panel is indistinguishable from a line charge at its midpoint,
// which drops the atan2 and one of the logs.
inline void panel_eval(const Panel& q, double px, double py, double& phi,
                       double& gx, double& gy) {
    const double dcx = px - q.cx, dcy = py - q.cy;
    const double d2 = dcx * dcx + dcy * dcy;
    if (d2 > 36.0 * q.len * q.len) {
        phi = 0.5 * q.len * std::log(d2);
        const double f = q.len / d2;
        gx = f * dcx;
        gy = f * dcy;
        return;
    }
    double u, v;
    panel_local(q, px, py, u, v);
    const double w1 = -u, w2 = q.len - u;
    const double r1 = w1 * w1 + v * v, r2 = w2 * w2 + v * v;
    const double l1 = r1 > 0.0 ? std::log(r1) : 0.0;
    const double l2 = r2 > 0.0 ? std::log(r2) : 0.0;
    const double dv = std::atan2(v * q.len, v * v + w1 * w2);
    phi = 0.5 * w2 * l2 - 0.5 * w1 * l1 - q.len + v * dv;
    const double du = 0.5 * (l1 - l2);
    gx = du * q.tx + dv * q.nx;
    gy = du * q.ty + dv * q.ny;
}

// gradient of the same integral, in GLOBAL coordinates
inline void panel_grad(const Panel& q, double px, double py,
                       double& gx, double& gy) {
    double u, v;
    panel_local(q, px, py, u, v);
    const double w1 = -u, w2 = q.len - u;
    const double r1 = w1 * w1 + v * v, r2 = w2 * w2 + v * v;
    // a field point sitting exactly on a panel endpoint is a logarithmic
    // singularity, not a number to invent
    const double du = (r1 > 0.0 && r2 > 0.0) ? 0.5 * std::log(r1 / r2) : 0.0;
    const double dv = std::atan2(v * q.len, v * v + w1 * w2);
    gx = du * q.tx + dv * q.nx;
    gy = du * q.ty + dv * q.ny;
}

// Mirror image of a panel in the plane y = 0. Endpoints are mirrored in the
// same order so the recomputed tangent/normal stay a consistent right-handed
// pair; the integrals above are orientation-independent for uniform density.
inline Panel mirrored(const Panel& p) {
    Panel m = make_panel(p.x1, -p.y1, p.x2, -p.y2);
    m.conductor = p.conductor;
    m.eps_out = p.eps_out;
    m.eps_pos = p.eps_pos;
    m.eps_neg = p.eps_neg;
    return m;
}

// ---------------------------------------------------------------------------
// Geometry description
// ---------------------------------------------------------------------------

struct Rect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // metres, y measured from the plane
    int conductor = 0;
};

// A conductor modelled with no thickness: one row of panels whose density is
// the charge on BOTH faces combined.
//
// This is only legitimate inside a HOMOGENEOUS medium. A zero-thickness sheet
// straddling a dielectric boundary has sigma_free = eps0(eps_a E_a + eps_b E_b)
// while the total charge the solver carries is sigma_tot = eps0(E_a + E_b), and
// the two are not proportional unless eps_a == eps_b — the free charge simply
// cannot be recovered from the unknown. discretise() enforces that.
struct Strip {
    double x0 = 0, x1 = 0, y = 0;
    int conductor = 0;
    // A reference plane is panelled the other way round from a trace: its
    // charge peaks UNDER the active conductors and dies away outward, so it
    // wants uniform panels across |x| < focus and geometrically coarsening
    // ones beyond. Chebyshev clustering, right for a trace edge, would put
    // every panel where there is no charge.
    bool plane = false;
    double focus = 0;
};

// A horizontally layered dielectric stack above the reference plane. `top_y`
// is the upper boundary of each slab; the last slab extends to infinity, which
// on a real board is air.
struct Slab {
    double top_y = 0;
    double eps_r = 1;
};

struct Geometry {
    std::vector<Rect> conductors;   // conductor ids 0..n_signal-1 are signals;
                                    // id == n_signal is the (optional) upper
                                    // reference plane, held at 0 V
    std::vector<Strip> strips;      // zero-thickness conductors (homogeneous media only)
    std::vector<Slab> slabs;        // sorted by top_y, last one is air above
    double eps_top = 1.0;           // permittivity above the last slab
    int n_signal = 0;
    int panels_per_face = 10;       // floor on conductor face discretisation
    int max_panels_per_face = 56;   // ceiling — beyond this, refuse rather than lie
    int interface_panels = 16;      // per free span of a dielectric interface
    // Interface half-width in units of the structure height. With the plane
    // imaged, the interface bound charge is a dipole distribution falling off
    // like 1/x^3, so this converges fast: measured, Z0 and kb move by under
    // 0.02% between 10 and 80, which is why the default sits just above 10.
    double truncate_factor = 12.0;
};

// Chebyshev-clustered break points on [a, b]: dense at BOTH ends, because the
// surface charge density on a conductor has an inverse-square-root singularity
// at every edge and a uniform mesh spends its unknowns in the flat middle.
inline void split_cheb(double a, double b, int m, std::vector<double>& out) {
    out.clear();
    if (m < 1) throw std::invalid_argument("bem: need at least one panel");
    for (int k = 0; k <= m; ++k) {
        const double s = 0.5 * (1.0 - std::cos(PI_ * k / m));
        out.push_back(a + (b - a) * s);
    }
}

// Geometrically graded break points from `a` (fine) to `b` (coarse) — for the
// outward tails of a dielectric interface, where the charge dies off and
// uniform panels would be a waste of unknowns.
inline void split_geom(double a, double b, int m, std::vector<double>& out) {
    out.clear();
    if (m < 1) throw std::invalid_argument("bem: need at least one panel");
    const double r = 1.35;
    double tot = 0;
    for (int k = 0; k < m; ++k) tot += std::pow(r, k);
    double s = 0;
    out.push_back(a);
    for (int k = 0; k < m; ++k) {
        s += std::pow(r, k) / tot;
        out.push_back(a + (b - a) * s);
    }
}

// Break points that start at length `e0` at BOTH ends and grow geometrically
// toward the middle, capped at `lmax`.
//
// This replaces Chebyshev clustering on the faces of a closed conductor, and
// the reason is a corner. Chebyshev sizes each face independently, so a 1 mm
// face split 48 ways puts a 2 um panel against the 59 um panel that starts the
// 400 um face it meets at the corner. The tiny panel's own self-term is then
// twenty times SMALLER than its coupling to that neighbour: the discrete
// operator loses diagonal dominance and the extracted capacitance diverges as
// the mesh is refined — refinement makes it worse, which is the signature to
// watch for. Driving both faces from a shared `e0` keeps the panels either
// side of every corner the same size.
inline void split_sym(double a, double b, double e0, double lmax,
                      std::vector<double>& out) {
    const double L = b - a;
    if (!(L > 0)) throw std::invalid_argument("bem: split_sym needs b > a");
    if (!(e0 > 0) || !(lmax >= e0))
        throw std::invalid_argument("bem: split_sym needs 0 < e0 <= lmax");
    const double ratio = 1.35;
    const size_t cap = 400;
    std::vector<double> half;
    double s = 0, l = std::min(e0, 0.5 * L);
    while (s < 0.5 * L && half.size() < cap) {
        const double take = std::min(l, lmax);
        half.push_back(take);
        s += take;
        l *= ratio;
    }
    // Running out of panels before covering the face is not a reason to
    // stretch the ones we have — that would quietly hand back panels larger
    // than lmax, which is precisely the under-resolution lmax exists to
    // prevent, and the wrong answer would look perfectly well-formed.
    if (s < 0.5 * L)
        throw std::invalid_argument(
            "bem: a face of " + std::to_string(L * 1e3) + " mm cannot be resolved "
            "with panels capped at " + std::to_string(lmax * 1e3) + " mm — the "
            "conductor's aspect ratio is too extreme to panel as a closed "
            "contour. Model it as a zero-thickness strip, which is valid in a "
            "homogeneous medium, or give it a realistic thickness.");
    std::vector<double> lens = half;
    for (size_t i = half.size(); i-- > 0;) lens.push_back(half[i]);
    double tot = 0;
    for (double x : lens) tot += x;
    out.clear();
    out.push_back(a);
    double acc = 0;
    for (double x : lens) { acc += x * L / tot; out.push_back(a + acc); }
    out.back() = b;
}

// Break points starting at length `e0` at `a` and growing geometrically toward
// `b`, which may lie either side of `a`. Used for the outward tails of a
// dielectric interface and of a reference plane.
inline void split_from(double a, double b, double e0, double lmax,
                       std::vector<double>& out) {
    const double L = std::abs(b - a);
    const double dir = (b > a) ? 1.0 : -1.0;
    if (!(L > 0)) throw std::invalid_argument("bem: split_from needs a != b");
    if (!(e0 > 0)) throw std::invalid_argument("bem: split_from needs e0 > 0");
    const double ratio = 1.45;
    std::vector<double> lens;
    double s = 0, l = std::min(e0, L);
    while (s < L && lens.size() < 512) {
        const double take = std::min(l, std::max(lmax, e0));
        lens.push_back(take);
        s += take;
        l *= ratio;
    }
    double tot = 0;
    for (double x : lens) tot += x;
    out.clear();
    out.push_back(a);
    double acc = 0;
    for (double x : lens) { acc += x * L / tot; out.push_back(a + dir * acc); }
    out.back() = b;
}

// Emit panels along a straight run given precomputed break points.
inline void emit_run(std::vector<Panel>& out, const std::vector<double>& br,
                     bool horizontal, double fixed, int conductor,
                     double eps_out) {
    for (size_t k = 0; k + 1 < br.size(); ++k) {
        Panel p = horizontal ? make_panel(br[k], fixed, br[k + 1], fixed)
                             : make_panel(fixed, br[k], fixed, br[k + 1]);
        p.conductor = conductor;
        p.eps_out = eps_out;
        out.push_back(p);
    }
}

inline void add_face(std::vector<Panel>& out, double x1, double y1, double x2,
                     double y2, int m, int conductor, double eps_out) {
    std::vector<double> s;
    split_cheb(0.0, 1.0, m, s);
    for (int k = 0; k < m; ++k) {
        Panel p = make_panel(x1 + (x2 - x1) * s[k], y1 + (y2 - y1) * s[k],
                             x1 + (x2 - x1) * s[k + 1], y1 + (y2 - y1) * s[k + 1]);
        p.conductor = conductor;
        p.eps_out = eps_out;
        out.push_back(p);
    }
}

// Permittivity of the slab containing height y.
inline double eps_at(const Geometry& g, double y) {
    for (const auto& s : g.slabs)
        if (y < s.top_y) return s.eps_r;
    return g.eps_top;
}

// The one length scale the whole mesh is built from: panels this size sit at
// every conductor corner, on both of the faces that meet there, and grow away
// from it. Taken as a fraction of the smallest conductor dimension present.
inline double fine_size(const Geometry& g) {
    double smallest = 1e30;
    for (const auto& r : g.conductors)
        smallest = std::min(smallest, std::min(r.x1 - r.x0, r.y1 - r.y0));
    for (const auto& s : g.strips)
        smallest = std::min(smallest, s.x1 - s.x0);
    if (!(smallest > 0) || smallest > 1e29)
        throw std::invalid_argument("bem: no conductor with a positive dimension");
    return smallest / 6.0;
}

// Build the panel list. `vacuum` drops every dielectric interface and treats
// all media as free space — the second solve that yields C0, hence L.
inline std::vector<Panel> discretise(const Geometry& g, bool vacuum) {
    std::vector<Panel> out;
    const double e0 = fine_size(g);
    std::vector<double> br;

    for (const auto& r : g.conductors) {
        if (r.x1 <= r.x0 || r.y1 <= r.y0)
            throw std::invalid_argument("bem: conductor rectangle is inverted or empty");
        // permittivity each face looks into; a face lying exactly on an
        // interface sees the medium on its own outward side
        const double w = r.x1 - r.x0, t = r.y1 - r.y0;
        const double mid_y = 0.5 * (r.y0 + r.y1);
        const double e_bot = vacuum ? 1.0 : eps_at(g, r.y0 - 1e-12);
        const double e_top = vacuum ? 1.0 : eps_at(g, r.y1 + 1e-12);
        const double e_side = vacuum ? 1.0 : eps_at(g, mid_y);
        // the widest panel a face may use: the opposite face must stay
        // distinguishable from it, so cap at the conductor's own thickness
        const double lmax = std::max(e0, std::min(w, t));
        split_sym(r.x0, r.x1, e0, lmax, br);
        emit_run(out, br, true, r.y0, r.conductor, e_bot);
        emit_run(out, br, true, r.y1, r.conductor, e_top);
        split_sym(r.y0, r.y1, e0, lmax, br);
        emit_run(out, br, false, r.x0, r.conductor, e_side);
        emit_run(out, br, false, r.x1, r.conductor, e_side);
    }

    for (const auto& s : g.strips) {
        if (s.x1 <= s.x0) throw std::invalid_argument("bem: strip is inverted or empty");
        const double above = vacuum ? 1.0 : eps_at(g, s.y + 1e-12);
        const double below = vacuum ? 1.0 : eps_at(g, s.y - 1e-12);
        if (std::abs(above - below) > 1e-12)
            throw std::invalid_argument(
                "bem: a zero-thickness strip sits on a dielectric boundary, where "
                "its free charge is not recoverable from the total charge. Give it "
                "a thickness.");
        if (!s.plane) {
            // an isolated open strip HAS the inverse-square-root edge
            // singularity at both ends and no corner to match, so Chebyshev
            // clustering is exactly right here
            add_face(out, s.x0, s.y, s.x1, s.y,
                     std::clamp(g.panels_per_face * 2, 8, g.max_panels_per_face),
                     s.conductor, above);
            continue;
        }
        if (!(s.focus > 0) || s.focus >= 0.5 * (s.x1 - s.x0))
            throw std::invalid_argument("bem: reference plane needs 0 < focus < half-width");
        const int m = std::clamp(g.panels_per_face * 2, 8, g.max_panels_per_face);
        const double cell = 2.0 * s.focus / m;
        split_from(-s.focus, s.x0, cell, 8.0 * cell, br);   // left tail
        emit_run(out, br, true, s.y, s.conductor, above);
        br.clear();
        for (int k = 0; k <= m; ++k) br.push_back(-s.focus + cell * k);
        emit_run(out, br, true, s.y, s.conductor, above);   // uniform, under the traces
        split_from(s.focus, s.x1, cell, 8.0 * cell, br);    // right tail
        emit_run(out, br, true, s.y, s.conductor, above);
    }
    if (vacuum) return out;

    // Dielectric interfaces: one horizontal line per slab boundary where the
    // permittivity actually changes, spanning the truncation window minus the
    // footprints of any conductor sitting on that line.
    double xlo = 1e30, xhi = -1e30, hmax = 0;
    for (const auto& r : g.conductors) {
        xlo = std::min(xlo, r.x0);
        xhi = std::max(xhi, r.x1);
        hmax = std::max(hmax, r.y1);
    }
    for (const auto& s : g.strips) {
        xlo = std::min(xlo, s.x0);
        xhi = std::max(xhi, s.x1);
        hmax = std::max(hmax, s.y);
    }
    if (g.conductors.empty() && g.strips.empty())
        throw std::invalid_argument("bem: no conductors");
    const double reach = g.truncate_factor * std::max(hmax, xhi - xlo);
    const double xL = xlo - reach, xR = xhi + reach;

    for (size_t i = 0; i < g.slabs.size(); ++i) {
        const double y = g.slabs[i].top_y;
        const double e_below = g.slabs[i].eps_r;
        const double e_above = (i + 1 < g.slabs.size()) ? g.slabs[i + 1].eps_r : g.eps_top;
        if (std::abs(e_above - e_below) < 1e-12) continue;

        // occupied x-intervals: conductors whose body straddles this height
        std::vector<std::pair<double, double>> occ;
        for (const auto& r : g.conductors)
            if (r.y0 <= y + 1e-12 && r.y1 >= y - 1e-12) occ.push_back({r.x0, r.x1});
        std::sort(occ.begin(), occ.end());

        std::vector<std::pair<double, double>> free_spans;
        double cur = xL;
        for (const auto& [a, b] : occ) {
            if (a > cur) free_spans.push_back({cur, a});
            cur = std::max(cur, b);
        }
        if (cur < xR) free_spans.push_back({cur, xR});

        // Interface panels meet the trace footprints end-on, so they too are
        // driven from the shared corner size: a coarse interface panel butted
        // against a fine trace-bottom panel reintroduces exactly the imbalance
        // split_sym() exists to avoid.
        const double lmax = std::max(e0, 3.0 * hmax);
        for (size_t s = 0; s < free_spans.size(); ++s) {
            const auto [a, b] = free_spans[s];
            if (b - a <= 0) continue;
            const bool left_tail = (s == 0);
            const bool right_tail = (s + 1 == free_spans.size());
            if (left_tail && right_tail && occ.empty()) split_sym(a, b, e0, lmax, br);
            else if (left_tail) split_from(b, a, e0, lmax, br);   // fine at the conductor
            else if (right_tail) split_from(a, b, e0, lmax, br);
            else split_sym(a, b, e0, lmax, br);                   // gap between traces
            for (size_t k = 0; k + 1 < br.size(); ++k) {
                Panel p = make_panel(std::min(br[k], br[k + 1]), y,
                                     std::max(br[k], br[k + 1]), y);
                p.conductor = -1;
                // tangent is +x so the normal is +y: "pos" side is above
                p.eps_pos = e_above;
                p.eps_neg = e_below;
                out.push_back(p);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Solve
// ---------------------------------------------------------------------------

struct Solution {
    std::vector<Panel> panels;
    // charge densities, one vector per signal conductor excited to 1 V
    std::vector<std::vector<double>> sigma;
    std::vector<double> cmat;   // n_signal x n_signal, F/m
    int n_signal = 0;
    double at(size_t i, size_t j) const { return cmat[i * (size_t)n_signal + j]; }
};

// Assemble and factor the influence matrix. `rowmax` comes back out because
// the same row equilibration has to be applied to every right-hand side.
struct Assembled {
    Lu lu;
    std::vector<double> rowmax;
};

inline Assembled assemble(const std::vector<Panel>& p) {
    const size_t n = p.size();
    if (n == 0) throw std::invalid_argument("bem: empty panel set");
    std::vector<double> a(n * n, 0.0);
    std::vector<Panel> img(n);
    for (size_t j = 0; j < n; ++j) img[j] = mirrored(p[j]);

    for (size_t i = 0; i < n; ++i) {
        const bool cond = p[i].conductor >= 0;
        for (size_t j = 0; j < n; ++j) {
            if (cond) {
                // phi = -(1/2 pi eps0) sum sigma_j Phi_j;  row scaled by -2 pi eps0
                const double self =
                    (i == j) ? p[j].len * (std::log(0.5 * p[j].len) - 1.0)
                             : panel_phi(p[j], p[i].cx, p[i].cy);
                a[i * n + j] = -(self - panel_phi(img[j], p[i].cx, p[i].cy));
            } else {
                double gx = 0, gy = 0, ix = 0, iy = 0;
                if (i != j) panel_grad(p[j], p[i].cx, p[i].cy, gx, gy);
                panel_grad(img[j], p[i].cx, p[i].cy, ix, iy);
                a[i * n + j] = (gx - ix) * p[i].nx + (gy - iy) * p[i].ny;
                if (i == j) {
                    const double ea = p[i].eps_pos, eb = p[i].eps_neg;
                    if (std::abs(ea - eb) < 1e-12)
                        throw std::invalid_argument(
                            "bem: dielectric panel with equal permittivity on both "
                            "sides — it should not have been generated");
                    a[i * n + j] += PI_ * (ea + eb) / (ea - eb);
                }
            }
        }
    }
    // Row equilibration. Conductor rows carry lengths (~1e-4) and interface
    // rows carry dimensionless kernels (~1); mixing scales four orders apart
    // costs digits in the pivoted factorisation.
    std::vector<double> rowmax(n, 1.0);
    for (size_t i = 0; i < n; ++i) {
        double m = 0;
        for (size_t j = 0; j < n; ++j) m = std::max(m, std::abs(a[i * n + j]));
        if (!(m > 0))
            throw std::invalid_argument(
                "bem: an all-zero row in the influence matrix — a panel sees no "
                "other panel, which means the geometry is disconnected");
        rowmax[i] = m;
        for (size_t j = 0; j < n; ++j) a[i * n + j] /= m;
    }
    return Assembled{Lu(std::move(a), n), std::move(rowmax)};
}

// One solve per signal conductor: unit potential on that conductor, zero on
// every other conductor including the reference. The resulting free charges
// ARE the columns of the transmission-line capacitance matrix (Paul,
// Multiconductor Transmission Lines, ch. 5) — the reference row and column of
// the Maxwell matrix are exactly what a TL model discards, so they are never
// formed.
inline Solution solve(const Geometry& g, bool vacuum) {
    if (g.n_signal < 1) throw std::invalid_argument("bem: no signal conductors");
    Solution out;
    out.panels = discretise(g, vacuum);
    out.n_signal = g.n_signal;
    const size_t n = out.panels.size();
    Assembled A = assemble(out.panels);

    out.cmat.assign((size_t)g.n_signal * g.n_signal, 0.0);
    for (int k = 0; k < g.n_signal; ++k) {
        std::vector<double> rhs(n, 0.0);
        for (size_t i = 0; i < n; ++i) {
            if (out.panels[i].conductor == k)
                rhs[i] = 2.0 * PI_ * EPS0 / A.rowmax[i];
        }
        A.lu.solve(rhs);
        out.sigma.push_back(rhs);
        // free charge per unit length on each signal conductor
        for (int c = 0; c < g.n_signal; ++c) {
            double q = 0;
            for (size_t i = 0; i < n; ++i)
                if (out.panels[i].conductor == c)
                    q += out.panels[i].eps_out * rhs[i] * out.panels[i].len;
            out.cmat[(size_t)c * g.n_signal + k] = q;
        }
    }
    // symmetrise: reciprocity holds exactly in the continuous problem, so the
    // asymmetry left over is pure discretisation error and averaging halves it
    for (int i = 0; i < g.n_signal; ++i)
        for (int j = i + 1; j < g.n_signal; ++j) {
            const size_t a = (size_t)i * g.n_signal + j, b = (size_t)j * g.n_signal + i;
            const double s = 0.5 * (out.cmat[a] + out.cmat[b]);
            out.cmat[a] = out.cmat[b] = s;
        }
    return out;
}

// ---------------------------------------------------------------------------
// Field sampling — the picture
// ---------------------------------------------------------------------------
//
// Once the panel charges are known the potential and field are available
// ANYWHERE in closed form, with no mesh and no interpolation: sum the analytic
// panel integrals. That is the second advantage of BEM over FEM here — the
// plot is the solution, not a post-processed projection of it.

struct FieldMap {
    int nx = 0, ny = 0;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // window in metres
    std::vector<float> v;       // potential, volts (aggressor at 1 V)
    std::vector<float> e;       // |E|, V/m
    double e_max = 0;
};

inline FieldMap sample_field(const Solution& s, size_t excite, double x0,
                             double y0, double x1, double y1, int nx, int ny) {
    if (excite >= s.sigma.size())
        throw std::invalid_argument("bem: field sample for a conductor that was not solved");
    if (nx < 2 || ny < 2) throw std::invalid_argument("bem: field grid too small");
    FieldMap f;
    f.nx = nx; f.ny = ny; f.x0 = x0; f.y0 = y0; f.x1 = x1; f.y1 = y1;
    f.v.assign((size_t)nx * ny, 0.0f);
    f.e.assign((size_t)nx * ny, 0.0f);
    const auto& sig = s.sigma[excite];
    const size_t n = s.panels.size();
    std::vector<Panel> img(n);
    for (size_t j = 0; j < n; ++j) img[j] = mirrored(s.panels[j]);
    const double kq = 1.0 / (2.0 * PI_ * EPS0);

    for (int iy = 0; iy < ny; ++iy) {
        const double py = y0 + (y1 - y0) * iy / (ny - 1);
        for (int ix = 0; ix < nx; ++ix) {
            const double px = x0 + (x1 - x0) * ix / (nx - 1);
            double phi = 0, ex = 0, ey = 0;
            for (size_t j = 0; j < n; ++j) {
                if (sig[j] == 0.0) continue;
                double p1, g1x, g1y, p2, g2x, g2y;
                panel_eval(s.panels[j], px, py, p1, g1x, g1y);
                panel_eval(img[j], px, py, p2, g2x, g2y);
                const double w = kq * sig[j];
                phi -= w * (p1 - p2);
                ex += w * (g1x - g2x);
                ey += w * (g1y - g2y);
            }
            const size_t o = (size_t)iy * nx + ix;
            f.v[o] = (float)phi;
            const double em = std::sqrt(ex * ex + ey * ey);
            f.e[o] = (float)em;
            if (std::isfinite(em)) f.e_max = std::max(f.e_max, em);
        }
    }
    return f;
}

// ---------------------------------------------------------------------------
// Convenience: the two-line cross-sections Faraday's rules actually produce
// ---------------------------------------------------------------------------

struct PairSection {
    double w1 = 0.2e-3, w2 = 0.2e-3;   // trace widths, m
    // Three coplanar conductors: aggressor - victim - aggressor. The victim in
    // the middle with an attacker either side is THE classic three-conductor
    // case, and the BEM solver has been N-conductor all along — only this
    // builder was a pair.
    bool triple = false;
    double w3 = 0.2e-3;
    double gap2 = 0.2e-3;              // victim-to-second-aggressor gap
    double t = 35e-6;                  // copper thickness, m
    double gap = 0.2e-3;               // edge-to-edge gap (edge coupling), m
    double h = 0.2e-3;                 // height above the reference plane, m
    double eps_r = 4.3;
    bool broadside = false;            // trace 2 on the layer above
    double h_v = 0.2e-3;               // vertical separation for broadside, m
    double lateral = 0.0;              // centreline offset for broadside, m
    bool stripline = false;            // a second reference plane above
    double b = 0.6e-3;                 // plane-to-plane spacing, m
};

inline Geometry geometry_for(const PairSection& s) {
    if (s.w1 <= 0 || s.w2 <= 0 || s.t <= 0 || s.eps_r < 1)
        throw std::invalid_argument("bem: cross-section has a non-physical dimension");
    Geometry g;
    g.n_signal = 2;

    if (s.triple) {
        if (s.broadside || s.stripline)
            throw std::invalid_argument(
                "bem: the three-conductor section is microstrip-only for now — "
                "broadside and stripline triples are not implemented, and "
                "refusing beats guessing");
        if (s.h <= 0) throw std::invalid_argument("bem: triple needs h > 0");
        g.n_signal = 3;
        // conductor 0: left aggressor, 1: middle victim, 2: right aggressor
        double x = -(s.w1 + s.gap + s.w2 / 2);
        g.conductors.push_back({x, s.h, x + s.w1, s.h + s.t, 0});
        x = -s.w2 / 2;
        g.conductors.push_back({x, s.h, x + s.w2, s.h + s.t, 1});
        x = s.w2 / 2 + s.gap2;
        g.conductors.push_back({x, s.h, x + s.w3, s.h + s.t, 2});
        g.slabs.push_back({s.h, s.eps_r});
        g.eps_top = 1.0;
        return g;
    }

    if (s.broadside) {
        if (s.h <= 0 || s.h_v <= 0)
            throw std::invalid_argument("bem: broadside section needs h > 0 and h_v > 0");
        // trace 1 on the lower layer, trace 2 h_v above it, offset laterally
        const double x1 = -0.5 * s.w1;
        g.conductors.push_back({x1, s.h, x1 + s.w1, s.h + s.t, 0});
        const double x2 = s.lateral - 0.5 * s.w2;
        g.conductors.push_back({x2, s.h + s.h_v, x2 + s.w2, s.h + s.h_v + s.t, 1});
        // both layers are inner: laminate all the way past the upper trace
        g.slabs.push_back({s.h + s.h_v + s.t + 4.0 * s.h, s.eps_r});
        g.eps_top = 1.0;
        return g;
    }

    if (s.stripline) {
        if (s.b <= s.t * 2) throw std::invalid_argument("bem: stripline b too small");
        // Homogeneous medium, so the traces may be zero-thickness strips —
        // which is also exactly the geometry Cohn's conformal map solves, and
        // therefore the configuration in which the two can be compared without
        // a thickness correction standing between them.
        const double thin = s.t < 0.25 * std::min(s.w1, s.w2);
        const double y = 0.5 * (s.b - (thin ? 0.0 : s.t));
        const double x1 = -(0.5 * s.gap + s.w1);
        const double x2 = 0.5 * s.gap;
        if (thin) {
            g.strips.push_back({x1, x1 + s.w1, y, 0});
            g.strips.push_back({x2, x2 + s.w2, y, 1});
        } else {
            g.conductors.push_back({x1, y, x1 + s.w1, y + s.t, 0});
            g.conductors.push_back({x2, y, x2 + s.w2, y + s.t, 1});
        }
        // The upper plane is a real conductor held at 0 V, meshed rather than
        // imaged: a second image plane would need an infinite series, whereas
        // the stripline field decays like exp(-pi x / b), so a finite plane a
        // few b wide is exact far below the discretisation error. It is thin
        // and homogeneous, so it too is a strip.
        const double reach = 10.0 * s.b + s.w1 + s.w2 + s.gap;
        const double focus = 2.0 * s.b + s.w1 + s.w2 + s.gap;
        g.strips.push_back({-reach, reach, s.b, 2, true, focus});
        g.n_signal = 2;                    // conductor 2 is the reference
        g.slabs.clear();                   // homogeneous: no interface at all
        g.eps_top = s.eps_r;
        g.panels_per_face = 20;
        return g;
    }

    // microstrip pair over the imaged plane at y = 0
    if (s.h <= 0) throw std::invalid_argument("bem: microstrip section needs h > 0");
    const double x1 = -(0.5 * s.gap + s.w1);
    g.conductors.push_back({x1, s.h, x1 + s.w1, s.h + s.t, 0});
    const double x2 = 0.5 * s.gap;
    g.conductors.push_back({x2, s.h, x2 + s.w2, s.h + s.t, 1});
    g.slabs.push_back({s.h, s.eps_r});     // laminate below the traces, air above
    g.eps_top = 1.0;
    return g;
}

}  // namespace faraday::bem
