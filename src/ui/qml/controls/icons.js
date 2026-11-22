// RayCAD's own icon set: simple line drawings on a 16 x 16 grid, drawn for this project (no third-party icons).
// Each entry is an SVG path; "fill" icons are filled, the rest are stroked at 1.5 units with round caps and joins.
// Icon.qml draws them in the colour it is given, so icons carry no colour of their own.
.pragma library

var paths = {
    "search": { d: "M 11.25 7 A 4.25 4.25 0 1 1 2.75 7 A 4.25 4.25 0 1 1 11.25 7 Z M 10.1 10.1 L 13.5 13.5" },
    "sun": { d: "M 10.75 8 A 2.75 2.75 0 1 1 5.25 8 A 2.75 2.75 0 1 1 10.75 8 Z "
                + "M 8 1.5 V 3 M 8 13 V 14.5 M 1.5 8 H 3 M 13 8 H 14.5 "
                + "M 11.55 4.45 L 12.6 3.4 M 4.45 4.45 L 3.4 3.4 M 11.55 11.55 L 12.6 12.6 M 4.45 11.55 L 3.4 12.6" },
    "moon": { d: "M 13.5 9.6 A 5.75 5.75 0 1 1 6.4 2.5 A 4.6 4.6 0 0 0 13.5 9.6 Z" },
    "glass": { d: "M 5.75 2.75 H 11.75 A 1.75 1.75 0 0 1 13.5 4.5 V 10.25 "
                  + "M 4.25 5.25 H 9.25 A 1.75 1.75 0 0 1 11 7 V 11.75 A 1.75 1.75 0 0 1 9.25 13.5 H 4.25 "
                  + "A 1.75 1.75 0 0 1 2.5 11.75 V 7 A 1.75 1.75 0 0 1 4.25 5.25 Z" },
    // T-057: an operation template (a card over a card, with lines on it).
    "template": { d: "M 5.25 2.75 H 13.25 V 10.75 M 2.75 5.25 H 10.75 V 13.25 H 2.75 Z M 5 8.5 H 8.5 M 5 11 H 8.5" },
    "chevron-down": { d: "M 4.5 6.25 L 8 9.75 L 11.5 6.25" },
    "chevron-up": { d: "M 4.5 9.75 L 8 6.25 L 11.5 9.75" },
    "chevron-right": { d: "M 6.25 4.5 L 9.75 8 L 6.25 11.5" },
    "check": { d: "M 3.5 8.5 L 6.5 11.25 L 12.5 4.75" },
    "dash": { d: "M 4 8 H 12" },
    "close": { d: "M 4.5 4.5 L 11.5 11.5 M 11.5 4.5 L 4.5 11.5" },
    "plus": { d: "M 8 3.5 V 12.5 M 3.5 8 H 12.5" },
    // T-045: back to the default value (a turn anticlockwise, the arrow at its start).
    "reset": { d: "M 4 8 A 4 4 0 1 0 5.2 5.2 M 5.2 2.4 V 5.2 H 2.4" },
    "more": { fill: true,
              d: "M 4.75 8 A 1.25 1.25 0 1 1 2.25 8 A 1.25 1.25 0 1 1 4.75 8 Z "
                 + "M 9.25 8 A 1.25 1.25 0 1 1 6.75 8 A 1.25 1.25 0 1 1 9.25 8 Z "
                 + "M 13.75 8 A 1.25 1.25 0 1 1 11.25 8 A 1.25 1.25 0 1 1 13.75 8 Z" },
    "panel-left": { d: "M 3.5 2.75 H 12.5 A 1.75 1.75 0 0 1 14.25 4.5 V 11.5 A 1.75 1.75 0 0 1 12.5 13.25 H 3.5 "
                       + "A 1.75 1.75 0 0 1 1.75 11.5 V 4.5 A 1.75 1.75 0 0 1 3.5 2.75 Z M 6.25 2.75 V 13.25" },
    "panel-right": { d: "M 3.5 2.75 H 12.5 A 1.75 1.75 0 0 1 14.25 4.5 V 11.5 A 1.75 1.75 0 0 1 12.5 13.25 H 3.5 "
                        + "A 1.75 1.75 0 0 1 1.75 11.5 V 4.5 A 1.75 1.75 0 0 1 3.5 2.75 Z M 9.75 2.75 V 13.25" },
    "cube": { d: "M 8 1.75 L 13.75 4.9 V 11.1 L 8 14.25 L 2.25 11.1 V 4.9 Z M 2.25 4.9 L 8 8.05 L 13.75 4.9 "
                 + "M 8 8.05 V 14.25" },
    "document": { d: "M 3.75 1.75 H 9.25 L 12.25 4.75 V 14.25 H 3.75 Z M 9.25 1.75 V 4.75 H 12.25" },
    // T-022: the measure tools.
    "copy": { d: "M 6 5.75 H 12.25 A 1 1 0 0 1 13.25 6.75 V 13.25 A 1 1 0 0 1 12.25 14.25 H 6 A 1 1 0 0 1 5 13.25 "
                 + "V 6.75 A 1 1 0 0 1 6 5.75 Z M 2.75 10.25 V 3.25 A 1 1 0 0 1 3.75 2.25 H 10.25" },
    "pin": { d: "M 5.75 2.25 H 10.25 M 6.75 2.25 V 6.25 L 4.5 9.25 H 11.5 L 9.25 6.25 V 2.25 M 8 9.25 V 14" },
    "ruler": { d: "M 1.75 5.5 H 14.25 V 10.5 H 1.75 Z M 4.5 5.5 V 7.5 M 7 5.5 V 8.25 M 9.5 5.5 V 7.5 M 12 5.5 V 8.25" },
    "info": { d: "M 14 8 A 6 6 0 1 1 2 8 A 6 6 0 1 1 14 8 Z M 8 7.25 V 11 M 8 4.75 V 5.25" },
    // T-023: a block cut open, its section hatched, the removed part dashed.
    "section": { d: "M 2.75 6.75 H 13.25 V 13.25 H 2.75 Z M 2.75 10.25 L 6.25 6.75 M 5.75 13.25 L 12.25 6.75 "
                    + "M 10.25 13.25 L 13.25 10.25 M 2.75 3 H 4.25 M 6 3 H 7.5 M 9.25 3 H 10.75 M 12.25 3 H 13.25" },
    "sliders": { d: "M 2.5 5 H 13.5 M 2.5 11 H 13.5 "
                    + "M 7 5 A 1.75 1.75 0 1 1 3.5 5 A 1.75 1.75 0 1 1 7 5 Z "
                    + "M 12.5 11 A 1.75 1.75 0 1 1 9 11 A 1.75 1.75 0 1 1 12.5 11 Z" },
    // T-092: the sketch tools (a pencil on a plane; the drawing tools by the shape they make).
    "sketch": { d: "M 2 13.5 H 9 M 6.5 11.5 L 12.75 5.25 L 14.25 6.75 L 8 13 L 6 13.5 Z M 11.5 6.5 L 13 8" },
    "select": { d: "M 4 2.5 L 12 9.25 L 8.25 9.75 L 10.5 13.75 L 9 14.5 L 6.75 10.5 L 4 13 Z" },
    // A point: a small ring on short cross-hair ticks that stop clear of it.
    "point": { d: "M 9.75 8 A 1.75 1.75 0 1 1 6.25 8 A 1.75 1.75 0 1 1 9.75 8 Z "
                  + "M 8 1.75 V 4.25 M 8 11.75 V 14.25 M 1.75 8 H 4.25 M 11.75 8 H 14.25" },
    "line": { d: "M 3 13 L 13 3 M 4.5 13 A 1.5 1.5 0 1 1 1.5 13 A 1.5 1.5 0 1 1 4.5 13 Z "
                 + "M 14.5 3 A 1.5 1.5 0 1 1 11.5 3 A 1.5 1.5 0 1 1 14.5 3 Z" },
    "rectangle": { d: "M 2.5 4 H 13.5 V 12 H 2.5 Z" },
    "circle": { d: "M 13.5 8 A 5.5 5.5 0 1 1 2.5 8 A 5.5 5.5 0 1 1 13.5 8 Z M 8 7.25 V 8.75 M 7.25 8 H 8.75" },
    "arc3": { d: "M 2.5 12 A 6.25 6.25 0 0 1 13.5 12 M 2.5 12 V 12.01 M 8 5.4 V 5.41 M 13.5 12 V 12.01" },
    "arcCenter": { d: "M 13.5 12 A 9 9 0 0 0 4.5 3 M 4.5 12 H 13.5 M 4.5 12 V 3 M 4.5 12 V 12.01" },
    "arcTangent": { d: "M 1.5 12.5 H 7 A 5 5 0 0 0 12 7.5 V 3" },
    "slot": { d: "M 5 4.5 H 11 A 3.5 3.5 0 0 1 11 11.5 H 5 A 3.5 3.5 0 0 1 5 4.5 Z M 5 8 V 8.01 M 11 8 V 8.01" },
    "spline": { d: "M 1.5 12 C 4 2 8 2 8.5 8 C 9 14 13 13 14.5 4" },
    "construction": { d: "M 2 14 L 4 12 M 6 10 L 8 8 M 10 6 L 12 4 M 13.5 2.5 L 14 2" },
    "trim": { d: "M 2 8 H 6 M 10 8 H 14 M 5.5 2.5 L 10.5 13.5 M 10.5 2.5 L 5.5 13.5" },
    "extend": { d: "M 2 8 H 8 M 9.5 8 H 10.5 M 12 8 H 12.5 M 14 2.5 V 13.5 M 9 5.5 L 11.5 8 L 9 10.5" },
    "offset": { d: "M 2 13 V 6 A 3 3 0 0 1 5 3 H 13 M 5 13 V 7.5 A 1.5 1.5 0 0 1 6.5 6 H 13" },
    "fillet": { d: "M 2.5 13.5 V 9 A 6 6 0 0 1 8.5 3 H 13.5" },
    "finish": { d: "M 3.5 8.5 L 6.5 11.25 L 12.5 4.75" },
    // T-098: the feature tools (a profile pushed along an arrow; a profile turned about an axis; a cut corner; a drilled
    // hole seen from above; a plane drawn in perspective) and the feature tree (parameters, a warning, delete).
    "extrude": { d: "M 2 12.5 H 10 L 14 9.5 H 6 Z M 8 8 V 1.75 M 5.75 4 L 8 1.75 L 10.25 4" },
    "revolve": { d: "M 3 1.5 V 3.5 M 3 5.5 V 7.5 M 3 9.5 V 11.5 M 6 2.5 H 10.5 V 8.5 H 6 Z "
                    + "M 14 11 A 5.5 2.5 0 1 1 5 13.4 M 14 11 L 14.6 9 M 14 11 L 12.1 11.4" },
    "fillet3d": { d: "M 2.5 13.5 V 8.5 A 6 6 0 0 1 8.5 2.5 H 13.5 V 13.5 Z" },
    "chamfer": { d: "M 2.5 13.5 V 8.5 L 8.5 2.5 H 13.5 V 13.5 Z" },
    "hole": { d: "M 13.5 8 A 5.5 5.5 0 1 1 2.5 8 A 5.5 5.5 0 1 1 13.5 8 Z M 10.5 8 A 2.5 2.5 0 1 1 5.5 8 A 2.5 2.5 0 1 1 10.5 8 Z" },
    "datum": { d: "M 1.5 11.5 L 5 4.5 H 14.5 L 11 11.5 Z" },
    "parameters": { d: "M 2.5 5 L 7 11 M 7 5 L 2.5 11 M 9.5 6.5 H 14 M 9.5 9.5 H 14" },
    "warning": { d: "M 8 2 L 14.5 13.5 H 1.5 Z M 8 6.5 V 9.5 M 8 11.5 V 11.6" },
    "suppress": { d: "M 6 4 V 12 M 10 4 V 12" },
    "eye": { d: "M 1.75 8 Q 8 1.5 14.25 8 Q 8 14.5 1.75 8 Z M 10 8 A 2 2 0 1 1 6 8 A 2 2 0 1 1 10 8 Z" },
    "eye-off": { d: "M 1.75 8 Q 8 1.5 14.25 8 Q 8 14.5 1.75 8 Z M 3 13 L 13 3" },
    "trash": { d: "M 3 4.5 H 13 M 6.25 4.5 V 2.75 H 9.75 V 4.5 M 4.5 4.5 L 5.25 13.5 H 10.75 L 11.5 4.5" },
    // Inferred-constraint glyphs over the view.
    "glyph-coincident": { fill: true, d: "M 11 8 A 3 3 0 1 1 5 8 A 3 3 0 1 1 11 8 Z" },
    "glyph-center": { d: "M 12.5 8 A 4.5 4.5 0 1 1 3.5 8 A 4.5 4.5 0 1 1 12.5 8 Z M 8 6.25 V 9.75 M 6.25 8 H 9.75" },
    "glyph-midpoint": { d: "M 2 11 H 14 M 8 4 V 11 M 6 6 L 8 4 L 10 6" },
    "glyph-onCurve": { d: "M 2 13 Q 8 1 14 13 M 9.5 7 A 1.5 1.5 0 1 1 6.5 7 A 1.5 1.5 0 1 1 9.5 7 Z" },
    "glyph-horizontal": { d: "M 2.5 8 H 13.5 M 2.5 5.5 V 10.5 M 13.5 5.5 V 10.5" },
    "glyph-vertical": { d: "M 8 2.5 V 13.5 M 5.5 2.5 H 10.5 M 5.5 13.5 H 10.5" },
    "glyph-tangent": { d: "M 11.5 10 A 3.5 3.5 0 1 1 4.5 10 A 3.5 3.5 0 1 1 11.5 10 Z M 1.5 6.5 H 14.5" },
    "glyph-dimension": { d: "M 2.5 8 H 13.5 M 2.5 5 V 11 M 13.5 5 V 11 M 4.5 6.5 L 2.5 8 L 4.5 9.5 "
                            + "M 11.5 6.5 L 13.5 8 L 11.5 9.5" },
    // T-093: the other constraint glyphs (also the constraint toolbar's icons), driven dimensions, deleting.
    "glyph-parallel": { d: "M 3 12.5 L 8.5 3 M 7.5 13 L 13 3.5" },
    "glyph-perpendicular": { d: "M 2.5 13 H 13.5 M 8 13 V 2.5 M 8 10 H 11 V 13" },
    "glyph-equal": { d: "M 3 6 H 13 M 3 10 H 13" },
    "glyph-concentric": { d: "M 13.5 8 A 5.5 5.5 0 1 1 2.5 8 A 5.5 5.5 0 1 1 13.5 8 Z "
                             + "M 10.5 8 A 2.5 2.5 0 1 1 5.5 8 A 2.5 2.5 0 1 1 10.5 8 Z" },
    "glyph-collinear": { d: "M 2 14 L 6.5 9.5 M 9.5 6.5 L 14 2 M 8 8 V 8.01" },
    "glyph-symmetric": { d: "M 8 2 V 3.5 M 8 5.5 V 7 M 8 9 V 10.5 M 8 12.5 V 14 "
                            + "M 5.5 5 L 2 8 L 5.5 11 M 10.5 5 L 14 8 L 10.5 11" },
    "glyph-fix": { d: "M 4.5 7.5 H 11.5 V 13.5 H 4.5 Z M 6 7.5 V 5.5 A 2 2 0 0 1 10 5.5 V 7.5" },
    "driven": { d: "M 5 3 Q 2.5 8 5 13 M 11 3 Q 13.5 8 11 13 M 6.5 8 H 9.5" },
    "delete": { d: "M 3 4.5 H 13 M 6.5 4.5 V 3 H 9.5 V 4.5 M 4.5 4.5 L 5.25 13.5 H 10.75 L 11.5 4.5" },
    // T-046: the tool library (an end mill on its side view), read-only items, reading folders again.
    "endmill": { d: "M 6 1.75 H 10 V 6.5 H 6 Z M 6.5 6.5 V 13.5 L 8 14.5 L 9.5 13.5 V 6.5 "
                    + "M 6.5 9 L 9.5 7.75 M 6.5 11.5 L 9.5 10.25" },
    "lock": { d: "M 4.75 7.25 H 11.25 A 1 1 0 0 1 12.25 8.25 V 12.75 A 1 1 0 0 1 11.25 13.75 H 4.75 "
                 + "A 1 1 0 0 1 3.75 12.75 V 8.25 A 1 1 0 0 1 4.75 7.25 Z M 5.75 7.25 V 5.25 A 2.25 2.25 0 0 1 10.25 5.25 V 7.25" },
    "refresh": { d: "M 12.75 8 A 4.75 4.75 0 1 1 11.2 4.5 M 12.25 2 V 5 H 9.25" },
    // T-047: post a setup (a program page with lines of code and an arrow leaving it).
    "post": { d: "M 10.5 6 V 2.25 H 2.75 V 13.75 H 10.5 V 11.5 M 4.75 5 H 8.25 M 4.75 7.5 H 7 M 4.75 10 H 7.75 "
                 + "M 8.75 8.75 H 14.25 M 12.25 6.75 L 14.25 8.75 L 12.25 10.75" }
};

function path(name) {
    var entry = paths[name];
    return entry ? entry.d : "";
}

function filled(name) {
    var entry = paths[name];
    return entry ? entry.fill === true : false;
}
