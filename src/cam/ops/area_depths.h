// The depths of the area above a floor face, and that area at each depth: the one implementation of the rule that
// pocket2d.h (THE POCKET, DEPTHS) states and adaptive2d shares (adaptive2d/op.h THE AREAS, DEPTHS). Private to src/cam.
//
// THE AREA AT A DEPTH z: the footprint minus the part's material at z - stockFloor and above (the union of the part's
// slices, each taken stockFloor lower, at z and at every height of the list below that lies above z: the depths, and
// the candidate faces whether or not they became depths), keeping the pieces that meet the floor face (a part of the
// footprint that is closed off by material at that height, such as a pocket inside the part when its outside is
// roughed, is not the area).
// THE DEPTHS: the operation's own levels (its stepdown), and the height h + stockFloor of every CANDIDATE face that
// MATTERS at that depth. A candidate is an up-facing flat face of the part whose height + stockFloor lies strictly
// between `low` and the top and that overlaps the footprint (a part wider than 2 kFaceMeetNm). It matters where the
// area the tool reaches at that depth (the area shrunk by `keep`, how far the operation keeps the tool centre from the
// walls, and grown back by the tool radius `reach`) overlaps the face in a part wider than 2 kFaceMeetNm: the top of a
// low island, a lower step of the part open to the area, such a face only partly inside the area. There the levels
// alone would cut over the face and leave the material on it (or cut nothing of it when no level lies above it), so a
// depth is added at it. A face the tool does not reach at its height (the floor of a pocket closed off inside the
// part, a ledge on a closed pocket's wall, which is outside its footprint) adds nothing: what the tool cuts there is
// what it cuts at the next level down, and a depth would repeat those passes. A candidate at the height of a level is
// that level.
// DETERMINISM: integer nanometres (geom2d), candidates by height in a std::map, the faces in the kernel's order.
#pragma once

#include "../geom2d.h"

#include <ray/cam.h>

#include <functional>
#include <vector>

namespace ray::cam::ops {

inline constexpr Nm kFaceMeetNm = 5'000; // a face meets an area where they overlap in a part wider than 10 um

// The part's material just above z (geom2d::sliceRegions' rule), from the caller's cache.
using SliceAt = std::function<Result<const geom2d::Regions*>(Nm z)>;

struct AreaDepthsInput {
    const kernel::Shape* part = nullptr;
    Xform frame;               // ResolvedGeometry::wcsFromModel
    geom2d::Regions footprint; // pocket2d.h THE POCKET
    geom2d::Region floor;      // the floor face
    Nm top = 0;                // candidates lie below it
    Nm low = 0;                // and above this (the lowest roughing depth)
    Nm stockFloor = 0;         // stock to leave on the floor
    Nm keep = 0;               // the distance the tool centre keeps from the walls (>= reach)
    Nm reach = 0;              // the tool radius
    std::vector<Nm> levels;    // the operation's own depths, top down (a repeated last depth is kept)
};

struct AreaDepths {
    std::vector<Nm> depths;                // top down
    std::vector<geom2d::Regions> region;   // per depth: the area
    std::vector<geom2d::Regions> material; // per depth: the material at it and above
    std::vector<Nm> unreached;             // candidate heights that added no depth, top down
};

Result<AreaDepths> areaDepths(const AreaDepthsInput& in, const SliceAt& sliceAt, JobContext& ctx);

} // namespace ray::cam::ops
