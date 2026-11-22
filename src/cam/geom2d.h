// Private to src/cam (task T-050): the shared 2D geometry core every milling operation stands on. Integer nanometre
// paths made of lines and circular arcs; boundaries from the model; offsets and booleans through Clipper2 with arcs
// kept exact; deterministic arc fitting of polylines; measures and nesting; conversion to toolpath moves.
//
// WHO USES WHAT (the operation tasks of PLAN.md M3/M4):
//   T-051 linking   length, pointAt, tangentAt, subPath, startAt, reverse, polygonize (collision and stock tests),
//                   locate (is a link inside uncut stock), appendMoves (spans -> Move records, arcs split at
//                   kMaxMoveSweep), Bounds.
//   T-052 face      silhouetteRegions (or the stock box as a Region: rectangle), offset (stepover rings or the
//                   overhang), intersect/subtract to clip passes to the stock, sweptArea + subtract for the coverage
//                   check.
//   T-053 contour   chainEdges (open and closed chains, tangent extension), faceBoundary, orientForTool + toolSideFor
//                   (climb/conventional), offset / offsetOpen by tool radius + stock (arcs exact to 1 nm), the
//                   un-offset chain for compensation in control, subPath for tabs, fitArcs never needed (arcs stay).
//   T-054 pocket    pocketAboveFace or faceBoundary + sliceRegions, offset repeatedly (rings; islands merging with the
//                   wall come out of offset itself), unite with a strip across open edges (offsetOpen/sweptArea),
//                   nest, area (stop when a ring vanishes), sweptArea + subtract for the uncut-area check.
//   T-055 slot      chainEdges (centre line), sweptArea; chamfer: offset/offsetOpen at the tip offset, sliceRegions
//                   below the chamfer for the tip collision check.
//   T-056 engrave   fromLoops / chainEdges for sketch curves; fitArcs for font polylines; threadmill: circles only
//                   (Path::circle) and appendMoves' helical arcs.
//   T-060/T-061     fromLoops/sliceRegions in, fitArcs on the engine's polylines (tolerance and thresholds below), out
//   adaptive        to Paths.
// The independent toolpath invariant checker (tests/unit/cam/invariants) must not use this file.
//
// UNITS AND LIMITS. Coordinates are integer nanometres (ray::Nm). Every coordinate, centre and radius must lie within
// +-kMaxCoord (1 km); functions that take user geometry check this and return InvalidArgument. Every predicate that
// decides topology (orientation, side of a line, crossing) is computed exactly in 128-bit integers; the remaining
// geometry (intersections of lines and circles, arc sweeps) is IEEE double arithmetic with ray::dmath, rounded to the
// nanometre by round-half-even (roundNm). src/cam is built with strict floating point (no contraction, no fast math),
// so every result is bit-identical on every platform.
//
// DETERMINISM. No function depends on thread timing, unordered containers, locale or the clock. Outputs are in
// canonical form (canonicalize): closed paths start at their lowest vertex (x, then y); regions are sorted by the
// lower-left corner of their outer path's bounds, then its lowest vertex, then area; holes likewise. The text dump (toText) is the golden-file format.
#pragma once
#include <ray/cam.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ray::cam::geom2d {

using Coord = Nm;

// ---------- Tolerances and thresholds (the rationale of each value is in geom2d.cpp, "TOLERANCES") ----------
inline constexpr Coord kMaxCoord = 1'000'000'000'000;  // 1 km
inline constexpr Coord kJoinTolerance = 1'000;          // 1 um: curve ends closer than this are joined (fromLoops)
inline constexpr Coord kOffsetSag = 100;                // 0.1 um: arc-to-chord sag Clipper2 sees (topology only)
inline constexpr Coord kFitTolerance = 1'000;           // 1 um: default fitArcs tolerance
inline constexpr Coord kMinArcChord = 10'000;           // 10 um: no fitted arc is shorter (chord) than this
inline constexpr Coord kMaxArcRadius = 10'000'000'000;  // 10 m: no fitted arc is larger than this
inline constexpr double kMaxFitSweep = 3.141592653589793238462643383279502884;   // 180 degrees per fitted arc
inline constexpr double kMaxMoveSweep = 3.141592653589793238462643383279502884;  // appendMoves splits longer arcs

