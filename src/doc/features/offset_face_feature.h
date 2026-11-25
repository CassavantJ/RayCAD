// The "offset_face" feature type (push/pull; PLAN.md 3.2, task T-082), private to src/doc and its tests.
//
// Props: {"faces": [<TopoRef of a planar face>, ...], "distance": <length>}
// Every face moves along its outward normal by `distance`: positive pushes material out (the part grows), negative
// pulls it in. 0 is refused. Planar faces only (the kernel refuses a curved face with a message saying so). Where a
// neighbouring face is not square to the moved face the kernel does not extend it: the result is a slab or a notch with
// a step there, and the feature carries the kernel's warning saying so. References, warnings, failures and naming as
// delete_face_feature.h says for all three direct edits.
#pragma once

#include <ray/document.h>

#include <memory>
#include <string_view>

namespace ray::doc::features {

inline constexpr std::string_view kOffsetFaceType = "offset_face";

std::unique_ptr<IFeatureType> makeOffsetFaceFeatureType();
std::string_view offsetFacePropsSchema();

} // namespace ray::doc::features
