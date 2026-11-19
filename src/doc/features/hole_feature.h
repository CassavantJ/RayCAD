// The "hole" feature type (task T-097), private to src/doc and its tests.
//
// Props: {"sketch": "<sketch feature id>",
//         "points"?: ["<point entity id>", ...],  the hole centres; all free-standing points of the sketch when absent
//         "target": "<body id>",                  the body drilled
//         "flip"?: false,                         false: drill against the sketch normal (into the face the sketch
//                                                 is on, whose normal points out of the material); true: along it
//         "kind": "simple" | "tapped" | "counterbore" | "countersink" | "counterbore_tapped" | "countersink_tapped",
//         "diameter"?: <length>,                  the drilled diameter; required unless tapped (a tapped hole is
//                                                 drilled at thread.tapDrill and must not give it)
//         "end": "blind" | "through",
//         "depth"?: <length>,                     blind: depth of the full-diameter bore from the sketch plane
//         "bottom"?: "angle" | "flat",            blind: drill point (default) or flat bottom
//         "tipAngle"?: <angle, default 118 deg>,  blind, "angle": included angle of the drill point
//         "counterbore"?: {"diameter": <length>, "depth": <length>},           counterbore kinds
//         "countersink"?: {"diameter": <length>, "angle"?: <angle, 90 deg>},   countersink kinds
//         "thread"?: {"designation": "M6x1", "standard": "iso_metric_coarse" | "iso_metric_fine" | "unc" | "unf" |
//                     "custom", "nominalDiameter": <length>, "pitch": <length> | "tpi": <number>, "class"?: "6H",
//                     "depth"?: <length, default the whole tapped length>, "tapDrill": <length>}}   tapped kinds
// Lengths and angles are numbers (mm, radians) or expressions over the document parameters. Options the kind or end
// does not use (a depth on a through hole, a counterbore on a simple hole) are kept and ignored, so switching back
// restores them. Counterbore and countersink sizes are entered by the user: RayCAD ships no screw-head tables.
//
// POINTS. A free-standing point is a point entity that is not construction geometry and is not an end, centre or
// control point of a curve. Listed points are looked up by id on every rebuild, so a hole moves with its point and
// keeps its identity through sketch edits; a listed point that is no longer in the sketch (or is not a point) fails
// the feature with a message naming it, as a Broken reference does (PLAN.md 3.3). Without "points", deleting a point
// simply removes its hole.
//
// BUILD. For each point, in order: the half-section of the hole (top, counterbore wall and floor, countersink cone,
// wall, flat bottom or drill point, and the axis) is revolved a full turn about the hole axis (kernel::revolve), named
// by naming::propagate from its sweep origins with the role "hole" (each section line carries a tag made from the
// point's id and the line's part, so the wall of one hole is "hole.side" named from <point, wall> whatever the other
// holes do), then cut from the target and the result named by propagate over the body's and the tool's maps, as an
// extrude cut is. A through hole runs flat-ended 1 mm past the far side of the body's bounding box. Tapped holes are
// modelled at the tap-drill diameter; the thread is not modelled, its spec is stored in the props for CAM (T-097b,
// holeThreadOf). The feature writes `target`.
#pragma once

#include "feature_props.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ray::doc::features {

inline constexpr std::string_view kHoleType = "hole";

std::unique_ptr<IFeatureType> makeHoleFeatureType();
std::string_view holePropsSchema();

// The thread of a tapped hole, as CAM reads it (T-097b). Lengths in mm.
struct HoleThread {
    std::string designation;                // "M6x1", "1/4-20 UNC"
    std::string standard;                   // "iso_metric_coarse", "iso_metric_fine", "unc", "unf", "custom"
    std::optional<std::string> threadClass; // "6H", "2B"
    double nominalDiameter = 0;
    double pitch = 0;          // mm (25.4 / tpi for a thread given in threads per inch)
    std::optional<double> tpi; // when the props give threads per inch
    double tapDrill = 0;
    std::optional<double> depth; // nullopt: the whole tapped length of the hole
};

// The thread spec of a hole feature with its expressions evaluated over `params` (RebuildContext::paramValues);
// nullopt for a hole of a kind that is not tapped. InvalidArgument naming the field when the props are bad.
Result<std::optional<HoleThread>> holeThreadOf(const Feature& hole, const std::map<std::string, double>& params);

} // namespace ray::doc::features