// ---------- Types ----------
struct Pt {
    Coord x = 0, y = 0;
    auto operator<=>(const Pt&) const = default;
};
enum class SpanKind : std::uint8_t { Line, ArcCCW, ArcCW };
// One piece of a path. It starts where the previous span ended (or at Path::start) and ends at `end`.
// Arcs: `centre` and `radius` are exact (an offset arc's radius is the source radius plus the offset, exactly); the
// endpoints are rounded points and lie within 1 nm of the circle (fitArcs: within its tolerance). An arc whose end equals its start is a whole circle
// (sweep 360 degrees); every other arc sweeps less than 360 degrees in its direction.
struct Span {
    SpanKind kind = SpanKind::Line;
    Pt end;
    Pt centre;           // arcs only; {0, 0} for lines
    Coord radius = 0;    // arcs only; 0 for lines
    auto operator<=>(const Span&) const = default;
};
struct Path {
    Pt start;
    std::vector<Span> spans;
    bool closed = false;   // closed: the last span ends at `start`
    bool operator==(const Path&) const = default;
    static Path circle(Pt centre, Coord radius, bool ccw);   // one whole-circle span starting at (cx + r, cy)
};
// A region of the plane: the outer boundary counter-clockwise, the holes clockwise, the material (the region) on the
// left of every path. Holes lie inside the outer path and do not cross each other; an island inside a hole is a Region
// of its own.
struct Region {
    Path outer;
    std::vector<Path> holes;
    bool operator==(const Region&) const = default;
};
using Paths = std::vector<Path>;
using Regions = std::vector<Region>;

struct Bounds {
    Pt min, max;
    bool empty = true;
};

// ---------- Small helpers ----------
Coord roundNm(double nm);              // round half to even; saturates at +-kMaxCoord * 4
Pt toPt(Vec2 mm);                      // toNm of each coordinate
Vec2 toVec2(Pt);                       // mm
Coord toCoord(double mm);              // toNm
Status checkRange(const Path&);        // InvalidArgument if a coordinate or radius is beyond kMaxCoord
double sweepOf(Pt start, const Span& arc);   // radians in (0, 2 pi], 0 for lines

// ---------- Conversion from and to the kernel's profiles ----------
// A closed ProfileLoop (curves in any order and direction; ends meeting within kJoinTolerance after rounding) as one
// closed path in the order the curves chain from the first curve, in the first curve's direction. Bspline curves are
// refused (InvalidArgument): the kernel never returns them, and a caller with one approximates it first. The round
// trip toLoop(fromLoop(l)) moves each point by at most 0.5 nm (rounding); fromLoop(toLoop(p)) == p.
Result<Path> fromLoop(const kernel::ProfileLoop&);
kernel::ProfileLoop toLoop(const Path&);   // a closed path; one whole-circle span becomes a Circle curve
// Loops of any orientation and order that do not cross (a slice, a sketch's regions) -> regions by even-odd nesting
// (nest), every path oriented and canonical. Polls ctx.cancel per loop.
Result<Regions> regionsFromLoops(std::span<const kernel::ProfileLoop>, JobContext&);
std::vector<kernel::ProfileLoop> toLoops(const Regions&);

// ---------- Boundaries from the model (kernel calls; `frame` is ResolvedGeometry::wcsFromModel) ----------
// Every result is in the frame's XY in nanometres, heights in frame Z rounded to nanometres.
// The material just above height z (kernel::sliceAt's rule), as regions.
Result<Regions> sliceRegions(const kernel::Shape&, const Xform& frame, Coord z, JobContext&);
// The outline of the shape's shadow along frame Z (kernel::silhouette), as regions.
Result<Regions> silhouetteRegions(const kernel::Shape&, const Xform& frame, JobContext&);
// A planar face perpendicular to frame Z: its outer boundary and holes (the footprints of islands rising from it) at
// its height. InvalidArgument for any other face.
struct FaceBoundary {
    Region region;
    Coord z = 0;
    bool facesUp = true;
};
Result<FaceBoundary> faceBoundary(const kernel::Shape&, kernel::SubId face, const Xform& frame);
// The pocket a floor face belongs to: the empty area just above the floor that contains it, bounded by the walls
// (the hole of the slice at the floor height that contains the floor), with the islands standing in it as holes.
// The floor's height is `z`. InvalidArgument when the floor is not enclosed by walls (an open pocket: use faceBoundary
// and treat its edges as open) or is not a face looking up.
struct Pocket {
    Region region;
    Coord z = 0;
};
Result<Pocket> pocketAboveFace(const kernel::Shape&, kernel::SubId floorFace, const Xform& frame, JobContext&);

