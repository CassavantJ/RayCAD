#!/usr/bin/env python3
"""Writes the synthetic DXF profiles and the deliberately broken files of the test corpus (PLAN.md section 11).

Run after tools/corpus_gen (the OpenCASCADE generator of the synthetic STEP and IGES parts), with the same
output folder: the broken STEP and IGES files are cut from two of its parts. Standard library only; the output is
the same on every run and carries no user, machine or path information.

DXF profiles (R12, LINE/ARC/CIRCLE and 2D POLYLINE with bulges): 2D job-shop outlines in mm and inch, closed and
open contours, holes, slots, fillets, a keyhole, a flange gasket, layers, and a contour with a tiny gap.
Broken files (manifest "expect": "fail"): truncated STEP, STEP with a wrong header, empty STEP, an HTML error page
saved as STEP, truncated IGES, and DXF cut off without EOF.

Usage: corpus_synth.py [--out tests/corpus/files/raycad-synthetic]
Prints one line per file: name, then a short description.
"""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(os.path.dirname(HERE), "tests", "corpus", "files", "raycad-synthetic")
INSUNITS = {"mm": 4, "inch": 1}


class Dxf:
    """A minimal R12 DXF writer: HEADER with version and units, LAYER table, ENTITIES."""

    def __init__(self, units):
        self.units = units
        self.entities = []
        self.layers = ["0"]

    def _layer(self, layer):
        if layer not in self.layers:
            self.layers.append(layer)

    def line(self, a, b, layer="0"):
        self._layer(layer)
        self.entities.append(["0", "LINE", "8", layer, "10", a[0], "20", a[1], "30", 0.0,
                              "11", b[0], "21", b[1], "31", 0.0])

    def circle(self, c, r, layer="0"):
        self._layer(layer)
        self.entities.append(["0", "CIRCLE", "8", layer, "10", c[0], "20", c[1], "30", 0.0, "40", r])

    def arc(self, c, r, start_deg, end_deg, layer="0"):
        """Counter-clockwise arc from start to end angle, in degrees."""
        self._layer(layer)
        self.entities.append(["0", "ARC", "8", layer, "10", c[0], "20", c[1], "30", 0.0, "40", r,
                              "50", start_deg, "51", end_deg])

    def polyline(self, vertices, closed, layer="0"):
        """vertices: (x, y, bulge); bulge = tan(sweep/4) of the arc to the next vertex, 0 for a straight segment."""
        self._layer(layer)
        entity = ["0", "POLYLINE", "8", layer, "66", 1, "10", 0.0, "20", 0.0, "30", 0.0, "70", 1 if closed else 0]
        for x, y, bulge in vertices:
            entity += ["0", "VERTEX", "8", layer, "10", x, "20", y, "30", 0.0]
            if bulge:
                entity += ["42", bulge]
        entity += ["0", "SEQEND", "8", layer]
        self.entities.append(entity)

    def rounded_rect(self, x0, y0, x1, y1, r, layer="0"):
        """Closed outline of lines and arcs."""
        self.line((x0 + r, y0), (x1 - r, y0), layer)
        self.arc((x1 - r, y0 + r), r, 270, 360, layer)
        self.line((x1, y0 + r), (x1, y1 - r), layer)
        self.arc((x1 - r, y1 - r), r, 0, 90, layer)
        self.line((x1 - r, y1), (x0 + r, y1), layer)
        self.arc((x0 + r, y1 - r), r, 90, 180, layer)
        self.line((x0, y1 - r), (x0, y0 + r), layer)
        self.arc((x0 + r, y0 + r), r, 180, 270, layer)

    def obround(self, cx, cy, length, width, layer="0"):
        """Closed slot along X as a polyline with two half-circle bulges."""
        r, h = width / 2, length / 2 - width / 2
        self.polyline([(cx - h, cy - r, 0), (cx + h, cy - r, 1.0), (cx + h, cy + r, 0), (cx - h, cy + r, 1.0)],
                      True, layer)

    def text(self):
        def fmt(value):
            if isinstance(value, float):
                text = ("%.6f" % value).rstrip("0")
                return text + "0" if text.endswith(".") else text
            return str(value)

        out = ["999", "RayCAD corpus_synth.py (synthetic test profile)",
               "0", "SECTION", "2", "HEADER",
               "9", "$ACADVER", "1", "AC1009",
               "9", "$INSUNITS", "70", INSUNITS[self.units],
               "9", "$MEASUREMENT", "70", 1 if self.units == "mm" else 0,
               "0", "ENDSEC",
               "0", "SECTION", "2", "TABLES", "0", "TABLE", "2", "LAYER", "70", len(self.layers)]
        for layer in self.layers:
            out += ["0", "LAYER", "2", layer, "70", 0, "62", 7, "6", "CONTINUOUS"]
        out += ["0", "ENDTAB", "0", "ENDSEC", "0", "SECTION", "2", "ENTITIES"]
        for entity in self.entities:
            out += entity
        out += ["0", "ENDSEC", "0", "EOF"]
        return "\n".join(fmt(v) for v in out) + "\n"


