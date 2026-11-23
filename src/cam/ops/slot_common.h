// Helpers shared by the "slot" and "chamfer2d" operation types (task T-055), private to src/cam and its tests: words
// for messages, the spindle, feed and height checks both types make, the material test along the tool's height that
// both use to prove the tool clears the part, and the tool's radius at a height above its tip.
//
// HEIGHTS of both types are relative to the top of the stock (ResolvedGeometry::stockBounds.max.z): clearance, retract
// and feed plane always, and the slot's top. They must be ordered clearance >= retract >= feed plane (>= top), and the
// retract plane must not be below the stock top (linking rapids across at it).
#pragma once

#include "../geom2d.h"
#include "../link.h"

#include <ray/cam.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ray::cam::ops::mill {

using geom2d::Coord;

inline constexpr double kRadPerDeg = 0.017453292519943295; // pi / 180, rounded to double
inline constexpr double kHolderClearanceMm = 1.0;          // the holder stays this far above the stock top

tl::unexpected<Error> failure(ErrorCode code, std::string message);
tl::unexpected<Error> invalid(std::string message);

// A length in mm for messages: rounded to 1 um (half to even), trailing zeros dropped ("12.5", "0.002", "-3").
std::string mmText(Nm nm);
std::string mmText(double mm);
std::string atXY(geom2d::Pt p);           // "X12.5 Y3"
std::string atXYZ(geom2d::Pt p, Nm z);    // "X12.5 Y3 Z-2"
const char* toolWord(lib::ToolType type); // "flat end mill"

// The spindle: a speed set, within the machine's range, turning clockwise (every library tool is right hand).
Status checkSpindle(const GenerateInput& in);
// A feed in mm/min as um/min (rounded half to even): > 0 and within int32. `what` names it ("cutting feed"). With
// `zeroIsCut`, 0 (or less) gives 0, which linking reads as "the cut feed".
Result<std::int32_t> feedUm(double mmPerMin, std::string_view what, bool zeroIsCut);
// HEIGHTS above: finite and ordered; `withTop` adds feed plane >= top.
Status checkHeights(const Heights& h, bool withTop);

// The top of the stock in WCS nanometres.
Nm stockTopNm(const ResolvedGeometry& g);
// The highest WCS Z of the stock, the part bodies and the fixture bodies (their boxes), in nanometres.
Nm highestNm(const ResolvedGeometry& g);

// The heights in [lo, hi] at which the material of `shape` can change its outline for a prismatic part: the planar
// faces perpendicular to frame Z (their frame Z, rounded to nm), sorted and unique. Polls ctx.cancel per face.
Result<std::vector<Nm>> faceHeights(const kernel::Shape& shape, const Xform& frame, Nm lo, Nm hi, JobContext& ctx);

// The tool as a solid of revolution, radius at a height above its tip (nm): the cutting part (a cone from the tip
// diameter at the tip angle up to the diameter, for pointed tools; the diameter for flat ones) up to the flute length,
// then a neck the tool states (libraries.h neckDiameter, up to neckLength, else the shoulder; after M3), then the shank
// (its diameter, else the cutting diameter). The holder is not part of it.
struct ToolShape {
    Coord radius = 0;     // cutting radius
    Coord tipRadius = 0;  // radius at the tip (pointed tools)
    double tanHalf = 0;   // tan(tip angle / 2) for pointed tools, 0 for flat ones
    Nm fluteLength = 0;   // 0: the cutting part has no stated length (it goes on up)
    Coord neckRadius = 0; // 0: no neck
    Nm neckTop = 0;       // the neck's top above the tip
    Coord shankRadius = 0;
    Coord radiusAt(Nm heightAboveTip) const;
};
ToolShape toolShape(const lib::Tool& t);

// The part material the tool would remove outside `allowed` at height z: the slice of `shape` just above z
// (sliceRegions) intersected with `swept` (the area the tool's disc covers at that height, geom2d::sweptArea of its
// centre paths), minus `allowed` (nullopt: nothing is allowed). Empty when the tool clears the part there. Regions of
// area below kContactMinArea count as none (rounding slivers along a boundary the tool runs on).
inline constexpr double kContactMinArea = 25e6; // nm^2: 25 um^2, a 5 x 5 um patch
Result<geom2d::Regions> contactAt(const kernel::Shape& shape, const Xform& frame, Nm z, const geom2d::Regions& swept,
                                  const std::optional<geom2d::Regions>& allowed, JobContext& ctx);
// The same with the slice given: `material` intersected with `swept`, minus `allowed`, slivers dropped.
Result<geom2d::Regions> contactWith(const geom2d::Regions& material, const geom2d::Regions& swept,
                                    const std::optional<geom2d::Regions>& allowed, JobContext& ctx);
// A point to name a contact by: inside its first region (its lowest vertex if it has no interior point).
geom2d::Pt contactPoint(const geom2d::Regions& contact);

// The span check against the machine's travel where it gives one (the work offset is not known here, so only the
// extent of the moves): X, Y and Z spans from the toolpath start and every move end.
Status checkTravel(const std::vector<Move>& moves, Nm startZ, const lib::MachineDef& machine);

// Toolpath statistics from linked moves.
void fillStats(Toolpath& path, const link::Linked& linked);

} // namespace ray::cam::ops::mill
