// Closed-region detection (task T-091): the full result behind sketch::closedLoops. Private to src/sketch.
//
// WHAT A REGION IS. The non-construction curves of a sketch (lines, arcs, circles, splines and the two lines and two
// arcs of each slot) are cut where they cross or touch each other, and points closer than kJoinTolerance are joined.
// Every bounded face of the resulting planar drawing is one region: an outer loop and the loops of whatever lies
// inside it (holes). An island inside a hole is a region of its own, and so is the ring between the hole and the
// island. Two faces that share an edge are two regions that both use it. Curves that do not bound any face (open
// chains, dangling ends, a line that only links two separate loops, duplicates of another curve) are reported, not
// used.
//
// HOW closedLoops RETURNS THEM. sketch.h returns a flat std::vector<kernel::ProfileLoop>, so regions are encoded by
// order and direction: each region is its outer loop, counter-clockwise, followed directly by its holes, each
// clockwise. regionsOf() below splits such a list back into regions; kernel::Profile takes one region as its loops.
// Regions come in ascending order of the outer loop's tag.
//
// TAGS. ProfileCurve::tag is curveTag(entity): the low 64 bits (the first 8 bytes, little-endian) of the BLAKE3 hash
// of the entity id's 16 bytes. The four curves of a slot get slotPartTag(slot, part) instead (parts: 0 the side from
// a to b, 1 the cap around b, 2 the side from b to a, 3 the cap around a) so that each side face of an extruded slot
// names one curve. A curve cut into pieces gives every piece the same tag. ProfileLoop::tag hashes the sorted list
// of (curve tag, travels along the entity's own direction) of the loop's pieces, so it changes only when the curves
// that bound the loop change, not when they move. The direction keeps apart the three faces that two crossing
// circles make from the same two curves. Where two loops still get the same tag, the one with the larger signed
// area (then the one further right, then up) gets a counter mixed in.
//
// CURVES THE KERNEL CANNOT TAKE. A spline is passed whole when kernel::ProfileCurve::Bspline is that same curve,
// otherwise as its Bezier segments, each exact (see loops_curves.h). A spline of degree 4 or more has no exact form
// in kernel::ProfileCurve, so a region bounded by one is rejected (listed in `rejected`; closedLoops then fails with
// that message rather than leave a region out). A spline's interior is never cut: where another curve crosses or
// touches a spline away from its end points the spline is left out and reported.
#pragma once

#include <ray/sketch.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ray::sketch {

// Points closer than this (mm) are one point; the kernel's own joint tolerance for profiles.
inline constexpr double kJoinTolerance = 1e-6;

std::uint64_t curveTag(SketchEntId entity);
std::uint64_t slotPartTag(SketchEntId slot, int part);

struct LoopRegion {
    kernel::ProfileLoop outer;              // counter-clockwise
    std::vector<kernel::ProfileLoop> holes; // clockwise
    double area = 0;                        // of the region (outer minus holes), mm^2
};

struct OpenCurve {
    SketchEntId entity;
    enum class Why {
        OpenChain,     // part of a chain that does not close (includes a dangling end on a closed loop)
        Bridge,        // links two loops (or a loop to an outer one) without bounding any face
        Duplicate,     // lies on top of another curve, which is used instead
        SplineCrossed, // a spline that another curve crosses or touches away from its end points
        Degenerate     // no length, or (an arc) its ends closer than kJoinTolerance: use a circle
    } why;
};

struct LoopAnalysis {
    std::vector<LoopRegion> regions;   // ascending outer.tag
    std::vector<OpenCurve> open;       // sorted by entity id, one entry per entity and reason
    std::vector<std::string> rejected; // regions that cannot be passed to the kernel exactly, with the reason
    std::vector<std::string> warnings; // open curves and rejected regions in words, for a log or a panel
};

// The analysis behind closedLoops. InvalidArgument for a sketch that fails validateSketch.
Result<LoopAnalysis> analyzeLoops(const Sketch&);

// Splits the flat closedLoops list back into regions: each starts at a counter-clockwise loop.
std::vector<std::vector<kernel::ProfileLoop>> regionsOf(const std::vector<kernel::ProfileLoop>& loops);

// Twice the signed area enclosed by a profile loop given in chain order and direction (positive counter-clockwise).
// Exact for lines and arcs, Gauss-Legendre for B-splines.
double loopSignedArea2(const kernel::ProfileLoop& loop);

} // namespace ray::sketch
