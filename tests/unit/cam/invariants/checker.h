// The toolpath invariant checker (task T-058; PLAN.md section 11, "Toolpath invariants"). Test-only code that shares
// nothing with the generators: not src/cam (geom2d, link, the operations), not Clipper2, not src/sim or src/post. It
// reads cam.h's types, meshes the part and fixture shapes through the public kernel API (triangulateExact, nothing
// else) and does all its geometry on its own grids. README.md in this folder has the list of invariants with their
// error bounds, what the checker cannot see, and how an operation type is plugged into the property tests.
//
// ONE ENTRY POINT: check(CheckInput) runs every invariant and returns every finding in move order. The M2 checkers
// (checkCycles in cycle_invariants.h, checkToolpath in toolpath_invariants.h) run inside it unchanged; their rule
// names are the ids of their findings.
//
// HOW A CONTACT IS MEASURED. The tool is a solid of revolution (tool_profile.h) in three zones (flutes, shank,
// holder). A material point at radius d from the tool axis and height g above the tip, inside the solid, penetrates
// it by min(radius(g) - d, g - (bottom of the solid's run at that radius)): its horizontal and vertical depth inside
// the tool, each at least its true (normal) depth, equal to it on a flat floor and on a vertical wall. Stock to leave
// is applied by growing the protected material by stockToLeaveWall in XY and stockToLeaveFloor in Z.
//
// PART CONTACTS (gouge, shank, holder against the part, rapidPart) are conservative with refinement: each grid square
// gets a lower and an upper bound of the largest penetration (part_model.h, conservative Z-sets; the square's nearest
// and farthest distance to the tool's path). Upper bound <= allowed: no contact there (pass). Lower bound > allowed:
// a real contact (fail). Otherwise the square is split in four, down to CheckOptions::minCell; a square still undecided
// there is reported (on the side of safety). So a pass means the true penetration into the meshed part is at most the
// allowed value, and a fail means it exceeds allowed - minCell * sqrt(2). The mesh lies within the chord tolerance
// of the true surface, so against the real part: pass => at most allowed + chord, fail => more than
// allowed - minCell * sqrt(2) - chord. Chorded XZ/YZ arcs add their sagitta (0.1 um) to both.
//
// STOCK CONTACTS (fluteLength, holder against stock, rapidStock, plunge, rampAngle, helix, engagement, coverage,
// stockLeft) are point samples on the stock grid (stock_grid.h): every report is real at a sample point; a contact
// deeper than stockCell * sqrt(2) cannot be missed; coverage and stockLeft hold at every sample.
#pragma once

#include "motion.h"
#include "part_model.h"
#include "stock_grid.h"
#include "tool_profile.h"

#include <ray/cam.h>
#include <ray/libraries.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ray::cam::invariants {

struct Finding {
    std::string id;       // the invariant (README.md lists them)
    std::size_t move = 0; // index into Toolpath::moves; moves.size() for the toolpath as a whole
    Vec3 at{};            // WCS mm where it was found
    double measured = 0;  // what was found, in the unit of `allowed` (mm, degrees, nm, rpm or a count)
    double allowed = 0;   // the limit it broke
    std::string message;  // one line
};
std::string describe(const Finding&);
std::string describe(const std::vector<Finding>&, std::size_t max = 20);

enum class Direction : std::uint8_t { Any, Climb, Conventional };

// A region of the XY plane (WCS mm): the stock box, or even-odd loops.
struct Region {
    bool stockBox = true;
    std::vector<std::vector<Vec2>> loops;
    static Region box() { return Region{}; }
    static Region of(std::vector<std::vector<Vec2>> l) { return Region{false, std::move(l)}; }
    bool contains(double x, double y, const Box3& stock) const;
    double boundaryDistance(double x, double y, const Box3& stock) const; // to the nearest edge
};

// "The region is cleared": at every sample of the region the tool can reach (a disc of the cutting radius that fits
// the free space around the part, stock to leave included, two samples of margin), no stock remains above
// floorZ + allowedAbove. id "coverage" (a scallop limit) or "stockLeft" (a finish claim on a floor).
struct ClearClaim {
    Region region;
    double floorZ = 0;
    double allowedAbove = 0;
    std::string id = "coverage";
};
// "The walls are finished": at samples of the region the tool can reach whose distance to the part (material in
// [zLo, zHi]) is between stockToLeaveWall + bound and that plus the cutting radius, no stock remains in [zLo, zHi].
struct WallClaim {
    Region region;
    double zLo = 0, zHi = 0;
    double bound = 0.05;
};

