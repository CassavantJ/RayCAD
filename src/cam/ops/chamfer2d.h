// The "chamfer2d" operation type (task T-055), private to src/cam and its tests: chamfering and deburring level edges
// with the flank of a pointed tool (a chamfer mill or a spot drill) at a chosen chamfer width and tip offset, the tool
// proved clear of the walls and floors below and beside the edge. Parameters: schemas/ops/chamfer2d.schema.json.
// docs/operation_types.md describes the pattern of an operation type.
//
// GEOMETRY. The selected elements are edges; geom2d::chainEdges joins them (and, with the param tangentEdges, the edges
// that continue them tangentially) into chains in the setup's XY. Each chain must be level (its height range within
// kLevelTol): its height is the edge height E. The material side: at the middle of every span of the chain, the
// points kSideProbe to its left and to its right are located in the part's material just below the edge (sliceRegions
// at E - kSideDepth); exactly one of them must be in material, the same side all along the chain (else the edge is not
// the top edge of a wall: refused, naming where).
//
// THE TOOL is a cone: tip radius rt (tipDiameter / 2), half angle a / 2 (tipAngleDeg / 2), t = tan(a / 2), up to the
// cutting radius R (diameter / 2), on up to the flute length, then the shank (mill::ToolShape). A chamfer of width w
// (param width: the chamfer's width across the top face) made by that flank is c = w / t deep (a 90 degree tool makes a
// 45 degree chamfer, c = w). With the tip offset o (param tipOffset: how far the tip goes below the chamfer's lower
// edge, so the cut is made by the flank, not the tip), the tool's axis runs at
//   X = rt + o t     (rounded half to even to nm) beside the edge, on its air side, and the tip at
//   Z = E - c - o    (c rounded once: toNm(w / t)).
// At height h above the tip the flank is rt + h t from the axis: at the chamfer's lower edge (h = o) it meets the wall
// (X - rt - o t = 0), at the top face (h = c + o) it is w inside it. The cone must reach that far: X + w <= R (else
// refused: a larger tool, a smaller tip offset or a narrower chamfer); the flutes must be at least c + o long; the
// stick-out must keep the holder kHolderClearanceMm above the stock and the part at the tip's depth.
// Centre paths: a closed chain with the material inside it (an island, the outside of a part): the chain's region grown
// by X (geom2d::offset, round joins at its corners); with the material outside it (a pocket, a hole): shrunk by X, one
// path per region the shrinking leaves (none: refused, the tool does not fit); an open chain: geom2d::offsetOpen to its
// air side by X, extended straight along its end tangents by the param extension. X = 0 (a sharp tool with no tip
// offset) runs on the chain itself.
// Direction (param direction): the tool side geom2d::toolSideFor(climb or conventional, M3 spindle); a centre path is
// reversed when the tool is on the other side of the material edge.
//
// THE COLLISION PROOF (refused when it fails, naming where and at what height): at every height z the tool passes
// through below the stock top, the part's material just above z (sliceRegions), within the tool's radius at that height
// of the centre paths (geom2d::sweptArea), must lie inside what the chamfers are meant to remove: the material within
// (z - (E - c)) t + kBandTol of a picked chain at heights E - c <= z < E (the chamfer's section; kBandTol alone below
// it). So the tip and the tool's body below the chamfer touch no wall and no floor, and above it nothing stands in the
// cone's or the shank's way. Heights: the tip Z, every kChamferProbeStep up to E, every kChamferProbeAbove from E up to
// the highest point of the stock, parts and fixtures (mill::highestNm), E - c, E - kSideDepth, and every planar face
// perpendicular to Z in that range with 1 um below it (where the outline of a prismatic part changes and the tool there
// is at its widest). Every part body and fixture body is checked. Between those heights a sloped or curved wall is
// seen only at the heights sampled.
//
// LEVELS. Heights are relative to the stock top (slot_common.h): clearance, retract, feed plane; top and bottom are
// not read. PASSES: one per centre path at the tip Z, feed feedCut, closed ones rotatable (linking picks the start),
// none reversible (the direction is the cut mode). Linking: entry {Plunge} (the descent beside the edge, on the air
// side; chamfer mills and spot drills cut at their centre, as the invariant checker also assumes), feedPlunge (0: the
// cut feed), maxStayDown 0 (every link retracts to the retract plane).
//
// VALIDATE runs the same plan as generate (planChamfer) and returns its first error.
#pragma once

#include "../geom2d.h"
#include "../link.h"

#include <ray/cam.h>

#include <memory>
#include <string>
#include <vector>

namespace ray::cam::ops {

inline constexpr Nm kLevelTol = 1'000;             // 1 um: a chain is level when its height range is within this
inline constexpr geom2d::Coord kSideProbe = 5'000; // 5 um beside the edge for the material side
inline constexpr Nm kSideDepth = 5'000;            // 5 um below the edge for the material side
inline constexpr geom2d::Coord kBandTol = 2'000;   // 2 um: the chamfer's section is grown by this for the proof
inline constexpr geom2d::Coord kMinToolPathRadius = 1'000; // 1 um: below this the tool's disc is a point
inline constexpr Nm kChamferProbeStep = 100'000;           // 0.1 mm between the heights proved below the edge
inline constexpr Nm kChamferProbeAbove = 1'000'000;        // 1 mm between the heights proved above it

// The chamfer's numbers for a tool (CHAMFER above): its depth c, the axis offset X, the tip Z below the edge (c + o).
struct ChamferGeometry {
    Nm depth = 0;           // c
    geom2d::Coord axis = 0; // X
    Nm tipBelow = 0;        // c + o
};
// InvalidArgument when the tool is not pointed.
Result<ChamferGeometry> chamferGeometry(const lib::Tool& tool, double widthMm, double tipOffsetMm);

struct PlannedChain {
    std::size_t part = 0;
    geom2d::Chain chain;
    Nm edgeZ = 0;
    geom2d::Side air = geom2d::Side::Left; // the side of the chain's direction the air is on
    std::vector<geom2d::Path> centres;     // the tool's centre paths, in cut direction
};

struct ChamferPlan {
    ChamferGeometry geometry;
    Nm width = 0;
    std::vector<PlannedChain> chains;
    std::vector<link::Pass> passes;
    link::Options options;
    link::Stock stock;
    std::vector<std::string> warnings;
};

Result<ChamferPlan> planChamfer(const GenerateInput& in, JobContext& ctx);
Result<Toolpath> chamferToolpath(const ChamferPlan& plan, const lib::MachineDef& machine, JobContext& ctx);

std::unique_ptr<IOperationType> makeChamfer2dOperationType();

} // namespace ray::cam::ops
