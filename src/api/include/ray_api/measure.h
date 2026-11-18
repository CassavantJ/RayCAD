// Measure and analysis tools (task T-022): distance, angle, radius and diameter, an element's own measures, mass
// properties and the bounding box, for a job-shop user checking an imported part. No Qt here.
//
// Pure functions over kernel::Shape + SubId (src/ui may link only api and render, so the UI's Measure panel calls these
// on a worker, and the measure.* actions call them for scripts and agents). Every function runs kernel queries: call
// it on a worker. Results are structured: every value in internal units (mm, mm², mm³, radians, grams, g/cm³) with a
// stable key, plus the witness geometry needed to draw it (points, lines, the angle arc as line segments, and where
// its labels go). toJson() gives the actions' canonical result JSON. formatValue() turns a value into display text in
// a document's units.
//
// What the kernel's queries cannot answer yet is said in the measurement's notes, never guessed: the minor (tube)
// radius of a torus and the minor radius of an ellipse are not in kernel::SurfaceInfo / CurveInfo; the minimum
// internal corner radius arrives with task T-050.
#pragma once

#include <ray/api.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ray::api::measure {

// What a value is, which decides its display unit.
enum class Quantity : std::uint8_t {
    Length,    // mm
    Angle,     // radians (shown in degrees)
    Area,      // mm²
    Volume,    // mm³
    Mass,      // grams
    Density,   // g/cm³
    Point,     // `vector` in mm
    Size,      // `vector` in mm: the X, Y and Z extents of a box
    Direction, // `vector`, unit length, no unit
    Count,     // `number`, an integer
    Text,      // `text`
};

struct Value {
    std::string key;   // stable id for scripts and tests: "distance", "dx", "planeDistance", "radius", ...
    std::string label; // English, for the read-out (the UI translates it)
    Quantity quantity = Quantity::Length;
    double number = 0;
    Vec3 vector{};
    std::string text;
    bool primary = false; // the headline value of the measurement
};

// Witness geometry, in world mm. `construction` lines are axes, extension lines and the box outline (drawn dashed).
struct Segment {
    Vec3 a, b;
    bool construction = false;
};
struct Label {
    Vec3 at;
    std::string key; // the value shown there
};
struct Witness {
    std::vector<Vec3> points;
    std::vector<Segment> lines;
    std::vector<Label> labels;
};

enum class Kind : std::uint8_t { Element, Distance, Angle, Radius, MassProperties, BoundingBox, Pair };

struct Measurement {
    Kind kind = Kind::Element;
    std::string title; // "Distance", "Angle", "Face", "Body", ...
    std::vector<Value> values;
    Witness witness;
    std::vector<std::string> notes;

    const Value* find(std::string_view key) const;
    const Value* primary() const; // the first primary value, else the first value; null when there is none
};

// One element of a body. A Solid SubId (any index) is the whole body.
struct Element {
    kernel::Shape shape;
    kernel::SubId sub{kernel::SubKind::Solid, 0};
    std::string name; // the body's name, for notes
};

// Steel. Mass properties take the density in g/cm³.
constexpr double kDefaultDensity = 7.85;

// Minimum distance between two elements (vertices, edges, faces or bodies, possibly of different bodies) with both
// witness points and the X/Y/Z components of the vector from the first to the second. Where it applies, also: the
// plane-to-plane distance of two parallel planar faces, the line distance of two parallel straight edges, the distance
// of a point to a plane, a line or an axis, and for two round elements (cylinder or cone faces, circular edges,
// spheres) the centre-to-centre (axis) distance with its components. The special distance is then the primary value.
Result<Measurement> distance(const Element& a, const Element& b, JobContext&);
// Angle between two planar faces, two straight edges (or the axes of round faces and edges), or a line and a plane:
// always the acute angle and its obtuse supplement. Two lines that share an end also give the included angle at that
// corner; two faces that share an edge give the angle of the material at that edge.
Result<Measurement> angle(const Element& a, const Element& b, JobContext&);
// Radius and diameter of a circular edge or a cylindrical face; the half angle of a cone; the radius of a sphere; the
// major radius of a torus. InvalidArgument for anything not round.
Result<Measurement> radius(const Element&, JobContext&);
// The element's own measures: a vertex's position; an edge's length (and radius for a circle); a face's area (and its
// normal or radius); a body's mass properties and bounding box.
Result<Measurement> element(const Element&, double densityGPerCm3, JobContext&);
// Volume, surface area, centroid and mass (volume x density) of the bodies together.
Result<Measurement> massProperties(std::span<const kernel::Shape>, double densityGPerCm3, JobContext&);
// The axis-aligned bounding box of the bodies together, with its sizes.
Result<Measurement> boundingBox(std::span<const kernel::Shape>, JobContext&);
// The right measurement for a selection: one element: element(); two: distance(), plus the angle when both have a
// direction (Kind::Pair); more than two: the last two, with a note. InvalidArgument for none.
Result<Measurement> selection(std::span<const Element>, double densityGPerCm3, JobContext&);

// ---------- Display ----------
// The value in `format`'s units: lengths through formatLength ("1.1811 in", "30.000 mm"), areas in in² / mm², volumes
// in in³ / mm³, angles in degrees with 3 decimals, mass in lb or g / kg, density in lb/in³ or g/cm³, points and sizes
// with one unit at the end ("1.0000, 2.0000, 0.5000 in", "120.000 \u00d7 80.000 \u00d7 10.000 mm").
std::string formatValue(const Value&, UnitFormat);

// ---------- JSON ----------
// The canonical result JSON of the measure.* actions:
//   {"kind": "distance" | "angle" | "radius" | "element" | "massProperties" | "boundingBox" | "pair",
//    "title": "...",
//    "values": [{"key", "label", "quantity", "primary", and "value": number | [x, y, z] | "text"}, ...],
//    "measures": {"<key>": number | [x, y, z] | "text", ...},
//    "witness": {"points": [[x, y, z], ...], "lines": [{"a": [x, y, z], "b": [x, y, z], "construction": bool}, ...],
//                "labels": [{"at": [x, y, z], "key": "..."}]},
//    "notes": ["..."]}
// Numbers are mm, mm², mm³, radians, grams and g/cm³.
std::string toJson(const Measurement&);
std::string_view kindName(Kind);
std::string_view quantityName(Quantity);

} // namespace ray::api::measure