def profiles():
    """(name, units, description, Dxf)."""
    result = []

    d = Dxf("mm")
    d.polyline([(0, 0, 0), (100, 0, 0), (100, 60, 0), (0, 60, 0)], True)
    for x in (10, 90):
        for y in (10, 50):
            d.circle((x, y), 3.3)
    result.append(("dxf_plate_4_holes_mm", "mm", "rectangular plate, four 6.6 mm holes", d))

    d = Dxf("mm")
    d.rounded_rect(0, 0, 120, 80, 8)
    d.circle((60, 40), 10)
    result.append(("dxf_rounded_plate_mm", "mm", "rounded rectangle of lines and arcs, centre bore", d))

    d = Dxf("inch")
    d.polyline([(0, 0, 0), (4, 0, 0), (4, 2, 0), (0, 2, 0)], True)
    d.obround(1.0, 1.0, 1.25, 0.375)
    d.obround(3.0, 1.0, 1.25, 0.375)
    result.append(("dxf_slotted_plate_inch", "inch", "4 x 2 in plate with two obround slots (bulge polylines)", d))

    d = Dxf("mm")
    d.circle((0, 0), 60)
    d.circle((0, 0), 30)
    for i in range(6):
        angle = 2 * math.pi * i / 6
        d.circle((45 * math.cos(angle), 45 * math.sin(angle)), 4.5)
    result.append(("dxf_flange_gasket_mm", "mm", "flange gasket: OD 120, ID 60, six holes on PCD 90", d))

    d = Dxf("mm")
    t, r = 8.0, 6.0
    d.line((0, 0), (80, 0))
    d.line((80, 0), (80, t))
    d.line((80, t), (t + r, t))
    d.arc((t + r, t + r), r, 180, 270)
    d.line((t, t + r), (t, 60))
    d.line((t, 60), (0, 60))
    d.line((0, 60), (0, 0))
    result.append(("dxf_l_bracket_profile_mm", "mm", "L-bracket section with inner fillet arc", d))

    d = Dxf("inch")
    d.rounded_rect(0, 0, 3, 2, 0.25)
    d.rounded_rect(0.5, 0.5, 2.5, 1.5, 0.125, layer="POCKET")
    d.circle((1.5, 1.0), 0.25, layer="ISLAND")
    result.append(("dxf_pocket_island_inch", "inch", "outline, pocket boundary and island on separate layers", d))

    d = Dxf("mm")
    hexagon = [(20 * math.cos(math.pi / 6 + i * math.pi / 3), 20 * math.sin(math.pi / 6 + i * math.pi / 3), 0)
               for i in range(6)]
    d.polyline(hexagon, True)
    d.circle((0, 0), 8.5)
    result.append(("dxf_hex_profile_mm", "mm", "hexagon across flats 34.64 with M10 tap-drill hole", d))

    d = Dxf("mm")
    d.polyline([(0, 0, 0), (60, 0, 0), (60, 40, 0), (0, 40, 0)], True)
    rb, rs, cy = 6.0, 3.0, 26.0
    # Keyhole: a 12 mm circle above a 6 mm slot, as one closed contour of lines and arcs.
    a = math.degrees(math.asin(rs / rb))
    d.arc((30, cy), rb, 270 + a, 270 - a)
    ylow = cy - rb * math.cos(math.radians(a))
    d.line((30 - rs, ylow), (30 - rs, 10))
    d.arc((30, 10), rs, 180, 360)
    d.line((30 + rs, 10), (30 + rs, ylow))
    result.append(("dxf_keyhole_mm", "mm", "plate with keyhole slot (arcs tangent to lines)", d))

    d = Dxf("mm")
    d.polyline([(0, 0, 0), (40, 0, 0), (40, 20, 0.4142135623730951), (60, 40, 0), (100, 40, 0)], False)
    result.append(("dxf_open_contour_mm", "mm", "open contour (not closed) with one bulge arc", d))

    d = Dxf("inch")
    d.line((0, 0), (2, 0))
    d.line((2, 0), (2, 1))
    d.line((2, 1), (0, 1))
    d.line((0, 1), (0, 0.001))  # 0.001 in gap to the start point
    d.line((0.5, 0.5), (1.5, 0.5), layer="CONSTRUCTION")
    d.circle((1, 0.5), 0.75, layer="CONSTRUCTION")
    result.append(("dxf_gap_and_construction_inch", "inch",
                   "rectangle with a 0.001 in gap, plus construction geometry on its own layer", d))
    return result


