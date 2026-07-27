#!/usr/bin/env python3
"""Render the field-solver tier's mesh and solution.

    faraday_solve ... --mesh section.msh --field field.vtk
    python3 scripts/plot_section.py section.msh field.vtk out_prefix

Produces three PNGs: the tagged mesh, the potential with equipotentials, and
|E| on a log scale. Reads the same files the solver writes — nothing is
recomputed here, so the pictures are evidence of what actually ran.
"""
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection


def read_gmsh(path):
    """gmsh 2.2 -> (nodes Nx2, quads Mx4, tag per quad, name per tag)."""
    names, nodes, quads, tags = {}, {}, [], []
    with open(path) as f:
        lines = f.read().splitlines()
    i = 0
    while i < len(lines):
        s = lines[i].strip()
        if s == "$PhysicalNames":
            n = int(lines[i + 1])
            for k in range(n):
                parts = lines[i + 2 + k].split(' ', 2)
                names[int(parts[1])] = parts[2].strip().strip('"')
            i += 2 + n
        elif s == "$Nodes":
            n = int(lines[i + 1])
            for k in range(n):
                p = lines[i + 2 + k].split()
                nodes[int(p[0])] = (float(p[1]), float(p[2]))
            i += 2 + n
        elif s == "$Elements":
            n = int(lines[i + 1])
            for k in range(n):
                p = lines[i + 2 + k].split()
                if p[1] != '3':          # 3 = 4-node quad
                    continue
                ntags = int(p[2])
                tags.append(int(p[3]))
                quads.append([int(x) for x in p[3 + ntags:3 + ntags + 4]])
            i += 2 + n
        else:
            i += 1
    ids = sorted(nodes)
    remap = {nid: j for j, nid in enumerate(ids)}
    pts = np.array([nodes[nid] for nid in ids])
    qs = np.array([[remap[v] for v in q] for q in quads])
    return pts, qs, np.array(tags), names


def read_vtk(path):
    """Legacy VTK -> (points Nx2, cells Mx4, point scalar)."""
    with open(path) as f:
        lines = f.read().splitlines()
    pts, cells, vals = None, [], None
    i = 0
    while i < len(lines):
        s = lines[i].split()
        if s and s[0] == "POINTS":
            n = int(s[1])
            pts = np.array([[float(x) for x in lines[i + 1 + k].split()[:2]]
                            for k in range(n)])
            i += n + 1
        elif s and s[0] == "CELLS":
            n = int(s[1])
            for k in range(n):
                p = [int(x) for x in lines[i + 1 + k].split()]
                cells.append(p[1:1 + p[0]])
            i += n + 1
        elif s and s[0] == "SCALARS":
            n = len(pts)
            vals = np.array([float(lines[i + 2 + k]) for k in range(n)])
            i += n + 2
        else:
            i += 1
    return pts, cells, vals


def plot_mesh(msh, out):
    pts, quads, tags, names = read_gmsh(msh)
    mm = 1e3
    fig, ax = plt.subplots(figsize=(11, 5.2))
    palette = {}
    for t in sorted(set(tags)):
        nm = names.get(t, str(t))
        palette[t] = ("#d98b5f" if nm == "conductor_a" else
                      "#6f9fc4" if nm == "conductor_b" else
                      "#b9c4bf" if nm == "conductor_gnd" else "#1d2a22")
    verts = [pts[q] * mm for q in quads]
    pc = PolyCollection(verts, facecolors=[palette[t] for t in tags],
                        edgecolors="#3b4a41", linewidths=0.12)
    ax.add_collection(pc)
    ax.set_xlim(0, pts[:, 0].max() * mm)
    ax.set_ylim(0, pts[:, 1].max() * mm)
    ax.set_aspect("equal")
    ax.set_facecolor("#101613")
    ax.set_xlabel("x  [mm]"); ax.set_ylabel("y  [mm]")
    ax.set_title(f"FEM mesh — {len(quads)} quads, "
                 f"{len(set(tags))} tagged regions", color="#e6ede8")
    handles = [plt.Rectangle((0, 0), 1, 1, fc=palette[t],
                             label=names.get(t, str(t)))
               for t in sorted(set(tags))]
    ax.legend(handles=handles, loc="upper right", fontsize=8, framealpha=0.9)
    _style(fig, ax)
    fig.savefig(out, dpi=150, facecolor="#0b100d", bbox_inches="tight")
    print("wrote", out, f"({len(quads)} cells)")


def plot_field(vtk, out_v, out_e):
    pts, cells, V = read_vtk(vtk)
    mm = 1e3
    x, y = pts[:, 0] * mm, pts[:, 1] * mm
    tris = []
    for c in cells:                      # split quads into triangles
        if len(c) == 4:
            tris += [[c[0], c[1], c[2]], [c[0], c[2], c[3]]]
        elif len(c) == 3:
            tris.append(c)
    tris = np.array(tris)

    fig, ax = plt.subplots(figsize=(11, 5.2))
    tc = ax.tripcolor(x, y, tris, V, shading="gouraud", cmap="magma")
    ax.tricontour(x, y, tris, V, levels=np.linspace(0.05, 0.95, 19),
                  colors="#8fe3c4", linewidths=0.45, alpha=0.85)
    fig.colorbar(tc, ax=ax, label="potential V  [V]  (conductor_a at 1 V)")
    ax.set_aspect("equal")
    ax.set_xlabel("x  [mm]"); ax.set_ylabel("y  [mm]")
    ax.set_title("Electrostatic solution — V with equipotentials",
                 color="#e6ede8")
    _style(fig, ax)
    fig.savefig(out_v, dpi=150, facecolor="#0b100d", bbox_inches="tight")
    print("wrote", out_v)

    # |E| = |grad V|, evaluated per triangle from its linear gradient
    p = pts[tris]
    v = V[tris]
    x1, y1 = p[:, 0, 0], p[:, 0, 1]
    x2, y2 = p[:, 1, 0], p[:, 1, 1]
    x3, y3 = p[:, 2, 0], p[:, 2, 1]
    det = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
    det[np.abs(det) < 1e-300] = np.nan
    gx = ((y2 - y3) * (v[:, 0] - v[:, 2]) + (y3 - y1) * (v[:, 1] - v[:, 2])) / det
    gy = ((x3 - x2) * (v[:, 0] - v[:, 2]) + (x1 - x3) * (v[:, 1] - v[:, 2])) / det
    E = np.hypot(gx, gy)
    good = np.isfinite(E) & (E > 0)
    fig, ax = plt.subplots(figsize=(11, 5.2))
    tc = ax.tripcolor(x, y, tris[good], np.log10(E[good]), shading="flat",
                      cmap="inferno")
    fig.colorbar(tc, ax=ax, label="log10 |E|  [V/m per volt applied]")
    ax.set_aspect("equal")
    ax.set_xlabel("x  [mm]"); ax.set_ylabel("y  [mm]")
    ax.set_title("Field magnitude |E| = |grad V| — note the edge singularities",
                 color="#e6ede8")
    _style(fig, ax)
    fig.savefig(out_e, dpi=150, facecolor="#0b100d", bbox_inches="tight")
    print("wrote", out_e)


def _style(fig, ax):
    ax.tick_params(colors="#9db4ad")
    for sp in ax.spines.values():
        sp.set_color("#243028")
    ax.xaxis.label.set_color("#9db4ad")
    ax.yaxis.label.set_color("#9db4ad")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    msh, vtk, pre = sys.argv[1], sys.argv[2], sys.argv[3]
    plot_mesh(msh, pre + "_mesh.png")
    plot_field(vtk, pre + "_V.png", pre + "_E.png")
