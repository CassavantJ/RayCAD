// The "transform_body" feature type (PLAN.md 3.2, task T-077), private to src/doc and its tests.
//
// It moves one body rigidly. Its main use is placing a fixture (a vise, parallels, a fixture plate imported with the
// role "fixture") against the part, but it moves any body.
//
// Props (canonical JSON; numbers are mm or rad, or expression strings over the document parameters):
//   {"body": "<body uuid>",                                       required: the body moved
//    "mate": {"face": <TopoRef>, "target": <TopoRef>},            optional: a planar face of the body and a planar face
//                                                                 of another body
//    "gap": <length>,                                             optional, default 0; only with "mate"
//    "angle": <angle>,                                            optional, default 0
//    "tiltX": <angle>, "tiltY": <angle>,                          optional, default 0; only without "mate"
//    "x": <length>, "y": <length>, "z": <length>}                 optional, default 0
// (PLAN.md 3.2 sketched the props as `body` and `xform`; a stored matrix could not follow the part, so the placement is
// stored as the mate and the numbers a user types.)
//
// THE MOVE, in this order:
//   1. Mate (with "mate"): the body turns and moves so that its face lies on the target face, the two coincident and
//      OPPOSED (outward normals opposite: the faces touch, as a vise jaw touches the side of a part), with the centre
//      of the face (its area centroid) on the centre of the target face. The turn keeps "up" up where it can: for a
//      face that is not within 45 degrees of horizontal, the direction of model +Z projected onto the face goes onto
//      the same direction of the target face; for a face within 45 degrees of horizontal, model +X projected does. A
//      face whose turn would be undefined is never met (those directions always exist).
//   2. Gap: the body moves `gap` along the target face's outward normal (positive: the faces stand apart; a jaw
//      opening, the height of a parallel, the stock allowance on that side).
//   3. Angle: the body turns by `angle` about the target face's outward normal through its centre (counter-clockwise
//      seen from outside the target face); without a mate, about model +Z through the model origin. Without a mate
//      the body may first be tilted (added after T-077, for a vise placed on a setup's stock whose Z is not model +Z,
//      a part turned over for its second side): by `tiltX` about model +X, then by `tiltY` about model +Y, both
//      through the model origin, then by the angle about +Z (the rotation Rz(angle) Ry(tiltY) Rx(tiltX)). A tilt
//      with a mate is refused: the faces set the orientation. Without tilts the move is exactly what it was.
//   4. X, Y, Z: the body moves by (x, y, z) along the model axes.
// The references are resolved where the feature stands in the history: the face on the body as the body is just before
// this feature, the target on its body as it is then. So a mate follows every edit of the features before it (a part
// made wider moves the vise with its face); features after it do not move it. A reference that does not resolve usably
// (Broken, an unconfirmed repair), a face that is not planar, a face on the wrong body or a target on the body itself
// fails the feature with a message naming which; Split pieces on one plane are used as one face (centre: the
// area-weighted centre of the pieces) with a warning, as are Merged faces.
//
// Deterministic (the doc module's rule): normals are rounded per component to a multiple of 2^-40 and snapped onto a
// model axis when within 1e-9 of it, centres are rounded to integer nanometres, trigonometry is ray::dmath, and matrix
// entries within 1e-15 of 0 or +-1 are made exact, so a quarter turn is an exact quarter turn.
#pragma once

#include <ray/document.h>

#include <memory>
#include <optional>
#include <string_view>

namespace ray::doc::features {

inline constexpr std::string_view kTransformBodyType = "transform_body";

std::unique_ptr<IFeatureType> makeTransformBodyFeatureType();
std::string_view transformBodyPropsSchema();

// A planar face as the mate reads it: the centre of its area and its outward unit normal (model frame).
struct MateFace {
    Vec3 centre;
    Vec3 normal;
};

// The placement as the props give it, evaluated (mm, rad).
struct Placement {
    std::optional<MateFace> face, target; // both or neither
    double gap = 0, angle = 0;
    Vec3 offset;
    double tiltX = 0, tiltY = 0; // only without a mate
};

// The rigid move of steps 1 to 4 above (model frame, row-major 3x4 as kernel::transformed takes it). Pure arithmetic,
// exposed for tests and the fixture panel.
Xform placementTransform(const Placement& placement);

// The same rounding the feature applies to the normals and centres it reads from the kernel (exposed for tests).
Vec3 roundedNormal(Vec3 n);
Vec3 roundedPoint(Vec3 p);

} // namespace ray::doc::features