// Chains from picked edges, projected along frame Z (kernel::edgeCurves). Edges along Z are skipped. Ends that meet
// within kJoinTolerance join; at a point where more than two picked edges meet the chains stop (they are not guessed).
// With tangentExtend, each open end is continued through the shape's other edges at that vertex while exactly one of
// them continues tangentially (within tangentAngleDeg) and is not along Z; a branch stops the extension.
// Direction: a closed chain is counter-clockwise and starts at its lowest vertex; an open chain runs from its lower
// end (x, then y) to the other. Use orientForTool to put the tool on the side the operation needs.
// Chains are sorted by their start point. Edges must belong to `shape`. Polls ctx.cancel per edge.
struct ChainOptions {
    bool tangentExtend = false;
    double tangentAngleDeg = 0.5;
};
struct Chain {
    Path path;
    Coord zMin = 0, zMax = 0;             // height range of the edges
    std::vector<kernel::SubId> edges;     // in path order, extensions included (closed: cyclic, any first edge)
};
Result<std::vector<Chain>> chainEdges(const kernel::Shape&, std::span<const kernel::SubId> edges, const Xform& frame,
                                      const ChainOptions&, JobContext&);

// ---------- Direction and side ----------
enum class Side : std::uint8_t { Left, Right };   // relative to the direction of travel
enum class CutMode : std::uint8_t { Climb, Conventional };
// The side of the path the tool travels on for this cut mode: with the spindle turning clockwise (M3) climb milling
// keeps the tool on the left of the material edge (G41), conventional on the right; counter-clockwise the reverse.
Side toolSideFor(CutMode, bool spindleClockwise);
// `p` with the tool on `want`: `current` is the side the tool is on for p as given; reversed when they differ.
Path orientForTool(Path p, Side current, Side want);
Path reverse(const Path&);
// For a closed path: the side of travel the region it encloses lies on (Left for counter-clockwise).
Side insideSide(const Path& closed);

// ---------- Measures ----------
double area(const Path&);       // signed nm^2, counter-clockwise positive (a closed path; arcs exact)
double area(const Region&);     // outer minus holes, >= 0
double area(const Regions&);
double length(const Path&);     // nm
Bounds bounds(const Path&);     // arcs' extreme points included
Bounds bounds(const Region&);
enum class Where : std::uint8_t { Outside, Inside, OnBoundary };
// Where a point is relative to a closed path (the area it encloses, either orientation) or a region. Exact for lines;
// for arcs exact to the rounding of the point to the circle (a point within 0.5 nm of an arc is OnBoundary).
Where locate(Pt, const Path& closed);
Where locate(Pt, const Region&);
Where locate(Pt, const Regions&);
// A point strictly inside the region (not on any boundary), deterministic; nullopt for an empty region.
std::optional<Pt> interiorPoint(const Region&);

// ---------- Nesting ----------
// For closed paths that do not cross: parent[i] = the index of the smallest path that encloses path i, or -1, and
// depth[i] = the number of paths enclosing it. Box-pruned (sorted sweep over x), so n loops cost about n log n box
// tests plus one point test per enclosing candidate. Polls ctx.cancel.
struct Nesting {
    std::vector<int> parent;
    std::vector<int> depth;
};
Result<Nesting> nest(std::span<const Path> closed, JobContext&);
// Regions from such paths by even-odd depth: even depth -> outer (made counter-clockwise), its odd-depth children ->
// holes (made clockwise). Canonical.
Regions regionsByNesting(std::span<const Path> closed, const Nesting&);

// ---------- Offsets and booleans (Clipper2, arcs exact) ----------
// HOW (geom2d_clip.cpp): every result is first built as the exact curves it must consist of (the offset of each line
// and arc, an arc of radius |delta| about each convex corner), these are flattened with sag kOffsetSag and tagged
// with the curve they came from (Clipper2's Z), Clipper2 decides the topology (which pieces survive, how they join),
// and the result is rebuilt from the exact curves: every surviving piece is a line or an arc of exact centre and
// radius, ending at the exact intersection of its neighbours, rounded to the nanometre. So an offset is exact to
// 1 nm (an arc of radius r offset by d has radius r + d exactly), and only the topology is decided at kOffsetSag.
// A piece of a result curve that Clipper2 keeps shorter than kOffsetSag may be dropped or kept by that decision.
//
// Regions grown (delta > 0) or shrunk (delta < 0) by |delta| with round joins: the set of points within |delta| of
// the region (grow) or the points of the region at least |delta| from its outside (shrink). Shrinking a pocket by the
// tool radius gives where the tool centre may go; an island too close to the wall merges with it, and a pocket the
// tool does not fit gives no region. delta 0 returns the regions canonicalised. Polls ctx.cancel.
Result<Regions> offset(const Regions&, Coord delta, JobContext&, Coord sag = kOffsetSag);
// The tool-centre path of an open chain: the side `side` of the boundary of the area swept by a disc of radius d > 0
// moved along the path, without the end caps, i.e. the path offset to that side by d with round joins at its convex
// corners and trimmed where the offset of one part of the path comes closer than d to another part. Pieces in path
// order (more than one where the trimming cuts it). For a closed path use offset on its region.
Result<Paths> offsetOpen(const Path& open, Coord d, Side side, JobContext&, Coord sag = kOffsetSag);
// The area a disc of radius r > 0 covers moving along the paths (open or closed): the cut area of a tool, for coverage
// and collision checks.
Result<Regions> sweptArea(std::span<const Path> paths, Coord r, JobContext&, Coord sag = kOffsetSag);
Result<Regions> unite(const Regions& a, const Regions& b, JobContext&);
Result<Regions> intersect(const Regions& a, const Regions& b, JobContext&);
Result<Regions> subtract(const Regions& a, const Regions& b, JobContext&);
// Diagnostics: how many polygon edges the rebuild had to keep literally (no exact curve within 2 sag + 4 nm) since
// the process started. The tests require it to stay 0 on their cases.
std::size_t literalEdgeCount();