// "No move outside containment plus tolerance" (PLAN.md section 11): the tool's centre on every feed move stays
// inside the region grown by `tolerance` (sampled along the path every min(tolerance, 0.05) mm, exactly at ends).
struct ContainClaim {
    Region region;
    double tolerance = 0.01;
};

struct AbsHeights { // absolute WCS mm, any of them may be unknown
    std::optional<double> clearance, retract, feedPlane, top, bottom;
};

struct Claims {
    double spindleRpm = 0;   // Operation::cutting.spindleRpm
    bool clockwise = true;   // Operation::cutting.clockwise: what the tool needs (every library tool is right hand)
    double stickOut = 0;     // ToolRef::stickOut (0: see makeProfile)
    double tolerance = 0.01; // allowed penetration of the part (and of the stock-to-leave envelope), mm
    double stockToLeaveWall = 0, stockToLeaveFloor = 0;
    double holderClearance = 0;        // the holder keeps this far from part and stock
    double rapidClearance = 0;         // rapids keep this far from part and stock
    std::optional<double> rapidFloorZ; // M2 rapidLow (default: the stock top)
    std::optional<double> floorZ;      // M2 belowFloor (default: none)
    AbsHeights heights;                // heights; clearance also bounds the start and end
    Direction direction = Direction::Any;
    double directionBand = 0.5; // a cut is along a wall when the part is within cutting radius + stock to leave + this
    std::optional<bool> centreCutting; // default: the tool's stated Tool::centreCutting, else centreCuttingByDefault
    double deadZoneFraction = 0.25;    // a non-centre-cutting tool cannot cut within this fraction of its radius
    std::optional<double> maxRampAngleDeg, helixMinDiameter, maxEngagementDeg;
    InitialStock initialStock = InitialStock::Box;
    std::vector<AllowedCut> allowedCuts;
    std::vector<ClearClaim> clears;
    std::vector<WallClaim> walls;
    std::optional<ContainClaim> containment;
    Nm arcRadiusTolNm = 2;          // start and end radius of an arc agree within this
    double leadTangentTolDeg = 0.1; // lead-in and lead-out joints
    Vec3 wcsInMachine{};            // M2 travel: machine coordinates of the WCS origin
};

// The claims every operation shares: spindle and stick-out from the operation, the stock top as the rapid floor.
Claims claimsFor(const Operation& op, const ResolvedGeometry& geom);
bool centreCuttingByDefault(lib::ToolType);

struct CheckOptions {
    double partCell = 0.25;   // the part grid, mm
    double minCell = 0.001;   // the smallest square refinement goes to, mm
    double chord = 0.001;     // the part mesh's chord tolerance, mm
    double stockCell = 0;     // the stock grid, mm; 0: max(0.1, the stock's longer side / 1500)
    double rampStepMm = 0.02; // sloped pieces are cut into steps of at most this much Z for the stock
};

struct CheckInput {
    const Toolpath& path;
    const ResolvedGeometry& geom;
    const lib::Tool& tool;
    const lib::Holder* holder = nullptr;
    const lib::MachineDef* machine = nullptr;
    Claims claims;
};

// Every finding, in move order (ties: id). `part` may be passed in to reuse a mesh (PartModel::build of the same
// geometry with options.partCell and options.chord); otherwise it is built here. A part that does not mesh is a
// finding "mesh" (move moves.size()).
std::vector<Finding> check(const CheckInput& in, const CheckOptions& options = {},
                           std::shared_ptr<const PartModel> part = nullptr);

// The error bounds the options give (README.md, "Error bounds").
struct ErrorBounds {
    double partBelow = 0; // a reported part contact may be this much under the allowed value
    double partAbove = 0; // a part contact this much over the allowed value may pass
    double stockMiss = 0; // a stock contact shallower than this may be missed
    double stockCell = 0;
};
ErrorBounds errorBounds(const CheckOptions& options, const Box3& stock);

} // namespace ray::cam::invariants
