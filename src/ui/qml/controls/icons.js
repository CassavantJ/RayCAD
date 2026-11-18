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
    "chevron-down": { d: "M 4.5 6.25 L 8 9.75 L 11.5 6.25" },
    "chevron-up": { d: "M 4.5 9.75 L 8 6.25 L 11.5 9.75" },
    "chevron-right": { d: "M 6.25 4.5 L 9.75 8 L 6.25 11.5" },
    "check": { d: "M 3.5 8.5 L 6.5 11.25 L 12.5 4.75" },
    "dash": { d: "M 4 8 H 12" },
    "close": { d: "M 4.5 4.5 L 11.5 11.5 M 11.5 4.5 L 4.5 11.5" },
    "plus": { d: "M 8 3.5 V 12.5 M 3.5 8 H 12.5" },
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
    "sliders": { d: "M 2.5 5 H 13.5 M 2.5 11 H 13.5 "
                    + "M 7 5 A 1.75 1.75 0 1 1 3.5 5 A 1.75 1.75 0 1 1 7 5 Z "
                    + "M 12.5 11 A 1.75 1.75 0 1 1 9 11 A 1.75 1.75 0 1 1 12.5 11 Z" }
};

function path(name) {
    var entry = paths[name];
    return entry ? entry.d : "";
}

function filled(name) {
    var entry = paths[name];
    return entry ? entry.fill === true : false;
}