// ---------- Arc fitting ----------
// Lines and arcs through a polyline (the adaptive engine's passes, sampled curves, font strokes), deterministic:
// greedily from the first point, each span covers as many points as it can (exponential then binary search on the
// end index) while every point is within `tolerance` of it, every polyline edge is within `tolerance` of it, and the
// points progress monotonically along it; a line is preferred when it covers as many points. An arc must also
//   - have a chord of at least minArcChord and a sagitta above `tolerance` (else the line is within tolerance),
//   - have a radius of at most maxArcRadius, an integer centre, and sweep at most maxSweep (so a control's R-form
//     or I/J-form arc is never ambiguous and never a whole circle by accident),
//   - turn the same way at every point (its direction is never ambiguous).
// The centre is the integer point near the least-squares centre that minimises the largest deviation (ties: the
// smallest x, then y); the radius is then rounded. Arcs sampled from a known integer arc come back as one arc with the
// same endpoints and direction and a centre and radius within a few nanometres (the property test measures it);
// exact reproduction of known arcs is what offset and the booleans do, through their tags, not this function.
struct FitOptions {
    Coord tolerance = kFitTolerance;
    Coord minArcChord = kMinArcChord;
    Coord maxArcRadius = kMaxArcRadius;
    double maxSweep = kMaxFitSweep;
};
Path fitArcs(std::span<const Pt> points, bool closed, const FitOptions& = {});

// ---------- Paths along their length ----------
// The points of a path: every span end, and arcs flattened so no chord is farther than `sag` from its arc; the
// vertices lie on the arcs (rounded). Starts with path.start; a closed path does not repeat it at the end.
std::vector<Pt> polygonize(const Path&, Coord sag);
Pt pointAt(const Path&, double s);       // the point at arc length s (clamped to [0, length]), rounded
// The unit tangent at arc length s in the direction of travel (at a corner: of the span that starts there).
Vec2 tangentAt(const Path&, double s);
// The part of a path between arc lengths s0 < s1 (for a closed path s may wrap past the length: s1 - s0 <= length).
Path subPath(const Path&, double s0, double s1);
// A closed path started at arc length s (for choosing an entry point). The point is rounded; the path is otherwise
// unchanged (a span is split there).
Path startAt(const Path& closed, double s);

// ---------- Canonical form ----------
// Removes zero-length spans, merges consecutive collinear lines (exactly collinear: integer cross product 0, same
// direction) and consecutive arcs of the same circle and direction (when their sweep stays below 360 degrees), and for
// a closed path starts it at its lowest vertex. Regions: outer counter-clockwise, holes clockwise, sorted.
Path canonicalize(Path);
Region canonicalize(Region);
Regions canonicalize(Regions);

// ---------- Moves ----------
// Appends the spans of `p` as Linear / ArcCW / ArcCCW moves at height z (XY arcs, centre absolute in i, j; k = z),
// role and feed as given, arcs longer than kMaxMoveSweep split into equal parts (each end rounded on the circle).
// The first move goes from wherever the tool is to... nothing: the caller positions the tool at p.start first.
void appendMoves(const Path& p, Coord z, MoveRole role, std::int32_t feedMicronPerMin, std::vector<Move>& out);

// ---------- Golden-file text ----------
// One line per path: "P <closed 0|1> <x> <y>" then one line per span: "L x y" or "A+ x y cx cy r" (counter-clockwise)
// or "A- x y cx cy r"; regions as "R" then "O" (outer) and "H" (hole) path blocks. Integers only, '\n' line ends.
std::string toText(const Path&);
std::string toText(const Region&);
std::string toText(const Regions&);

} // namespace ray::cam::geom2d
