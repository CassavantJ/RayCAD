// Curve arithmetic shared by loop detection and DXF exchange (task T-091): 2D vector helpers, Bezier segments and
// B-spline knot insertion. Private to src/sketch. Deterministic: only + - * / and ray::dmath.
//
// WHAT A SKETCH SPLINE IS. sketch.h stores a spline as control points, a degree and a `closed` flag and names no
// knots. This module reads it as a non-rational B-spline with a uniform knot vector:
//   closed = false   clamped uniform: degree + 1 equal knots at each end, the interior knots evenly spaced, so the
//                    curve starts at the first control point and ends at the last;
//   closed = true    periodic uniform: the control points are taken cyclically (the first `degree` of them repeat
//                    after the last) with evenly spaced knots, giving a closed curve with one span per control point
//                    that is as smooth at its seam as anywhere else.
// The kernel's ProfileCurve::Bspline is the clamped uniform curve of degree min(3, n - 1), so a sketch spline is
// passed to it whole when it is open and its degree is min(3, n - 1), and otherwise as its Bezier segments, each of
// which is a clamped uniform curve of its own degree (exact for degree 3 or less).
#pragma once

#include <ray/core.h>

#include <optional>
#include <vector>

namespace ray::sketch::detail {

inline Vec2 operator+(Vec2 a, Vec2 b)
{
    return {a.x + b.x, a.y + b.y};
}
inline Vec2 operator-(Vec2 a, Vec2 b)
{
    return {a.x - b.x, a.y - b.y};
}
inline Vec2 operator*(double s, Vec2 a)
{
    return {s * a.x, s * a.y};
}
inline double dot(Vec2 a, Vec2 b)
{
    return a.x * b.x + a.y * b.y;
}
inline double cross(Vec2 a, Vec2 b)
{
    return a.x * b.y - a.y * b.x;
}
inline double norm(Vec2 a)
{
    return dmath::hypot(a.x, a.y);
}
inline double dist(Vec2 a, Vec2 b)
{
    return dmath::hypot(a.x - b.x, a.y - b.y);
}
inline Vec2 perpLeft(Vec2 a)
{
    return {-a.y, a.x};
}
inline Vec2 lerp(Vec2 a, Vec2 b, double t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// A point of a rational curve in homogeneous form (x*w, y*w, w).
struct HPoint {
    double x = 0, y = 0, w = 1;
};

// ---------- Bezier segments (degree = number of points - 1) ----------
Vec2 bezierPoint(const std::vector<Vec2>& pts, double t);
// The first derivative with respect to t.
Vec2 bezierDerivative(const std::vector<Vec2>& pts, double t);
// The direction in which the segment leaves its first point (atEnd = false) or arrives at its last point (atEnd =
// true): the first non-zero control-point difference from that end. {0, 0} if every point is the same.
Vec2 bezierEndDirection(const std::vector<Vec2>& pts, bool atEnd);
// Signed curvature at the start (atEnd = false) or end, for the direction of travel from first to last point.
double bezierEndCurvature(const std::vector<Vec2>& pts, bool atEnd);
// The integral of (x dy - y dx) along the segment: twice its contribution to the signed area of a closed curve.
// Exact (Gauss-Legendre) up to degree 10; nullopt above.
std::optional<double> bezierAreaTerm(const std::vector<Vec2>& pts);
// The segment split at t (de Casteljau): {first part, second part}.
std::pair<std::vector<Vec2>, std::vector<Vec2>> bezierSplit(const std::vector<Vec2>& pts, double t);

// ---------- B-splines ----------
// Clamped uniform knots for n control points of degree p (n + p + 1 values from 0 to 1).
std::vector<double> clampedUniformKnots(int n, int p);
// Bezier segments of a B-spline with control points `poles` (homogeneous), knots `knots` (n + p + 1 values, non-
// decreasing) and degree p >= 1, over its domain [knots[p], knots[n]]. Works for clamped and unclamped knot vectors.
// Knots closer than 1e-12 of the domain length count as one. nullopt for an inconsistent input or an empty domain.
std::optional<std::vector<std::vector<HPoint>>> bezierDecompose(std::vector<HPoint> poles, std::vector<double> knots,
                                                                int p);
// The Bezier segments of a sketch spline (see the comment at the top of this file).
std::vector<std::vector<Vec2>> sketchSplineBeziers(const std::vector<Vec2>& controlPoints, int degree, bool closed);
// True when the kernel's Bspline (clamped uniform, degree min(3, n - 1)) is exactly this sketch spline.
bool kernelTakesWhole(std::size_t controlPointCount, int degree, bool closed);

} // namespace ray::sketch::detail
