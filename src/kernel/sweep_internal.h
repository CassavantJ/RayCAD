// Private to src/kernel (task T-094): turning a kernel::Profile into a planar OCCT face for extrude and revolve.
// Includes OCCT headers; nothing outside src/kernel may include it (PLAN.md 0.4 rule 1).
//
// HOW A PROFILE IS READ. kernel.h does not spell out how ProfileCurve::pts encodes each kind; this is the
// convention (2D points are in the plane's frame: x along Plane::xDir made square to the normal, y = normal x x):
//   Line     pts = {start, end}.
//   Arc      pts = {start, end, centre}; from start to end counter-clockwise about the plane normal if `ccw`,
//            clockwise otherwise. `radius` may be 0; if set it must equal |start - centre|.
//   Circle   pts = {centre}, `radius` > 0. A circle is a loop on its own.
//   Bspline  pts = control points (at least 2) of a clamped, uniform, non-rational B-spline of degree
//            min(3, count - 1); it starts at the first point and ends at the last.
// The curves of a loop are given in chain order. Each one may run either way: a curve is turned round when that
// makes it start where the previous one ends. Joints must meet within 1e-6 mm, and the last curve must end where
// the first starts. Loop direction does not matter (the outer loop is made counter-clockwise about the normal and
// holes clockwise). The first loop is the outer boundary; every other loop is a hole inside it. Holes may not
// overlap, touch, or lie inside each other (an island inside a hole is a separate profile).
#pragma once
#include "shape_impl.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt2d.hxx>

#include <cstdint>
#include <string_view>
#include <vector>

namespace ray::kernel::detail {

struct ProfileEdge {
    TopoDS_Edge edge;       // as built; the face holds the same TShape
    std::uint64_t curveTag; // ProfileCurve::tag
    std::uint64_t loopTag;  // ProfileLoop::tag of its loop
};

struct BuiltProfile {
    TopoDS_Face face;               // planar, normal = the profile plane's normal
    std::vector<ProfileEdge> edges; // one per profile curve, loop by loop
    std::uint64_t outerTag = 0;     // tag of the first loop: the region the caps belong to
    gp_Ax3 frame;                   // origin, normal (Z) and x direction of the profile plane
    std::vector<gp_Pnt2d> samples;  // points along every loop, in the frame's 2D coordinates
};

// Builds and checks the face. InvalidArgument (with the loop and curve numbers, 1-based) for a profile that does
// not follow the convention above, is open, crosses itself, or has holes outside the boundary. May throw.
Result<BuiltProfile> buildProfile(const Profile& profile, std::string_view where);

} // namespace ray::kernel::detail
