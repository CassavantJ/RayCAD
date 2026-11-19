// The "datum_plane" feature type (task T-096), private to src/doc and its tests.
//
// Props: {"base": {"principal": "xy" | "yz" | "xz"} | {"datum": "<datum_plane id>"} | {"face": <TopoRef>},
//         "offset"?: <length, default 0>, "flip"?: false}
// The plane is the base moved along its normal by `offset`; `flip` reverses the normal and the x direction (the y
// direction stays). It writes no body; it publishes its plane (PlaneArtifact) for sketches and other datums.
#pragma once

#include <ray/document.h>

#include <memory>
#include <string_view>

namespace ray::doc::features {

inline constexpr std::string_view kDatumPlaneType = "datum_plane";

std::unique_ptr<IFeatureType> makeDatumPlaneFeatureType();
std::string_view datumPlanePropsSchema();

} // namespace ray::doc::features
