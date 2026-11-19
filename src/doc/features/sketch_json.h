// The JSON form of a sketch model (PLAN.md 3.4; task T-096), private to src/doc and its tests.
//
// {"schema": 1,
//  "entities": [ {"id", "type": "point", "x", "y"},
//                {"id", "type": "line", "a": "<point id>", "b": "<point id>"},
//                {"id", "type": "arc", "center", "start", "end", "ccw": true},
//                {"id", "type": "circle", "center", "radius"},
//                {"id", "type": "spline", "points": ["<point id>", ...], "degree": 3, "closed": false},
//                {"id", "type": "slot", "a", "b", "width"} ],          each may add "construction": true
//  "constraints": [ {"id", "type", "refs": ["<entity id>", ...], "expression"?: "...", "value"?: n,
//                    "driven"?: false} ] }
// Constraint types: coincident horizontal vertical parallel perpendicular tangent equal symmetric concentric midpoint
// pointOnCurve fix distance distanceX distanceY radius diameter angle. Coordinates and values are mm and radians; a
// dimension's "expression" (over document parameters, e.g. "width / 2") is evaluated by the rebuild and overrides
// "value". Entities and constraints are written sorted by id; "construction" and "driven" only when true.
#pragma once

#include "feature_props.h"

#include <ray/sketch.h>

#include <optional>
#include <string>
#include <string_view>

namespace ray::doc::features {

Result<sketch::Sketch> sketchFromJson(const PropsReader& r, const json::Value& value, const std::string& field);
json::Value sketchToJson(const sketch::Sketch& sketch);

const char* constraintTypeName(sketch::ConstraintType type);
std::optional<sketch::ConstraintType> constraintTypeFromName(std::string_view name);
// Distance, DistanceX, DistanceY, Radius, Diameter, Angle.
bool isDimension(sketch::ConstraintType type);

// The tag closedLoops gives the curves of an entity (sketch.h: the low 64 bits of the BLAKE3 hash of the id), and the
// tags of the four curves of a slot (part 0 side a->b, 1 cap at b, 2 side b->a, 3 cap at a).
std::uint64_t entityCurveTag(const SketchEntId& entity);
std::uint64_t slotPartCurveTag(const SketchEntId& slot, int part);

std::string entityText(const SketchEntId& id);

} // namespace ray::doc::features
