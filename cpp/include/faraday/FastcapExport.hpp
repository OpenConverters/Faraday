#pragma once
// Emit a CrossSection as a FasterCap/FastCap 2D input deck, so an INDEPENDENT
// solver can be handed exactly the same geometry.
//
// Why this exists: the self terms of the extraction (Z0, eps_eff) are checked
// against Hammerstad and against the exact sqrt(L*C0)=1/c0 identity, but the
// MUTUAL terms — the coupling, which is the entire point of the tool — had
// nothing behind them except a SPICE deck built from those same matrices. That
// is not a check. FasterCap is a different method (boundary elements, not
// finite elements) and a different codebase, so agreement means something.
//
// FastCap's 2D format describes conductor and dielectric BOUNDARIES as
// segments, not filled regions:
//   C <file> <outperm> <xoffset> <yoffset>              (conductor surface)
//   D <file> <outperm> <inperm> <xoff> <yoff> <xref> <yref>   (dielectric i/f)
// and each referenced file holds lines of
//   S <name> <x1> <y1> <x2> <y2>
// The reference point on a D line marks which side carries `inperm`.

#include "CrossSection.hpp"

#include <fstream>
#include <string>
#include <vector>

namespace faraday {

// Writes <prefix>.lst plus one .txt per surface. Returns the .lst path, which
// is what FasterCap is invoked on.
inline std::string write_fastcap_2d(const CrossSection& cs,
                                    const std::string& prefix) {
    // Permittivity just above and below the trace layer. FastCap wants the
    // medium either side of every boundary, and a microstrip section has the
    // laminate below the traces and air above.
    const double y_trace = cs.conductors.size() > 1 ? cs.conductors[1].y0 : 0.0;
    const double eps_below = cs.eps_at(std::max(0.0, y_trace * 0.5));
    const double eps_above = cs.eps_at(cs.height * 0.99);

    std::vector<std::string> files;
    auto write_rect = [&](const std::string& name, double x0, double y0,
                          double x1, double y1) {
        const std::string path = prefix + "_" + name + ".txt";
        std::ofstream f(path);
        if (!f) throw BoardError("fastcap export: cannot write " + path);
        f << "0 " << name << " boundary\n";
        // four sides, counter-clockwise
        f << "S " << name << " " << x0 << " " << y0 << " " << x1 << " " << y0 << "\n";
        f << "S " << name << " " << x1 << " " << y0 << " " << x1 << " " << y1 << "\n";
        f << "S " << name << " " << x1 << " " << y1 << " " << x0 << " " << y1 << "\n";
        f << "S " << name << " " << x0 << " " << y1 << " " << x0 << " " << y0 << "\n";
        files.push_back(path);
        return path;
    };

    const std::string lst = prefix + ".lst";
    std::ofstream L(lst);
    if (!L) throw BoardError("fastcap export: cannot write " + lst);
    L << "* Faraday cross-section exported for FasterCap — same geometry the\n"
         "* finite-element solver sees, so the two can be compared directly.\n";

    for (const auto& c : cs.conductors) {
        // the medium surrounding each conductor: traces sit under air on top
        // and laminate below; the plane is fully embedded in laminate
        const bool is_plane = c.name.find("gnd") != std::string::npos;
        const double outperm = is_plane ? eps_below : eps_below;
        const std::string p = write_rect(c.name, c.x0, c.y0, c.x1, c.y1);
        L << "C " << p << " " << outperm << " 0 0\n";
    }

    // dielectric interface at the top of the laminate: laminate below, air above
    if (std::abs(eps_above - eps_below) > 1e-9) {
        const std::string path = prefix + "_interface.txt";
        std::ofstream f(path);
        f << "0 dielectric interface\n";
        f << "S iface 0 " << y_trace << " " << cs.width << " " << y_trace << "\n";
        // reference point BELOW the interface marks the `inperm` side
        L << "D " << path << " " << eps_above << " " << eps_below << " 0 0 "
          << (cs.width * 0.5) << " " << (y_trace * 0.5) << "\n";
    }
    return lst;
}

}  // namespace faraday
