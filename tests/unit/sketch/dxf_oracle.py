#!/usr/bin/env python3
"""Independent expected values for the DXF corpus test (task T-091), computed with ezdxf (MIT licence).

ezdxf is NOT a RayCAD dependency: this script only produced the numbers in dxf_corpus_expect.h, so the C++ reader is
checked against a second, unrelated DXF implementation. To regenerate (ezdxf 1.4.2 was used):

    python -m venv venv && venv/bin/pip install ezdxf==1.4.2
    venv/bin/python -I tests/unit/sketch/dxf_oracle.py tests/corpus > tests/unit/sketch/dxf_corpus_expect.h

For every DXF file listed below it reads the model space, explodes block references (ezdxf's recursive_decompose)
and polylines (virtual_entities, which turns bulges into ARC entities), and sums, in millimetres:
  - the length of every LINE, ARC, CIRCLE, circular ELLIPSE and non-rational SPLINE (projected onto XY),
  - the length of the arcs and circles alone, and their number,
  - the bounding box in XY,
  - the number of POINT entities.
Entities the RayCAD reader leaves out by design are left out here too: ellipses that are not circles, entities whose
extrusion is not +Z or -Z, polyface and polygon meshes, zero-length lines, circles of radius zero or less, and splines
without control points. Rational splines are summed separately (the reader keeps only those made of circular arcs).
Units follow the rule in src/sketch/dxf.h: $INSUNITS when it names a unit, else $MEASUREMENT, else millimetres
assumed. The header values are read from the raw group codes, not through ezdxf, so its defaults do not leak in.
"""
import math
import json
import os
import sys

import ezdxf
from ezdxf import disassemble

FILES = [
    # corpus ids; each path is taken from tests/corpus/manifest.json (exact case)
    "build123d/accumulatortest",
    "build123d/blocks1",
    "build123d/blocks2",
    "build123d/circlesellipsesarcs",
    "build123d/closedlwpolylinebug",
    "build123d/cube",
    "build123d/diamond",
    "build123d/layers",
    "build123d/lines",
    "build123d/lwpolylines",
    "build123d/output",
    "build123d/points",
    "build123d/polylines",
    "build123d/rectangle",
    "build123d/shaft_simple",
    "build123d/splinea",
    "build123d/splines",
    "build123d/squareandcircle",
    "build123d/test-angled-section",
    "build123d/test-sketch",
    "build123d/bridge",
    "cadquery/gear",
    "cadquery/genshi",
    "cadquery/rational_spline",
    "cadquery/spline",
    "cadquery/three_layers",
    "freecad/issue24314",
    "freecad/rectangle_lines",
    "freecad/square",
    "freecad/dxfsample",
    "mayo/lwpolyline_closed_duplicate_last_vertex",
    "ezdxf/cc_dxflib",
    "ezdxf/colors",
    "ezdxf/insert_bricscad_level_1",
    "ezdxf/minimal_dxf_ac1021",
    "ezdxf/no_layouts",
    "ezdxf/small_r13",
    "ezdxf/ascii_r12",
    "ezdxf/multi_insert_with_attribs",
    "ezdxf/circle_radius_le_0",
    "ezdxf/clockwise_arcs_hatch",
    "raycad-synthetic/dxf_flange_gasket_mm",
    "raycad-synthetic/dxf_gap_and_construction_inch",
    "raycad-synthetic/dxf_hex_profile_mm",
    "raycad-synthetic/dxf_keyhole_mm",
    "raycad-synthetic/dxf_l_bracket_profile_mm",
    "raycad-synthetic/dxf_open_contour_mm",
    "raycad-synthetic/dxf_plate_4_holes_mm",
    "raycad-synthetic/dxf_pocket_island_inch",
    "raycad-synthetic/dxf_rounded_plate_mm",
    "raycad-synthetic/dxf_slotted_plate_inch",
]

UNITS = {1: 25.4, 2: 304.8, 3: 1609344.0, 4: 1.0, 5: 10.0, 6: 1000.0, 7: 1e6, 8: 25.4e-6, 9: 0.0254, 10: 914.4,
         11: 1e-7, 12: 1e-6, 13: 1e-3, 14: 100.0, 15: 1e4, 16: 1e5, 17: 1e12, 18: 1.495978707e14,
         19: 9.4607304725808e18, 20: 3.0856775814913673e19, 21: 1200.0 / 3937.0 * 1000.0,
         22: 1200.0 / 3937.0 * 1000.0 / 12.0, 23: 1200.0 / 3937.0 * 1000.0 * 3.0,
         24: 1200.0 / 3937.0 * 1000.0 * 5280.0}


def header_units(path):
    """($INSUNITS, $MEASUREMENT) from the raw group codes of the HEADER section, -1 when absent."""
    with open(path, "rb") as f:
        lines = f.read().decode("latin-1").splitlines()
    ins, meas, var, in_header = -1, -1, None, False
    for i in range(0, len(lines) - 1, 2):
        code, value = lines[i].strip(), lines[i + 1].strip()
        if code == "2" and value == "HEADER":
            in_header = True
        elif code == "0" and value == "ENDSEC" and in_header:
            break
        elif in_header and code == "9":
            var = value
        elif in_header and code == "70" and var == "$INSUNITS":
            ins = int(float(value))
        elif in_header and code == "70" and var == "$MEASUREMENT":
            meas = int(float(value))
    return ins, meas


def mm_per_unit(ins, meas):
    if ins in UNITS:
        return UNITS[ins], "$INSUNITS"
    if meas in (0, 1):
        return (25.4 if meas == 0 else 1.0), "$MEASUREMENT"
    return 1.0, "assumed"


