// The "resize_hole" feature type (PLAN.md 3.2, task T-082), private to src/doc and its tests.
//
// Props: {"face": <TopoRef of a face of a hole>, "diameter": <length>}
// The hole the face belongs to gets the new diameter (larger or smaller); any face of the bore stands for the whole
// bore, which real holes often have in two or more faces (kernel.h resizeHole). The bottom of a blind hole keeps its
// shape. A diameter that is not above 0, a face that is not part of a round hole and a hole that already has the
// diameter are refused (the kernel's messages, or the feature's for the diameter).
// A face reference that resolves Split (the bore was cut into pieces by an earlier change) is resized through its
// first piece, and the feature then checks that every piece took the new diameter: pieces that the kernel does not see
// as one bore (a slot cut right through the hole) fail the feature with ReferenceBroken, so no piece is left at the old
// size behind a success. References, warnings, failures and naming otherwise as delete_face_feature.h says.
#pragma once

#include <ray/document.h>

#include <memory>
#include <string_view>

namespace ray::doc::features {

inline constexpr std::string_view kResizeHoleType = "resize_hole";

std::unique_ptr<IFeatureType> makeResizeHoleFeatureType();
std::string_view resizeHolePropsSchema();

} // namespace ray::doc::features
