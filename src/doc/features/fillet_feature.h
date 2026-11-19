// The "fillet" and "chamfer" feature types (task T-096), private to src/doc and its tests.
//
// Props: fillet {"edges": [<TopoRef of an edge>, ...], "radius": <length>}
//        chamfer {"edges": [...], "distance": <length>}
// Every edge reference is resolved on the body as it is just before the feature (naming::IResolver). Exact, Split
// (every piece is blended) and Merged (the whole edge that now holds the picked one is blended) are used; Split and
// Merged add a warning so the change can be reviewed. A reference that is Broken, or Repaired and not confirmed by
// the user, fails the feature with a message naming the edge: nothing is ever blended on a guess (PLAN.md 3.3).
// OCCT extends a blend along tangent edges; the kernel's warning saying so is passed on. All edges must be on one
// body, which the feature writes.
#pragma once

#include <ray/document.h>

#include <memory>
#include <string_view>

namespace ray::doc::features {

inline constexpr std::string_view kFilletType = "fillet";
inline constexpr std::string_view kChamferType = "chamfer";

std::unique_ptr<IFeatureType> makeFilletFeatureType();
std::unique_ptr<IFeatureType> makeChamferFeatureType();
std::string_view filletPropsSchema();
std::string_view chamferPropsSchema();

// Shared by both (fillet_feature.cpp).
struct BlendKind {
    std::string_view type;  // "fillet"
    std::string_view title; // "Fillet"
    std::string_view size;  // "radius"
    std::string_view schema;
    bool fillet = true;
};
std::unique_ptr<IFeatureType> makeBlendFeatureType(const BlendKind& kind);

} // namespace ray::doc::features
