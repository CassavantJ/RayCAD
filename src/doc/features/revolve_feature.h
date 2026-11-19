// The "revolve" feature type (task T-096), private to src/doc and its tests.
//
// Props: {"sketch": "<sketch feature id>", "regions"?: [[...]] (as extrude),
//         "axis": {"entity": "<line entity id of the sketch>"} | {"principal": "x" | "y" | "z"},
//         "angle"?: <angle, default one full turn>, "direction"?: "normal" | "reverse",
//         "operation": "new" | "join" | "cut" | "intersect", "target": "<body id>"}
// The axis is a line of the same sketch (usually a construction line), running from its first point to its second,
// or a world axis through the origin; it must lie in the sketch plane and the profile on one side of it. "reverse"
// turns the other way. Regions, operations and naming as extrude_feature.h, with the role "revolve".
#pragma once

#include <ray/document.h>

#include <memory>
#include <string_view>

namespace ray::doc::features {

inline constexpr std::string_view kRevolveType = "revolve";

std::unique_ptr<IFeatureType> makeRevolveFeatureType();
std::string_view revolvePropsSchema();

} // namespace ray::doc::features