def broken_files(out):
    """(name, description, bytes). Cut from parts written by corpus_gen in the same folder."""
    step_path = os.path.join(out, "plate_thru_holes_1.stp")
    iges_path = os.path.join(out, "closed_pocket_1_brep.igs")
    with open(step_path, "rb") as handle:
        step = handle.read()
    with open(iges_path, "rb") as handle:
        iges = handle.read()
    lines = iges.split(b"\n")
    html = (b"<!DOCTYPE html>\n<html><head><title>404 Not Found</title></head>\n"
            b"<body><h1>Not Found</h1><p>The requested URL was not found on this server.</p></body></html>\n")
    dxf = profiles()[0][3].text().encode()
    return [
        ("broken_truncated.stp", "STEP cut at 40% of its length, inside the DATA section", step[: len(step) * 2 // 5]),
        ("broken_wrong_header.stp", "STEP whose first line is not ISO-10303-21",
         step.replace(b"ISO-10303-21;", b"ISO-10303-28;", 1)),
        ("broken_empty.stp", "empty file with a STEP extension", b""),
        ("broken_html_page.stp", "an HTTP error page saved under a STEP name", html),
        ("broken_truncated.igs", "IGES cut in the middle of the directory entry section",
         b"\n".join(lines[: len(lines) // 3]) + b"\n"),
        ("broken_no_eof.dxf", "DXF cut inside ENTITIES, no ENDSEC and no EOF", dxf[: len(dxf) * 3 // 4]),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=DEFAULT_OUT)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for name, units, description, dxf in profiles():
        with open(os.path.join(args.out, name + ".dxf"), "w", encoding="ascii", newline="\n") as handle:
            handle.write(dxf.text())
        print("%s.dxf\t%s\t%s" % (name, units, description))
    try:
        broken = broken_files(args.out)
    except OSError as error:
        print("corpus_synth: run tools/corpus_gen into %s first (%s)" % (args.out, error), file=sys.stderr)
        return 1
    for name, description, data in broken:
        with open(os.path.join(args.out, name), "wb") as handle:
            handle.write(data)
        print("%s\tbroken\t%s" % (name, description))
    return 0


if __name__ == "__main__":
    sys.exit(main())