def flat_xy(e, sagitta):
    return [(p.x, p.y) for p in e.flattening(sagitta)]


def plane_ok(e):
    if not e.dxf.hasattr("extrusion"):
        return True
    n = e.dxf.extrusion
    return abs(n.x) <= 1e-9 * n.magnitude and abs(n.y) <= 1e-9 * n.magnitude


def polyline_length(pts):
    return sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts, pts[1:]))


class Totals:
    def __init__(self):
        self.length = 0.0
        self.arc_length = 0.0
        self.arcs = 0
        self.rational = 0.0
        self.points = 0
        self.box = None

    def add_box(self, pts):
        for x, y in pts:
            if self.box is None:
                self.box = [x, y, x, y]
            else:
                self.box = [min(self.box[0], x), min(self.box[1], y), max(self.box[2], x), max(self.box[3], y)]


def primitive(e, t):
    kind = e.dxftype()
    if kind == "LINE":
        s, f = e.dxf.start, e.dxf.end
        length = math.hypot(f.x - s.x, f.y - s.y)
        if length > 1e-12:
            t.length += length
            t.add_box([(s.x, s.y), (f.x, f.y)])
    elif kind in ("ARC", "CIRCLE"):
        if not plane_ok(e):
            return
        r = e.dxf.radius
        if r <= 0:
            return
        if kind == "CIRCLE":
            sweep = 360.0
        else:
            sweep = (e.dxf.end_angle - e.dxf.start_angle) % 360.0
            if sweep == 0:
                sweep = 360.0
        length = r * math.radians(sweep)
        t.length += length
        t.arc_length += length
        t.arcs += 1
        t.add_box(flat_xy(e, r * 1e-7))
    elif kind == "ELLIPSE":
        if not plane_ok(e) or abs(e.dxf.ratio - 1) > 1e-9:
            return
        r = e.dxf.major_axis.magnitude
        sweep = e.dxf.end_param - e.dxf.start_param
        if sweep >= 2 * math.pi - 1e-12 or sweep == 0:
            sweep = 2 * math.pi
        length = r * sweep
        t.length += length
        t.arc_length += length
        t.arcs += 1
        t.add_box(flat_xy(e, r * 1e-7))
    elif kind == "SPLINE":
        if len(e.control_points) == 0:
            return
        try:
            pts = flat_xy(e, 1e-6)
        except ValueError:
            return  # knots that do not match the control points: the reader leaves these out too
        length = polyline_length(pts)
        w = list(e.weights)
        if w and max(w) - min(w) > 1e-12 * max(w):
            t.rational += length
        else:
            t.length += length
            t.add_box(pts)
    elif kind == "POINT":
        t.points += 1
    elif kind in ("LWPOLYLINE", "POLYLINE"):
        if kind == "POLYLINE" and (e.is_poly_face_mesh or e.is_polygon_mesh):
            return
        if not plane_ok(e) and not (kind == "POLYLINE" and e.is_3d_polyline):
            return
        for v in e.virtual_entities():
            primitive(v, t)


def explode(entities, depth=0):
    """Model-space primitives with block references exploded; a reference ezdxf cannot transform (for example a
    zero scale) is skipped, as the RayCAD reader skips it."""
    for e in entities:
        if e.dxftype() == "INSERT":
            if depth > 16:
                continue
            # A Z scale of 0 does not change the XY projection of a planar block, but ezdxf cannot invert it.
            if e.dxf.hasattr("zscale") and e.dxf.zscale == 0:
                e.dxf.zscale = 1
            # A MINSERT array: virtual_entities() explodes only the first copy.
            copies = list(e.multi_insert()) if e.mcount > 1 else [e]
            for copy in copies:
                try:
                    inner = list(copy.virtual_entities())
                except (ZeroDivisionError, ValueError, ArithmeticError):
                    continue
                yield from explode(inner, depth + 1)
        else:
            yield e


def measure(path):
    doc = ezdxf.readfile(path)
    t = Totals()
    for e in explode(doc.modelspace()):
        primitive(e, t)
    return t


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "tests/corpus"
    print("// Generated by tests/unit/sketch/dxf_oracle.py with ezdxf %s; do not edit by hand." % ezdxf.__version__)
    print("// Values in mm (file units times mmPerUnit). See that script for what is counted.")
    print("#pragma once")
    print("#include <array>")
    print("namespace ray::sketch::test {")
    print("struct DxfExpect {")
    print("    const char* id;")
    print("    const char* path; // under tests/corpus/")
    print("    double mmPerUnit;")
    print("    const char* unitSource;")
    print("    double length, arcLength;")
    print("    int arcs;")
    print("    double rationalLength;")
    print("    int points;")
    print("    double x0, y0, x1, y1;")
    print("};")
    print("// clang-format off")
    print("inline constexpr DxfExpect kDxfCorpus[] = {")
    with open(os.path.join(root, "manifest.json"), encoding="utf-8") as f:
        paths = {row["id"]: row["path"] for row in json.load(f)["files"]}
    for cid in FILES:
        rel = paths[cid]
        path = os.path.join(root, rel)
        ins, meas = header_units(path)
        scale, source = mm_per_unit(ins, meas)
        t = measure(path)
        box = [v * scale for v in (t.box or [0, 0, 0, 0])]
        print('    {"%s", "%s", %r, "%s", %r, %r, %d, %r, %d, %r, %r, %r, %r},' % (
            cid, rel, scale, source, t.length * scale, t.arc_length * scale, t.arcs, t.rational * scale, t.points,
            box[0], box[1], box[2], box[3]))
    print("};")
    print("// clang-format on")
    print("} // namespace ray::sketch::test")


if __name__ == "__main__":
    main()
