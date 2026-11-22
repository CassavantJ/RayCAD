// Private to src/kernel (task T-050): what sliceAt, silhouette, faceLoops and edgeCurves share. Turns OCCT edges
// and faces into ProfileCurves in a frame's XY and puts loops into the order kernel.h promises. Includes OCCT headers;
// nothing outside src/kernel may include it (PLAN.md 0.4 rule 1).
//
// CURVES. An edge becomes ProfileCurves in the XY plane of the (already moved) shape, projected along Z:
//   - a line stays a Line (a line along Z projects to a point and gives nothing);
//   - a circle whose axis is parallel to Z (within kParallelSin) stays an Arc, or a Circle when the edge is closed;
//   - any other curve (an ellipse, a circle in a tilted plane, a B-spline, an intersection curve) is sampled at
//     kRecogniseSamples parameters: if every sample lies within kCurveTolerance of the line, or of the circle
//     through its start, middle and end, it becomes that Line or Arc (OCCT sometimes returns a circle as a B-spline,
//     e.g. where a torus is cut perpendicular to its axis); otherwise it becomes Lines through points of the curve,
//     split until every chord is within kCurveTolerance of the curve at its quarter, half and three-quarter
//     parameters (at least kMinPolylineSplits halvings, so a curve that leaves its chord and comes back is not missed).
// Endpoints are taken from the edge's vertices, so consecutive edges of a wire meet exactly.
//
// LOOPS. orientLoop makes a loop counter-clockwise seen from +Z (outer) or clockwise (hole) by its signed area,
// startLoop starts it at its lowest vertex (x, then y, each rounded to integer nanometres, so the choice does not
// depend on the last bits), and orderRegions sorts regions and holes by that lowest vertex. Loops whose area is
// below kMinLoopArea (a face of zero width where the slice plane touches a tangent face) are dropped.
#pragma once
#include "shape_impl.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>

#include <string_view>
#include <vector>

namespace ray::kernel::detail {

inline constexpr double kCurveTolerance = 5e-4;     // mm: approximation of curves that are not lines or arcs (0.5 um)
inline constexpr double kParallelSin = 1e-9;        // an axis is parallel to Z when |axis x Z| <= this
inline constexpr int kRecogniseSamples = 64;        // samples when checking whether a curve is a line or an arc
inline constexpr int kMinPolylineSplits = 2;        // a polyline approximation halves each curve at least this often
inline constexpr int kMaxPolylineSplits = 24;       // and at most this often (2^24 chords)
inline constexpr double kMinLoopArea = 1e-8;        // mm^2: loops below this are degenerate and dropped
inline constexpr double kDegenerateLength = 1e-9;   // mm: a projected curve shorter than this gives nothing

// The rigid move a frame describes (kernel.h boundsIn rules: finite, orthonormal within 1e-9, not mirrored).
Result<gp_Trsf> rigidTrsf(const Xform& frame, std::string_view where);

// A copy of the shape's topology (new TShapes, geometry shared) for the OCCT algorithms that write into their
// arguments (boolean_internal.h PrivateCopy says which). May throw.
TopoDS_Shape topologyCopy(const TopoDS_Shape& shape);

// Appends the curves of `edge` projected along Z into XY, in the edge's direction, or against it when `reversed`.
// May throw.
void appendProjectedEdge(const TopoDS_Edge& edge, bool reversed, std::vector<ProfileCurve>& out);

// The loops of a face's wires projected along Z, outer wire first, each in wire order (not yet oriented or started).
// May throw.
std::vector<ProfileLoop> faceWireLoops(const TopoDS_Face& face);

// Signed area in XY (counter-clockwise positive), arcs and circles included.
double loopArea(const ProfileLoop& loop);
// Reverses the direction of a loop.
void reverseLoop(ProfileLoop& loop);
// Counter-clockwise (ccw true) or clockwise; returns false if the loop is degenerate (|area| < kMinLoopArea).
bool orientLoop(ProfileLoop& loop, bool ccw);
// Starts the loop at its lowest vertex.
void startLoop(ProfileLoop& loop);

// One region: an outer loop and its holes, oriented and started.
struct LoopRegion {
    ProfileLoop outer;
    std::vector<ProfileLoop> holes;
};
// Orients and starts every loop, drops degenerate ones (a region whose outer is degenerate goes with its holes) and
// flattens the regions into kernel.h's order: each outer followed by its holes, regions and holes by lowest vertex.
std::vector<ProfileLoop> orderRegions(std::vector<LoopRegion> regions);

// The faces of a planar face set in a region list: outer wire -> outer, other wires -> holes. May throw.
std::vector<LoopRegion> regionsOfFaces(const std::vector<TopoDS_Face>& faces);

} // namespace ray::kernel::detail
