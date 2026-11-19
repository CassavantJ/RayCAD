// Geometric constraints in the desktop app (task T-093), without Qt: which constraint a selection of sketch entities
// can take (and what to select when it cannot), the sketch::Constraint records that apply it, and where the glyph of
// each existing constraint goes.
//
// The kinds are those sketch.h has, plus collinear, which sketch.h lacks: it is applied as two point-on-curve
// constraints (the second line's end points on the first line's carrier), one step. A point and a curve made
// coincident become PointOnCurve.
#pragma once

#include "sketch_geom.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::ui::sketchui {

enum class GeoKind {
    Coincident,
    Horizontal,
    Vertical,
    Parallel,
    Perpendicular,
    Tangent,
    Equal,
    Midpoint,
    Concentric,
    Collinear,
    Symmetric,
    Fix
};
inline constexpr GeoKind kGeoKinds[] = {GeoKind::Coincident, GeoKind::Horizontal,    GeoKind::Vertical,
                                        GeoKind::Parallel,   GeoKind::Perpendicular, GeoKind::Tangent,
                                        GeoKind::Equal,      GeoKind::Midpoint,      GeoKind::Concentric,
                                        GeoKind::Collinear,  GeoKind::Symmetric,     GeoKind::Fix};

const char* geoName(GeoKind kind); // "coincident", ... (command ids sketch.constrain.<name>, glyph names)
std::optional<GeoKind> geoFromName(std::string_view name);
const char* geoTitle(GeoKind kind);       // "Coincident", ... (English; the caller translates)
const char* geoRequirement(GeoKind kind); // "Select two points, or a point and a curve", ...

// The constraints that apply `kind` to `selection` (entity ids, any order); empty when the selection does not fit.
// Constraints the model has already (same type, same references) are left out; `already` is set when that leaves
// nothing to add.
std::vector<sketch::Constraint> planConstraint(GeoKind kind, const SketchView& view, const sketch::Sketch& model,
                                               const std::vector<SketchEntId>& selection, bool* already = nullptr);

// The glyph name of a constraint type (icons.js "glyph-<name>"): "coincident", "onCurve", "horizontal", ...
const char* glyphOf(sketch::ConstraintType type);
// An English title of a constraint type ("Distance", "Horizontal", ...).
const char* typeTitle(sketch::ConstraintType type);

// Where the glyphs of a (non-dimension) constraint go, in mm: one place per entity it is shown at.
std::vector<Vec2> glyphPlaces(const SketchView& view, const sketch::Constraint& c);

} // namespace ray::ui::sketchui
