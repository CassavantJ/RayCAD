// Sketch dimensions in the desktop app (task T-093), without Qt: what a dimension measures on the shown geometry,
// how it is drawn (extension lines, the dimension line or arc, arrowheads, where its value label sits), and which
// dimension the smart dimension tool makes from what is picked.
//
// Everything is in mm (radians for angles) on the sketch plane. A label's position is kept as an offset from the
// dimension's anchor (the middle of a distance, the centre of a circle or arc, the vertex of an angle), so it moves
// with the geometry; the sketch feature stores that offset ("labels" props, sketch.dimension.place).
//
// Reference conventions (the solver's, src/sketch/solver_translate.cpp):
//   Distance   [line] its length; [point, point]; [point, line] perpendicular; [point, circle|arc], [circle|arc,
//              line] and [circle|arc, circle|arc] the gap between them
//   DistanceX / DistanceY   [point, point] or [line]: the signed difference second - first; [point] from the origin
//   Radius / Diameter       [circle | arc]
//   Angle      [line, line]: the signed angle from the first line's direction to the second's; [line]: from the x axis
#pragma once

#include "sketch_geom.h"

#include <optional>
#include <string>
#include <vector>

namespace ray::ui::sketchui {

bool isDimension(sketch::ConstraintType type);

// The value the geometry has now for a dimension's references, signed as the solver reads it (DistanceX, DistanceY
// and Angle are signed; the others are not). nullopt when the references do not fit the type.
std::optional<double> measureSigned(const SketchView& view, const sketch::Constraint& c);

// How a dimension is drawn.
struct DimensionShape {
    struct Arrow {
        Vec2 tip;
        Vec2 dir; // unit, the way the arrowhead points (towards the tip)
    };
    std::vector<std::vector<Vec2>> lines; // extension lines, the dimension line or arc, leaders
    std::vector<Arrow> arrows;
    Vec2 anchor{}; // what the label offset is measured from
    Vec2 label{};  // where the value label's centre is
};

// The shape of dimension `c` with its label at anchor + offset; with no offset the label goes `gap` mm off the
// geometry (a distance to the side, a radius out along a diagonal). nullopt when the references do not fit.
std::optional<DimensionShape> layoutDimension(const SketchView& view, const sketch::Constraint& c,
                                              std::optional<Vec2> labelOffset, double gap);

// ---------- the smart dimension tool ----------
// The dimension made from up to two picked entities, with its value now (signed as measureSigned gives it), for a
// label at `cursor`:
//   one line          its length
//   two points        their distance; horizontal or vertical when the cursor is beside the pair's box in y or in x
//   a point, a line   the perpendicular distance (the line's own end point: its length)
//   two lines         the angle between them, or the distance between them when they are parallel
//   a circle          its diameter; an arc: its radius
//   point and circle, line and circle, two circles: the distance between centres or to the curve
// nullopt when the picks make no dimension.
struct DimensionPick {
    sketch::ConstraintType type = sketch::ConstraintType::Distance;
    std::vector<SketchEntId> refs;
    double value = 0;
};
std::optional<DimensionPick> pickDimension(const SketchView& view, const std::vector<SketchEntId>& picks, Vec2 cursor);

// The value to store when someone types `typed` (mm or rad, as shown: not negative) for a dimension whose value now
// is `current`: signed dimensions keep the sign of their present value.
double signedValue(sketch::ConstraintType type, double current, double typed);
// The value as shown (not negative).
double shownValue(sketch::ConstraintType type, double value);

} // namespace ray::ui::sketchui
