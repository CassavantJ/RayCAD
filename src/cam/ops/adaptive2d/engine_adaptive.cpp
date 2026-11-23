// SPDX-License-Identifier: LGPL-2.1-or-later

/**************************************************************************
 *   Copyright (c) 2018 Kresimir Tusek <kresimir.tusek@gmail.com>          *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

// MODIFIED FILE. This is FreeCAD's src/Mod/CAM/libarea/Adaptive.cpp from FreeCAD main at commit
// 56062b61c7085af5af12ead4c02ce5765d0107c4, changed for RayCAD by the RayCAD contributors in November 2026 (task
// T-060, PLAN.md section 13 "M4"). The unmodified upstream text is this file's first version in the repository's
// history, so `git diff <that commit> -- src/cam/ops/adaptive2d/engine_adaptive.cpp` shows every change. Changes:
//  1. Determinism. Every decision that read the clock (clock()) is gone: ResolveLinkPath gave up after 0.5 s of wall
//     time and now after Limits::linkSearchSteps segments (and Limits::linkSideSteps sideways probes); progress was
//     reported every 0.1 s and is now reported once per pass. Trigonometry goes through ray::dmath (identical on every
//     platform) and std::pow through a cube root made of IEEE operations. Sorts whose keys can tie break the tie by a
//     second key or are stable (std::sort may order equal keys differently on another standard library).
//  2. Integers. `long` (32 bits on Windows, 64 elsewhere) is std::int64_t everywhere; every conversion of a double
//     to a grid coordinate rounds (Rnd: half away from zero, as Clipper2 does) instead of truncating, and saturates a
//     value that is not finite or beyond +-2^52 instead of being undefined; engine.cpp checks the range of every
//     input and output coordinate. Output paths stay in grid units (upstream converted them to millimetre doubles
//     and back to integers by truncation to grow the cleared area).
//  3. Re-entrancy. The nine file-scope PerfCounter objects (and the class) are gone; no state is shared between
//     instances. The SVG debug writer, which wrote a file into the current folder, and the DEV_MODE code are gone.
//  4. Defined results. Every output field is initialised; the uninitialised locals of upstream (closest-path indices,
//     the helix radius, pointNotInterp) are initialised; directions that could become NaN (a zero-length average
//     direction, the arcsine of a zero-length step, the direction of a zero-length segment) are guarded.
//  5. Reporting. Console output (std::cout, std::cerr) is gone; what it said is in the output flags (engine.cpp turns
//     them into warnings or errors). New output: path roles, the engine's estimate of the radial depth of cut per
//     segment, the helix radius, the tool-bound area and the cleared area of each region, counters (Stats).
//  6. Cancel and limits. The cancel token is polled at every step of every loop (the step search, the passes, the
//     engagement search, link and lead search, the entry search, finishing); hard limits on passes, points, steps
//     and leads end a region with LimitReached instead of running on (upstream: LONG_MAX).
//  7. Interface. Inputs arrive in grid units (engine.cpp converts from nanometres); the progress callback that
//     received copies of the paths is replaced by the cancel token and a progress fraction.
//  8. One bug fix: the area a pass cleared (and the cleared-area totals) is the sum of Clipper2's signed areas;
//     upstream signed each loop by a nesting test that misjudged loops, so passes that cut thousands of square
//     millimetres were dropped as cutting nothing (on S5's islands pocket: links from where the tool never was,
//     material cut later far above the target engagement). The depth estimates of item 5 query the cleared area
//     without touching its cache, so they change nothing the step search computes.
//  9. Defensive guards against endless loops: a finishing path of zero length, and empty paths where upstream read
//     the first point or looped to size() - 1.
// 10. Offsets (OffsetPaths, OffsetPath, ClipperOffset) use engine_offset.h, a copy of Clipper2's offsetting with its
//     trigonometry through ray::dmath (Clipper2 itself calls the platform's libm for round joins). The library's own
//     Union, Intersect and Difference stay; ports/clipper2 patches its one unstable sort, which did make Linux and
//     Windows differ on S5's thin-wall pocket.
// The algorithm itself (the step search, the cut-area computation, engagement, linking, leads, finishing) is
// upstream's, statement for statement, apart from these changes.
// clang-format off

#include "engine_adaptive.h"
#include "engine_offset.h"
#include <cstddef>
#include <cmath>
#include <cstring>
#include <climits>
#include <algorithm>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <tuple>

// RayCAD: upstream's code converts between int64_t and double and reuses names in inner scopes on purpose; those
// warnings are silenced for this file only, rather than rewriting upstream's lines.
#ifdef _MSC_VER
# pragma warning(disable : 4244 4456 4457)
#endif

namespace ray::cam::adaptive2d::fork
{
using namespace std;
using namespace Clipper2Lib;
#define SAME_POINT_TOL_SQRD_SCALED 4.0
#define UNUSED(expr) (void)(expr)

// RayCAD: a double to a grid coordinate. Rounds half away from zero (as Clipper2's Point64(double, double) does);
// a value that is not finite or beyond +-2^52 saturates (engine.cpp rejects any output outside the input's bounds).
inline int64_t Rnd(double v)
{
    constexpr double kMax = 4503599627370496.0;  // 2^52
    if (!(v == v)) {
        return 0;
    }
    if (v >= kMax) {
        return int64_t(kMax);
    }
    if (v <= -kMax) {
        return -int64_t(kMax);
    }
    return int64_t(std::round(v));
}

// RayCAD: the cube root of x >= 0 from IEEE operations only (upstream: std::pow(x, 1 / 3.)), so it is the same on
// every platform. Newton's iteration from a power-of-two estimate; converges to within an ulp or two.
inline double CubeRoot(double x)
{
    if (!(x > 0)) {
        return 0;
    }
    int e = 0;
    (void)std::frexp(x, &e);  // x = m 2^e, m in [0.5, 1)
    double y = std::ldexp(1.0, e / 3);
    for (int i = 0; i < 100; i++) {
        double next = y - (y * y * y - x) / (3 * y * y);
        if (next == y) {
            break;
        }
        y = next;
    }
    return y;
}

// Matches CleanPolygons default distance in Clipper1 (sqrt(2))
const double CLEAN_POLYGONS_TOLERANCE = 1.415;

// SimplifyPaths, then drops 2-point pancake paths
inline Paths64 SimplifyPathsFilterPancakes(const Paths64& paths, double epsilon)
{
    Paths64 result = SimplifyPaths(paths, epsilon);
    std::erase_if(result, [](const Path64& p) { return p.size() < 3; });
    return result;
}

// Wrappers that preserve Clipper1 default offset behavior (arc_tolerance=0.25 absolute)
inline Paths64 OffsetPaths(
    const Paths64& paths,
    double delta,
    EndType et = EndType::Polygon,
    JoinType jt = JoinType::Round
)
{
    return offset::InflatePaths(paths, delta, jt, et, 2.0, 0.25);  // RayCAD: engine_offset.h
}

inline Paths64 OffsetPath(
    const Path64& path,
    double delta,
    EndType et = EndType::Polygon,
    JoinType jt = JoinType::Round
)
{
    return offset::InflatePaths(Paths64 {path}, delta, jt, et, 2.0, 0.25);  // RayCAD: engine_offset.h
}

//*****************************************
// Utils - inline
//*****************************************

inline double DistanceSqrd(const Point64& pt1, const Point64& pt2)
{
    double Dx = double(pt1.x - pt2.x);
    double dy = double(pt1.y - pt2.y);
    return (Dx * Dx + dy * dy);
}

inline double averageDV(const vector<double>& vec)
{
    double s = 0;
    std::size_t size = vec.size();
    if (size == 0) {
        return 0;
    }
    for (std::size_t i = 0; i < size; i++) {
        s += vec[i];
    }
    return s / double(size);
}

inline PointD rotate(const PointD& in, double rad)
{
    double c = dmath::cos(rad);
    double s = dmath::sin(rad);
    return PointD(c * in.x - s * in.y, s * in.x + c * in.y);
}

// calculates path length for open path
inline double PathLength(const Path64& path)
{
    double len = 0;
    if (path.size() < 2) {
        return len;
    }
    for (size_t i = 1; i < path.size(); i++) {
        len += sqrt(DistanceSqrd(path[i - 1], path[i]));
    }
    return len;
}

inline PointD DirectionV(const Point64& pt1, const Point64& pt2)
{
    double DX = double(pt2.x - pt1.x);
    double DY = double(pt2.y - pt1.y);
    double l = sqrt(DX * DX + DY * DY);
    if (l < NTOL) {
        return PointD(0, 0);
    }
    return PointD(DX / l, DY / l);
}

inline void NormalizeV(PointD& pt)
{
    double len = sqrt(pt.x * pt.x + pt.y * pt.y);
    if (len > NTOL) {
        pt.x /= len;
        pt.y /= len;
    }
}

inline PointD GetPathDirectionV(const Path64& pth, size_t pointIndex)
{
    if (pth.size() < 2) {
        return PointD(0, 0);
    }
    const Point64& p1 = pth.at(pointIndex > 0 ? pointIndex - 1 : pth.size() - 1);
    const Point64& p2 = pth.at(pointIndex);
    return DirectionV(p1, p2);
}

// Returns true if points 'a' and 'b' are coincident or nearly so.
bool isClose(const Point64& a, const Point64& b)
{
    return abs(a.x - b.x) <= 1 && abs(a.y - b.y) <= 1;
}

// Remove coincident and almost-coincident points from paths.
void filterCloseValues(Paths64& ppg)
{
    for (auto& pth : ppg) {
        while (true) {
            auto i = std::adjacent_find(pth.begin(), pth.end(), isClose);
            if (i == pth.end()) {
                break;
            }
            pth.erase(i);
        }
        // adjacent_find doesn't compare first with last element, so
        // do that manually.
        while (pth.size() > 1 && isClose(pth.front(), pth.back())) {
            pth.pop_back();
        }
    }
}

//*****************************************
// Utils
//*****************************************

class BoundBox
{
public:
    BoundBox()
    {
        minX = 0;
        maxX = 0;
        minY = 0;
        maxY = 0;
    }

    // generic: first point
    BoundBox(const Point64& p1)
    {
        minX = p1.x;
        maxX = p1.x;
        minY = p1.y;
        maxY = p1.y;
    }

    void SetFirstPoint(const Point64& p1)
    {
        minX = p1.x;
        maxX = p1.x;
        minY = p1.y;
        maxY = p1.y;
    }

    // generic: subsequent points
    void AddPoint(const Point64& pt)
    {
        minX = min(pt.x, minX);
        maxX = max(pt.x, maxX);
        minY = min(pt.y, minY);
        maxY = max(pt.y, maxY);
    }

    // line segment: two points
    BoundBox(const Point64& p1, const Point64& p2)
    {
        if (p1.x < p2.x) {
            minX = p1.x;
            maxX = p2.x;
        }
        else {
            minX = p2.x;
            maxX = p1.x;
        }
        if (p1.y < p2.y) {
            minY = p1.y;
            maxY = p2.y;
        }
        else {
            minY = p2.y;
            maxY = p1.y;
        }
    }

    // for circle: center and radius
    BoundBox(const Point64& center, int64_t radius)
    {
        minX = center.x - radius;
        maxX = center.x + radius;
        minY = center.y - radius;
        maxY = center.y + radius;
    }

    // bounds check - intersection
    inline bool CollidesWith(const BoundBox& bb2)
    {
        return minX <= bb2.maxX && maxX >= bb2.minX && minY <= bb2.maxY && maxY >= bb2.minY;
    }

    // bounds check -  contains
    inline bool Contains(const BoundBox& bb2)
    {
        return minX <= bb2.minX && maxX >= bb2.maxX && minY <= bb2.minY && maxY >= bb2.maxY;
    }

    int64_t minX;
    int64_t maxX;
    int64_t minY;
    int64_t maxY;
};

int getPathNestingLevel(const Path64& path, const Paths64& paths)
{
    int nesting = 0;
    for (const auto& other : paths) {
        if (!path.empty() && PointInPolygon(path.front(), other) != PointInPolygonResult::IsOutside) {
            nesting++;
        }
    }
    return nesting;
}

void appendDirectChildPaths(Paths64& outPaths, const Path64& path, const Paths64& paths)
{
    int nesting = getPathNestingLevel(path, paths);
    for (const auto& other : paths) {
        if (!path.empty() && !other.empty()
            && PointInPolygon(other.front(), path) != PointInPolygonResult::IsOutside) {
            if (getPathNestingLevel(other, paths) == nesting + 1) {
                outPaths.push_back(other);
            }
        }
    }
}

void AverageDirection(const vector<PointD>& unityVectors, PointD& output)
{
    std::size_t size = unityVectors.size();
    output.x = 0;
    output.y = 0;
    // sum vectors
    for (std::size_t i = 0; i < size; i++) {
        PointD v = unityVectors[i];
        output.x += v.x;
        output.y += v.y;
    }
    // normalize
    double magnitude = sqrt(output.x * output.x + output.y * output.y);
    if (magnitude < NTOL) {
        // RayCAD: the directions cancel out (upstream divided by zero): keep the most recent one
        output = size > 0 ? unityVectors[size - 1] : PointD(0, 0);
        return;
    }
    output.x /= magnitude;
    output.y /= magnitude;
}

double DistancePointToLineSegSquared(
    const Point64& p1,
    const Point64& p2,
    const Point64& pt,
    Point64& closestPoint,
    double& ptParameter,
    bool clamp = true
)
{
    double D21X = double(p2.x - p1.x);
    double D21Y = double(p2.y - p1.y);
    double DP1X = double(pt.x - p1.x);
    double DP1Y = double(pt.y - p1.y);
    double lsegLenSqr = D21X * D21X + D21Y * D21Y;
    if (lsegLenSqr == 0) {  // segment is zero length, return point to point distance
        closestPoint = p1;
        ptParameter = 0;
        return DP1X * DP1X + DP1Y * DP1Y;
    }
    double parameter = DP1X * D21X + DP1Y * D21Y;
    if (clamp) {
        // clamp the parameter
        if (parameter < 0) {
            parameter = 0;
        }
        else if (parameter > lsegLenSqr) {
            parameter = lsegLenSqr;
        }
    }
    // point on line at parameter
    ptParameter = parameter / lsegLenSqr;
    closestPoint.x = Rnd(p1.x + ptParameter * D21X);
    closestPoint.y = Rnd(p1.y + ptParameter * D21Y);
    // calculate distance from point on line to pt
    double DX = double(pt.x - closestPoint.x);
    double DY = double(pt.y - closestPoint.y);
    return DX * DX + DY * DY;  // return distance squared
}

void ScaleUpPaths(Paths64& paths, int64_t scaleFactor)
{
    for (auto& pth : paths) {
        for (auto& pt : pth) {
            pt.x *= scaleFactor;
            pt.y *= scaleFactor;
        }
    }
}

void ScaleDownPaths(Paths64& paths, int64_t scaleFactor)
{
    for (auto& pth : paths) {
        for (auto& pt : pth) {
            pt.x = Rnd(double(pt.x) / double(scaleFactor));  // RayCAD: rounds (upstream truncated)
            pt.y = Rnd(double(pt.y) / double(scaleFactor));
        }
    }
}


double DistancePointToPathsSqrd(
    const Paths64& paths,
    const Point64& pt,
    Point64& closestPointOnPath,
    size_t& clpPathIndex,
    size_t& clpSegmentIndex,
    double& clpParameter
)
{
    double minDistSq = __DBL_MAX__;
    // RayCAD: defined outputs when there is no segment at all
    clpPathIndex = 0;
    clpSegmentIndex = 0;
    clpParameter = 0;
    closestPointOnPath = pt;
    // iterate though paths
    for (size_t i = 0; i < paths.size(); i++) {
        const Path64& path = paths[i];
        size_t size = path.size();
        // iterate through segments
        for (size_t j = 0; j < size; j++) {
            double ptPar;
            Point64 clp;
            double distSq
                = DistancePointToLineSegSquared(path[j > 0 ? j - 1 : size - 1], path[j], pt, clp, ptPar);
            if (distSq < minDistSq) {
                clpPathIndex = i;
                clpSegmentIndex = j;
                clpParameter = ptPar;
                closestPointOnPath = clp;
                minDistSq = distSq;
            }
        }
    }
    return minDistSq;
}

bool Circle2CircleIntersect(
    const Point64& c1,
    const Point64& c2,
    double radius,
    pair<PointD, PointD>& intersections
)
{
    double DX = double(c2.x - c1.x);
    double DY = double(c2.y - c1.y);
    double d = sqrt(DX * DX + DY * DY);
    if (d < NTOL) {
        return false;  // same center
    }
    if (d >= radius) {
        return false;  // do not intersect, or intersect in one point (this case not relevant here)
    }
    double a_2 = sqrt(4 * radius * radius - d * d) / 2.0;
    intersections.first
        = PointD(0.5 * (c1.x + c2.x) - DY * a_2 / d, 0.5 * (c1.y + c2.y) + DX * a_2 / d);
    intersections.second
        = PointD(0.5 * (c1.x + c2.x) + DY * a_2 / d, 0.5 * (c1.y + c2.y) - DX * a_2 / d);
    return true;
}

bool Line2CircleIntersect(
    const Point64& c,
    double radius,
    const PointD& p1,
    const PointD& p2,
    vector<PointD>& result,
    bool clamp = true
)
{
    // if more intersections returned, first is closer to p1
    double dx = double(p2.x - p1.x);
    double dy = double(p2.y - p1.y);
    double lcx = double(p1.x - c.x);
    double lcy = double(p1.y - c.y);
    double a = dx * dx + dy * dy;
    double b = 2 * dx * lcx + 2 * dy * lcy;
    double C = lcx * lcx + lcy * lcy - radius * radius;
    double sq = b * b - 4 * a * C;
    if (sq < 0) {
        return false;  // no solution
    }
    sq = sqrt(sq);
    double t1 = (-b - sq) / (2 * a);
    double t2 = (-b + sq) / (2 * a);
    result.clear();
    if ((t1 >= 0.0 && t1 <= 1.0) || !clamp) {
        result.emplace_back(p1.x + t1 * dx, p1.y + t1 * dy);
    }
    if ((t2 >= 0.0 && t2 <= 1.0) || !clamp) {
        result.emplace_back(p1.x + t2 * dx, p1.y + t2 * dy);
    }
    return !result.empty();
}

// calculate center point of polygon
Point64 Compute2DPolygonCentroid(const Path64& vertices)
{
    PointD centroid(0, 0);
    double signedArea = 0.0;
    double x0 = 0.0;  // Current vertex X
    double y0 = 0.0;  // Current vertex Y
    double x1 = 0.0;  // Next vertex X
    double y1 = 0.0;  // Next vertex Y
    double a = 0.0;   // Partial signed area

    // For all vertices
    size_t i = 0;
    Path64::size_type size = vertices.size();
    for (i = 0; i < size; ++i) {
        x0 = double(vertices[i].x);
        y0 = double(vertices[i].y);
        x1 = double(vertices[(i + 1) % size].x);
        y1 = double(vertices[(i + 1) % size].y);
        a = x0 * y1 - x1 * y0;
        signedArea += a;
        centroid.x += (x0 + x1) * a;
        centroid.y += (y0 + y1) * a;
    }

    signedArea *= 0.5;
    if (size == 0) {
        return Point64(0, 0);  // RayCAD: defined for an empty path
    }
    if (signedArea == 0) {
        return vertices[0];  // RayCAD: a polygon without area has no centroid (upstream divided by zero)
    }
    centroid.x /= (6.0 * signedArea);
    centroid.y /= (6.0 * signedArea);
    return Point64(Rnd(centroid.x), Rnd(centroid.y));
}

// point must be within first path (boundary) and must not be within all other paths (holes)
bool IsPointWithinCutRegion(const Paths64& toolBoundPaths, const Point64& point)
{
    bool inside = false;
    for (size_t i = 0; i < toolBoundPaths.size(); i++) {
        if (PointInPolygon(point, toolBoundPaths[i]) != PointInPolygonResult::IsOutside) {
            inside = !inside;
        }
    }
    return inside;
}

/* finds intersection of line segment with line segment */
bool IntersectionPoint(
    const Point64& s1p1,
    const Point64& s1p2,
    const Point64& s2p1,
    const Point64& s2p2,
    Point64& intersection
)
{
    double S1DX = double(s1p2.x - s1p1.x);
    double S1DY = double(s1p2.y - s1p1.y);
    double S2DX = double(s2p2.x - s2p1.x);
    double S2DY = double(s2p2.y - s2p1.y);
    double d = S1DY * S2DX - S2DY * S1DX;
    if (fabs(d) < NTOL) {
        return false;  // lines are parallel
    }

    double LPDX = double(s1p1.x - s2p1.x);
    double LPDY = double(s1p1.y - s2p1.y);
    double p1d = S2DY * LPDX - S2DX * LPDY;
    double p2d = S1DY * LPDX - S1DX * LPDY;
    if ((d < 0) && (p1d < d || p1d > 0 || p2d < d || p2d > 0)) {
        return false;  // intersection not within segment1
    }
    if ((d > 0) && (p1d < 0 || p1d > d || p2d < 0 || p2d > d)) {
        return false;  // intersection not within segment2
    }
    double t = p1d / d;
    intersection = Point64(Rnd(s1p1.x + S1DX * t), Rnd(s1p1.y + S1DY * t));
    return true;
}

/* finds one/first intersection of line segment with paths */
bool IntersectionPoint(const Paths64& paths, const Point64& p1, const Point64& p2, Point64& intersection)
{
    BoundBox segBB(p1, p2);
    for (size_t i = 0; i < paths.size(); i++) {
        const Path64& path = paths[i];
        size_t size = path.size();
        if (size < 2) {
            continue;
        }
        BoundBox pathBB(path.front());
        for (size_t j = 0; j < size; j++) {

            const Point64& pp2 = path[j];

            // box check for performance
            pathBB.AddPoint(pp2);
            if (!pathBB.CollidesWith(segBB)) {
                continue;
            }

            const Point64& pp1 = path[j > 0 ? j - 1 : size - 1];
            double LDY = double(p2.y - p1.y);
            double LDX = double(p2.x - p1.x);
            double PDX = double(pp2.x - pp1.x);
            double PDY = double(pp2.y - pp1.y);
            double d = LDY * PDX - PDY * LDX;
            if (fabs(d) < NTOL) {
                continue;  // lines are parallel
            }

            double LPDX = double(p1.x - pp1.x);
            double LPDY = double(p1.y - pp1.y);
            double p1d = PDY * LPDX - PDX * LPDY;
            double p2d = LDY * LPDX - LDX * LPDY;
            if ((d < 0) && (p1d < d || p1d > 0 || p2d < d || p2d > 0)) {
                continue;  // intersection not within segment
            }
            if ((d > 0) && (p1d < 0 || p1d > d || p2d < 0 || p2d > d)) {
                continue;  // intersection not within segment
            }
            double t = p1d / d;
            intersection = Point64(Rnd(p1.x + LDX * t), Rnd(p1.y + LDY * t));
            return true;
        }
    }
    return false;
}

void SmoothPaths(Paths64& paths, double stepSize, int64_t pointCount, int64_t iterations)
{
    Paths64 output(paths.size());
    const int64_t scale = 1000;
    const double stepScaled = stepSize * scale;

    ScaleUpPaths(paths, scale);
    vector<pair<size_t /*path index*/, Point64>> points;
    for (size_t i = 0; i < paths.size(); i++) {
        for (const auto& pt : paths[i]) {
            if (points.empty()) {
                points.emplace_back(i, pt);
                continue;
            }
            const auto back = points.back();
            const Point64& lastPt = back.second;

            const double l = sqrt(DistanceSqrd(lastPt, pt));

            if (l < 0.5 * stepScaled) {
                if (points.size() > 1) {
                    points.pop_back();
                }
                points.emplace_back(i, pt);
                continue;
            }
            size_t lastPathIndex = back.first;
            const int64_t steps = max(int64_t(l / stepScaled), int64_t(1));  // a count: floor, as upstream
            const int64_t left = pointCount * iterations * 2;
            const int64_t right = steps - pointCount * iterations * 2;
            for (int64_t idx = 0; idx <= steps; idx++) {
                if (idx > left && idx < right) {
                    idx = right;
                    continue;
                }
                const double p = double(idx) / double(steps);
                const Point64 ptx(
                    Rnd(lastPt.x + double(pt.x - lastPt.x) * p),
                    Rnd(lastPt.y + double(pt.y - lastPt.y) * p)
                );

                if (idx == 0 && DistanceSqrd(back.second, ptx) < scale && points.size() > 1) {
                    points.pop_back();
                }

                if (p < 0.5) {
                    points.emplace_back(lastPathIndex, ptx);
                }
                else {
                    points.emplace_back(i, ptx);
                }
            }
        }
    }
    if (points.empty()) {
        return;
    }
    const int64_t size = int64_t(points.size());
    for (int64_t iter = 0; iter < iterations; iter++) {
        for (int64_t i = 1; i < size - 1; i++) {
            Point64& cp = points[i].second;
            Point64 avgPoint(cp);
            int64_t cnt = 1;

            int64_t ptsToAverage = pointCount;
            if (i <= ptsToAverage) {
                ptsToAverage = max(i - 1, int64_t(0));
            }
            else if (i + ptsToAverage >= size - 1) {
                ptsToAverage = size - 1 - i;
            }
            for (int64_t j = i - ptsToAverage; j <= i + ptsToAverage; j++) {
                if (j == i) {
                    continue;
                }
                int64_t index = j;
                if (index < 0) {
                    index = 0;
                }
                if (index >= size) {
                    index = size - 1;
                }
                Point64& p = points[index].second;
                avgPoint.x += p.x;
                avgPoint.y += p.y;
                cnt++;
            }
            cp.x = Rnd(double(avgPoint.x) / double(cnt));  // RayCAD: rounds (upstream truncated)
            cp.y = Rnd(double(avgPoint.y) / double(cnt));
        }
    }

    for (const auto& pr : points) {
        output[pr.first].push_back(pr.second);
    }
    for (size_t i = 0; i < paths.size(); i++) {
        paths[i] = SimplifyPath(output[i], 1.4 * scale, false);
    }
    ScaleDownPaths(paths, scale);
}

// PopNextFinishingPass: Select and remove the next finishing pass to execute
//
// This function examines all available finishing paths (both closed and open) and selects
// the best one to execute next based on proximity to the current tool position (p1).
//
// Requirements:
//   - At least one finishing path must be available (either closed or open)
//
// Behavior:
//   - Searches through both closedFinishingPaths and openFinishingPaths to find the
//     closest point to the current tool position
//   - For CLOSED paths: Can start at any point along the path. The path is rotated
//     (shifted) so that execution begins at the closest point. The extraDistanceAround
//     parameter allows advancing further along the path before starting.
//   - For OPEN paths: Must start at the first vertex (index 0). No rotation is allowed.
//     These paths represent segments where only part of the geometry needs finishing.
//
// Return:
//   - true: A closed finishing path was selected and returned in 'result'. The path is
//     removed from closedFinishingPaths.
//   - false: An open finishing path was selected and returned in 'result'. The path is
//     removed from openFinishingPaths.
//   - If no paths remain in either list, the function returns false with an empty result.
//
bool PopNextFinishingPass(
    Paths64& closedFinishingPaths,
    Paths64& openFinishingPaths,
    Point64 p1,
    Path64& result,
    double extraDistanceAround = 0
)
{
    // Early return if no paths remain in either list
    if (closedFinishingPaths.empty() && openFinishingPaths.empty()) {
        result.clear();
        return false;
    }

    double minDistSqrd = __DBL_MAX__;
    size_t closestPathIndex = 0;
    size_t closestPointIndex = 0;
    bool closestIsClosed = closedFinishingPaths.size() > 0;

    // Search through closed finishing paths (can start at any point)
    for (size_t pathIndex = 0; pathIndex < closedFinishingPaths.size(); pathIndex++) {
        Path64& path = closedFinishingPaths.at(pathIndex);
        for (size_t i = 0; i < path.size(); i++) {
            double dist = DistanceSqrd(p1, path.at(i));
            if (dist < minDistSqrd) {
                minDistSqrd = dist;
                closestPathIndex = pathIndex;
                closestPointIndex = i;
                closestIsClosed = true;
            }
        }
    }

    // Search through open finishing paths (can only start at first vertex)
    for (size_t pathIndex = 0; pathIndex < openFinishingPaths.size(); pathIndex++) {
        Path64& path = openFinishingPaths.at(pathIndex);
        if (!path.empty()) {
            double dist = DistanceSqrd(p1, path.at(0));
            if (dist < minDistSqrd) {
                minDistSqrd = dist;
                closestPathIndex = pathIndex;
                closestPointIndex = 0;
                closestIsClosed = false;
            }
        }
    }

    // Handle open path: copy as-is (no rotation) and remove from list
    if (!closestIsClosed) {
        Path64& closestPath = openFinishingPaths.at(closestPathIndex);
        result = closestPath;  // Copy path as-is, starting at first vertex
        openFinishingPaths.erase(openFinishingPaths.begin() + closestPathIndex);
        return false;  // Return false to indicate open path
    }

    // Handle closed path: rotate to start at closest point and remove from list
    // Copy the path before erasing to avoid dangling reference
    Path64 closestPath = closedFinishingPaths.at(closestPathIndex);
    closedFinishingPaths.erase(closedFinishingPaths.begin() + closestPathIndex);
    result.clear();

    // Apply extraDistanceAround to advance further along the path before starting
    // Interpolate as needed
    if (closestPath.empty()
        || PathLength(closestPath) + sqrt(DistanceSqrd(closestPath.back(), closestPath.front())) < NTOL) {
        extraDistanceAround = 0;  // RayCAD: a path of zero length would never use the distance up (endless loop)
    }
    while (extraDistanceAround > 0) {
        size_t nexti = (closestPointIndex + 1) % closestPath.size();
        const Point64& p1 = closestPath[closestPointIndex];
        const Point64& p2 = closestPath[nexti];
        double distToNext = sqrt(DistanceSqrd(p1, p2));
        closestPointIndex = nexti;

        if (distToNext <= extraDistanceAround) {
            extraDistanceAround -= distToNext;
        }
        else {
            // Interpolate to produce the first vertex
            double interp = extraDistanceAround / distToNext;  // Fraction along the edge
            Point64 interpolated(
                Rnd(p1.x * (1 - interp) + p2.x * interp),
                Rnd(p1.y * (1 - interp) + p2.y * interp),
                p1.z
            );
            result.push_back(interpolated);
            extraDistanceAround = 0;
        }
    }

    // Output the remaining points from that start poitn
    for (size_t offset = 0; offset < closestPath.size(); offset++) {
        size_t index = (closestPointIndex + offset) % closestPath.size();
        result.push_back(closestPath.at(index));
    }

    return true;  // Return true to indicate closed path
}

void DeduplicatePaths(const Paths64& inputs, Paths64& outputs)
{
    outputs.clear();
    for (const auto& new_pth : inputs) {
        bool duplicate = false;
        // if all points of new path exist on some of the old paths, path is considered duplicate
        for (const auto& old_pth : outputs) {
            bool all_points_exists = true;
            for (const auto pt1 : new_pth) {
                bool pointExists = false;
                for (const auto pt2 : old_pth) {
                    if (DistanceSqrd(pt1, pt2) < SAME_POINT_TOL_SQRD_SCALED) {
                        pointExists = true;
                        break;
                    }
                }
                if (!pointExists) {
                    all_points_exists = false;
                    break;
                }
            }
            if (all_points_exists) {
                duplicate = true;
                break;
            }
        }

        if (!duplicate && !new_pth.empty()) {
            outputs.push_back(new_pth);
        }
    }
}

void ConnectPaths(Paths64 input, Paths64& output)
{
    output.clear();
    bool newPath = true;
    Path64 joined;
    while (!input.empty()) {
        if (newPath) {
            if (!joined.empty()) {
                output.push_back(joined);
            }
            joined.clear();
            for (auto pt : input.front()) {
                joined.push_back(pt);
            }
            input.erase(input.begin());
            newPath = false;
        }
        bool anyMatch = false;
        for (size_t i = 0; i < input.size(); i++) {
            Path64& n = input.at(i);
            if (DistanceSqrd(n.front(), joined.back()) < SAME_POINT_TOL_SQRD_SCALED) {
                for (auto pt : n) {
                    joined.push_back(pt);
                }
                input.erase(input.begin() + i);
                anyMatch = true;
                break;
            }
            else if (DistanceSqrd(n.back(), joined.back()) < SAME_POINT_TOL_SQRD_SCALED) {
                std::reverse(n.begin(), n.end());
                for (auto pt : n) {
                    joined.push_back(pt);
                }
                input.erase(input.begin() + i);
                anyMatch = true;
                break;
            }
            else if (DistanceSqrd(n.front(), joined.front()) < SAME_POINT_TOL_SQRD_SCALED) {
                for (auto pt : n) {
                    joined.insert(joined.begin(), pt);
                }
                input.erase(input.begin() + i);
                anyMatch = true;
                break;
            }
            else if (DistanceSqrd(n.back(), joined.front()) < SAME_POINT_TOL_SQRD_SCALED) {
                std::reverse(n.begin(), n.end());
                for (auto pt : n) {
                    joined.insert(joined.begin(), pt);
                }
                input.erase(input.begin() + i);
                anyMatch = true;
                break;
            }
        }
        if (!anyMatch) {
            newPath = true;
        }
    }
    if (!joined.empty()) {
        output.push_back(joined);
    }
}

// RayCAD: upstream's PerfCounter class and its nine file-scope instances (state shared by every instance of the
// engine, and clock() readings) are removed, with every Start() and Stop() call.

//***********************************
// Cleared area bounding support
//***********************************
class ClearedArea
{
public:
    ClearedArea(int64_t p_toolRadiusScaled)
    {
        toolRadiusScaled = p_toolRadiusScaled;
    };

    void SetClearedPaths(const Paths64& paths)
    {
        clearedPaths = paths;
        bboxPathsInvalid = true;
        bboxClippedInvalid = true;
    }

    void AddClearedPaths(const Paths64& paths)
    {
        clearedPaths = Union(clearedPaths, paths, FillRule::EvenOdd);
        clearedPaths = SimplifyPathsFilterPancakes(clearedPaths, CLEAN_POLYGONS_TOLERANCE);
        bboxPathsInvalid = true;
        bboxClippedInvalid = true;
    }

    void ExpandCleared(const Path64& toClearToolPath)
    {
        if (toClearToolPath.empty()) {
            return;
        }
        Paths64 toolCoverPoly = OffsetPath(toClearToolPath, double(toolRadiusScaled + 1), EndType::Round);
        clearedPaths = Union(clearedPaths, toolCoverPoly, FillRule::EvenOdd);
        clearedPaths = SimplifyPathsFilterPancakes(clearedPaths, CLEAN_POLYGONS_TOLERANCE);
        bboxPathsInvalid = true;
        bboxClippedInvalid = true;
    }

    // get cleared area/poly bounded to toolbox
    const Paths64& GetBoundedClearedAreaClipped(const Point64& toolPos, int64_t delta)
    {
        // first, attempt to serve this query from cache
        BoundBox toolBB(toolPos, delta);
        if (!bboxClippedInvalid && clearedBBClippedInFocus.Contains(toolBB)) {
            return clearedBoundedClipped;
        }

        // second, check if the window needs to be recomputed
        if (bboxClippedInvalid || !clearedBBWindow.Contains(toolBB)) {
            const int64_t deltaWindow = delta * clearedBoundedWindowScale;
            clearedBBWindow.SetFirstPoint(Point64(toolPos.x - deltaWindow, toolPos.y - deltaWindow));
            clearedBBWindow.AddPoint(Point64(toolPos.x + deltaWindow, toolPos.y + deltaWindow));

            Path64 bbPath;
            bbPath.push_back(Point64(toolPos.x - deltaWindow, toolPos.y - deltaWindow));
            bbPath.push_back(Point64(toolPos.x + deltaWindow, toolPos.y - deltaWindow));
            bbPath.push_back(Point64(toolPos.x + deltaWindow, toolPos.y + deltaWindow));
            bbPath.push_back(Point64(toolPos.x - deltaWindow, toolPos.y + deltaWindow));
            clearedBoundedWindow = Intersect(Paths64 {bbPath}, clearedPaths, FillRule::EvenOdd);
        }

        // finally, perform the query using data from the window
        clearedBBClippedInFocus.SetFirstPoint(Point64(toolPos.x - delta, toolPos.y - delta));
        clearedBBClippedInFocus.AddPoint(Point64(toolPos.x + delta, toolPos.y + delta));

        Path64 bbPath;
        bbPath.push_back(Point64(toolPos.x - delta, toolPos.y - delta));
        bbPath.push_back(Point64(toolPos.x + delta, toolPos.y - delta));
        bbPath.push_back(Point64(toolPos.x + delta, toolPos.y + delta));
        bbPath.push_back(Point64(toolPos.x - delta, toolPos.y + delta));
        clearedBoundedClipped = Intersect(Paths64 {bbPath}, clearedBoundedWindow, FillRule::EvenOdd);

        bboxClippedInvalid = false;
        return clearedBoundedClipped;
    }

    // RayCAD: the same query without reading or changing the cache, for the depth estimates (EstimateDepth), so that
    // they leave the step search's sequence of cache states, and with it its floating-point results, as upstream's
    Paths64 GetBoundedClearedAreaUncached(const Point64& toolPos, int64_t delta) const
    {
        Path64 bbPath;
        bbPath.push_back(Point64(toolPos.x - delta, toolPos.y - delta));
        bbPath.push_back(Point64(toolPos.x + delta, toolPos.y - delta));
        bbPath.push_back(Point64(toolPos.x + delta, toolPos.y + delta));
        bbPath.push_back(Point64(toolPos.x - delta, toolPos.y + delta));
        return Intersect(Paths64 {bbPath}, clearedPaths, FillRule::EvenOdd);
    }

    // get full cleared area
    const Paths64& GetCleared()
    {
        return clearedPaths;
    }

private:
    Paths64 clearedPaths;
    Paths64 clearedBoundedWindow;
    Paths64 clearedBoundedClipped;
    Paths64 clearedBoundedPaths;

    int64_t toolRadiusScaled;
    BoundBox clearedBBWindow;
    BoundBox clearedBBClippedInFocus;
    BoundBox clearedBBPathsInFocus;

    bool bboxClippedInvalid = false;
    bool bboxPathsInvalid = false;
    int clearedBoundedWindowScale = 10;
};

//***************************************
// Linear Interpolation - area vs angle
//***************************************
struct InterpItem
{
    std::pair<double, Point64> angle;
    double error;
    bool isConventional;
};

class Interpolation
{
public:
    const double MIN_ANGLE = -std::numbers::pi / 4;
    const double MAX_ANGLE = std::numbers::pi / 4;

    void clear()
    {
        m_min.reset();
        m_max.reset();
    }
    bool bothSides()
    {
        return m_min && m_max && m_min->error < 0 && m_max->error >= 0
            && (!m_min->isConventional || !m_max->isConventional);
    }
    // adds point keeping the incremental order of areas for interpolation to work correctly
    void addPoint(double error, std::pair<double, Point64> angle, bool allowSkip, bool isConventional)
    {
        const InterpItem newItem = {angle, error, isConventional};

        if (!m_min) {
            m_min = newItem;
        }
        else if (!m_max) {
            m_max = newItem;
            if (m_min->error > m_max->error) {
                auto tmp = m_min;
                m_min = m_max;
                m_max = tmp;
            }
        }
        else if (isConventional && (m_min->isConventional ^ m_max->isConventional)) {
            if (!allowSkip) {
                if (m_min->isConventional) {
                    m_min.reset();
                }
                else {
                    m_max.reset();
                }
                addPoint(error, angle, false, isConventional);
            }
        }
        else if (bothSides()) {
            if (error < 0) {
                m_min = newItem;
            }
            else {
                m_max = newItem;
            }
        }
        else {
            if (allowSkip && abs(error) > abs(m_min->error) && abs(error) > abs(m_max->error)
                && (isConventional || !m_min->isConventional || !m_max->isConventional)) {
                return;
            }

            if (m_min->isConventional ^ m_max->isConventional) {
                if (m_min->isConventional) {
                    m_min.reset();
                }
                else {
                    m_max.reset();
                }
            }
            else if (abs(m_min->error) > abs(m_max->error)) {
                m_min.reset();
            }
            else {
                m_max.reset();
            }
            addPoint(error, angle, false, isConventional);
        }
    }

    double interpolateAngle()
    {
        if (!m_min) {
            return MIN_ANGLE;
        }

        if (!m_max) {
            return MAX_ANGLE;
        }
        double p = (0 - m_min->error) / (m_max->error - m_min->error);

        // Ensure search is sufficiently efficient -- this is a compromise
        // between binary search (p = 0.5, guaranteed search completion in log
        // time) and following linear interpolation completely (often faster
        // since area cut is locally linear in movement angle)
        const double minInterp = .2;
        p = max(min(p, 1 - minInterp), minInterp);

        return m_min->angle.first * (1 - p) + m_max->angle.first * p;
    }

    double clampAngle(double angle)
    {
        return max(min(angle, MAX_ANGLE), MIN_ANGLE);
    }

    size_t getPointCount()
    {
        return (m_min ? 1 : 0) + (m_max ? 1 : 0);
    }

public:
    // {{angle, clipper point}, error}
    std::optional<InterpItem> m_min;
    std::optional<InterpItem> m_max;
};

//***************************************
// Adaptive2d main class - implementation
//***************************************

Adaptive2d::Adaptive2d()
{}

// Algorithm to compute area inside circle c2 but outside circle c1 and polygons clearedArea:
// All computations are done on doubles (at some point we can transition off of clipper and operate
// on curves!)
//
// Re-express the problem: find area inside all circles and polygons, where c1 and polygons are
// inverted
//
// 0) Extract from clearedArea a set of polygons close enough to potentially affect the bounded area
// 0.5) Rotate all geometry so the vector from c1 to c2 points up (y+)
// 1) Find all x-coordinates of interest:
//   a) All polygon vertices
//   b) Intersection of all polygons with c1
//   c) Intersection of all polygons with c2
//   d) There are no self-intersections or intersection points with other polygons (guarantee from
//   clipper), so we don't have to compute those
//   e) Compute intersection points between c1 and c2
//   f) //   Add c1's and c2's vertical tangents to the list
//   g) x=c2.X
// 2) Sort these x-coordinates. Discard all values before c2-r or after c2+r. We will consider
// ranges of x-values between these points 3) For each non-empty range in x, construct a vertical
// line through its midpoint
//   Over the full (open) x-range, vertical lines cross all polygons/circles in the same order (no
//   topology changes!) Also note that this line is guaranteed to not pass through any polygon
//   vertex, or tangent to any circle. No funny business! a) Compute the intersection point(s)
//   between the line and each polygon and circle. Keep track of which shape crossing each parameter
//   came from (polygon index and edge index, or circle index and top/bottom flag) b) Sort these
//   intersection on their y-coordinate. c) Loop over y-coordinates. At each, we will update state
//   to account for stepping over that crossing:
//     Init (i.e. y=-inf): outsideCount = 1 (outside c2 and inside all other shapes)
//     1) Identify the shape we are crossing; determine check in->out vs out->in
//     2) Update outsideCount and the list of what we're outside.
//       a) If outsideCount=0, totalArea += integral(x0, x1, crossed boundary)
//       b) If outsideCount was 0 and just changed to 1, totalArea -= integral(x0, x1, crossed
//       boundary)
//         ...careful with the signs on those integrals; be sure to add area from c2 and
//         subtract from other shapes
//     3) if x<c2.X, additionally accumulate area in conventionalArea, for detection of conventional
//     cutting
// 4) Return <totalArea, conventionalArea>
std::pair<double, double> Adaptive2d::CalcCutArea(Point64 c1, Point64 c2, ClearedArea& clearedArea, bool useCache)
{
    double dist = sqrt(DistanceSqrd(c1, c2));
    if (dist < NTOL) {
        return {0, 0};
    }
    stats.cutAreaCalls++;

    // 0) Extract from clearedArea a set of polygons close enough to potentially affect the bounded area
    vector<vector<PointD>> polygons;
    vector<PointD> inters;  // temporary, to hold intersection results
    const BoundBox c2BB(c2, toolRadiusScaled);
    // get curves from slightly enlarged region that will cover all points tested in this iteration
    const bool useC2 = dist > 2 * toolRadiusScaled;
    Paths64 uncached;  // RayCAD
    if (!useCache) {
        uncached = clearedArea.GetBoundedClearedAreaUncached(
            useC2 ? c2 : c1,
            toolRadiusScaled + (useC2 ? 0 : int64_t(dist)) + 4
        );
    }
    const Paths64& clearedBounded = !useCache ? uncached : clearedArea.GetBoundedClearedAreaClipped(
        useC2 ? c2 : c1,
        toolRadiusScaled + (useC2 ? 0 : int64_t(dist)) + 4
    );

    for (const Path64& path : clearedBounded) {
        if (path.size() == 0) {
            continue;
        }

        // bound box check
        BoundBox pathBB(path.front());
        for (const auto& pt : path) {
            pathBB.AddPoint(pt);
        }
        if (!pathBB.CollidesWith(c2BB)) {
            continue;  // this path cannot colide with tool
        }

        vector<PointD> polygon;
        for (const auto p : path) {
            polygon.emplace_back((double)p.x, (double)p.y);
        }
        polygons.push_back(polygon);
    }

    // 0.5) Rotate all geometry so the vector from c1 to c2 points up (y+)
    {
        const double angle = std::numbers::pi / 2 - dmath::atan2(double(c2.y - c1.y), double(c2.x - c1.x));
        const double ca = dmath::cos(angle);
        const double sa = dmath::sin(angle);
        c1 = {Rnd(ca * c1.x - sa * c1.y), Rnd(sa * c1.x + ca * c1.y)};
        c2 = {Rnd(ca * c2.x - sa * c2.y), Rnd(sa * c2.x + ca * c2.y)};
        vector<vector<PointD>> rotatedPolygons;
        for (vector<PointD>& pgon : polygons) {
            vector<PointD> rotated;
            for (auto& p : pgon) {
                rotated.push_back({ca * p.x - sa * p.y, sa * p.x + ca * p.y});
            }
            rotatedPolygons.push_back(rotated);
        }

        polygons = rotatedPolygons;
    }

    // 1) Find all x-coordinates of interest:
    vector<double> xs;
    for (const auto& polygon : polygons) {
        // 1.a) All polygon vertices
        for (const auto& p : polygon) {
            xs.push_back(p.x);
        }

        // 1.b) Intersection of all polygons with c1
        // 1.c) Intersection of all polygons with c2
        for (size_t i = 0; i < polygon.size(); i++) {
            const auto& p0 = polygon[i];
            const auto& p1 = polygon[(i + 1) % polygon.size()];
            if (Line2CircleIntersect(c1, toolRadiusScaled, p0, p1, inters)) {
                for (const auto p : inters) {
                    xs.push_back(p.x);
                }
            }
            if (Line2CircleIntersect(c2, toolRadiusScaled, p0, p1, inters)) {
                for (const auto p : inters) {
                    xs.push_back(p.x);
                }
            }
        }
    }

    // 1.e) Compute intersection points between c1 and c2
    {
        pair<PointD, PointD> res;
        if (Circle2CircleIntersect(c1, c2, toolRadiusScaled, res)) {
            xs.push_back(res.first.x);
            xs.push_back(res.second.x);
        }
    }

    // 1.f) Add c1's and c2's vertical tangents to the list
    xs.push_back((double)c1.x - toolRadiusScaled);
    xs.push_back((double)c1.x + toolRadiusScaled);

    const double xmin = (double)c2.x - toolRadiusScaled;
    const double xmax = (double)c2.x + toolRadiusScaled;
    xs.push_back(xmin);
    xs.push_back(xmax);

    // 1.g) x=c2.X
    xs.push_back(c2.x);

    // 2) Sort these x-coordinates. Discard all values before c2-r or after c2+r
    {
        vector<double> xfilter;
        for (const double x : xs) {
            if (xmin <= x && x <= xmax) {
                xfilter.push_back(x);
            }
        }
        xs = xfilter;
        std::sort(xs.begin(), xs.end());
    }

    const auto interpX = [](const PointD p0, const PointD p1, double x) {
        const double interp = (x - p0.x) / (p1.x - p0.x);
        const double y = (p1.y * interp) + (p0.y * (1 - interp));
        return y;
    };

    // 3) For each non-empty range in x, construct a vertical line through its midpoint
    const vector<PointD> circles = {PointD(c2.x, c2.y), PointD(c1.x, c1.y)};
    double area = 0;
    double conventionalArea = 0;
    for (size_t ix = 0; ix + 1 < xs.size(); ix++) {
        const double x0 = xs[ix];
        const double x1 = xs[ix + 1];
        if (x0 == x1) {
            continue;
        }
        const double xtest = (x0 + x1) / 2;

        // 3.a) Compute the intersection point(s) between the line and each polygon and circle. Keep
        // track of which shape crossing each parameter came from (polygon index and edge index, or
        // circle index and top/bottom flag)

        // y, polygon index (or polygons.size() + circle index), edge index (or 0/1 for top/bottom half)
        vector<tuple<double, size_t, size_t>> ys;

        for (size_t ipolygon = 0; ipolygon < polygons.size(); ipolygon++) {
            const auto& polygon = polygons[ipolygon];
            for (size_t iedge = 0; iedge < polygon.size(); iedge++) {
                const auto& p0 = polygon[iedge];
                const auto& p1 = polygon[(iedge + 1) % polygon.size()];
                // note: we skip if the edge is vertical, p0.x == p1.x == xtest
                if (min(p0.x, p1.x) < xtest && max(p0.x, p1.x) > xtest) {
                    const double y = interpX(p0, p1, xtest);
                    ys.emplace_back(y, ipolygon, iedge);
                }
            }
        }

        for (size_t icircle = 0; icircle < circles.size(); icircle++) {
            const PointD c = circles[icircle];
            const double dx = abs(xtest - c.x);
            if (dx < toolRadiusScaled) {  // skip tangent; xtest can't be a tangent anyway
                const double dy = sqrt((toolRadiusScaled * toolRadiusScaled) - (dx * dx));
                ys.emplace_back(c.y + dy, polygons.size() + icircle, 0);
                ys.emplace_back(c.y - dy, polygons.size() + icircle, 1);
            }
        }

        // 3.b) Sort these intersection on their y-coordinate.
        // RayCAD: equal y-coordinates are ordered by shape and part (the whole tuple), not left to the library
        std::sort(
            ys.begin(),
            ys.end(),
            [](const std::tuple<double, size_t, size_t>& a,
               const std::tuple<double, size_t, size_t>& b) {
                return a < b;
            }
        );

        // 3.c) Loop over y-coordinates. At each, we will update state to account for stepping over
        // that crossing:
        //     Init (i.e. y=-inf): outsideCount = 1 (outside c2 and inside all other shapes)
        std::vector<bool> outside;
        outside.reserve(polygons.size() + circles.size());
        for (size_t i = 0; i < polygons.size() + circles.size(); i++) {
            outside.push_back(i == (polygons.size()));  // poly_0, ..., poly_n-1, c2, c1
        }
        int outsideCount = 1;
        for (const auto& [_, ishape, ipart] : ys) {
            const bool prevOutside = outside[ishape];
            const int prevCount = outsideCount;
            outside[ishape] = !outside[ishape];
            outsideCount += prevOutside ? -1 : 1;

            // Sign
            // We want to compute integral(exitY - entranceY)
            // We do this in two steps: -integral(entranceY - 0) + integral(exitY - 0)
            const double entranceExitSign = prevOutside ? -1 : 1;

            if (outsideCount == 0 || prevCount == 0) {
                if (ishape < polygons.size()) {
                    // crossed a polygon
                    const auto& polygon = polygons[ishape];
                    const auto& p0 = polygon[ipart];
                    const auto& p1 = polygon[(ipart + 1) % polygon.size()];
                    const auto y0 = interpX(p0, p1, x0);
                    const auto y1 = interpX(p0, p1, x1);
                    const double newArea = (y0 + y1) / 2 * (x1 - x0);
                    area += entranceExitSign * newArea;
                    if (xtest < c2.x) {
                        conventionalArea += entranceExitSign * newArea;
                    }
                }
                else {
                    // crossed a circle
                    const auto c = circles[ishape - polygons.size()];
                    const double circleSign = ipart == 0 ? 1 : -1;

                    // first, compute area of sector - area of triangle = area of segment
                    const auto clamp = [](double a) {
                        return max(-1.0, min(1.0, a));
                    };
                    // clamp is required only because of floating point rounding errors
                    const double phi0 = dmath::acos(clamp((x0 - c.x) / toolRadiusScaled)) * circleSign;
                    const double phi1 = dmath::acos(clamp((x1 - c.x) / toolRadiusScaled)) * circleSign;
                    const double areaSector = toolRadiusScaled * toolRadiusScaled / 2
                        * abs(phi1 - phi0);

                    const double y0 = c.y
                        + circleSign
                            * sqrt(toolRadiusScaled * toolRadiusScaled - (x0 - c.x) * (x0 - c.x));
                    const double y1 = c.y
                        + circleSign
                            * sqrt(toolRadiusScaled * toolRadiusScaled - (x1 - c.x) * (x1 - c.x));
                    const double tbase = sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
                    const double tmidx = (x0 + x1) / 2;
                    const double tmidy = (y0 + y1) / 2;
                    const double th = sqrt(
                        ((tmidx - c.x) * (tmidx - c.x)) + ((tmidy - c.y) * (tmidy - c.y))
                    );
                    const double areaTriangle = tbase * th / 2;
                    const double areaSegment = areaSector - areaTriangle;

                    // then add on trapezoid between the segment and 0
                    // the sign of the segment area is negative for bottom half of the circle,
                    // positive for top half
                    const double areaTrapezoid = (x1 - x0) * (y0 + y1) / 2;
                    const double newArea = (circleSign * areaSegment) + areaTrapezoid;
                    area += entranceExitSign * newArea;
                    if (xtest < c2.x) {
                        conventionalArea += entranceExitSign * newArea;
                    }
                }
            }
        }
    }

    return {area, conventionalArea};
}

void Adaptive2d::ApplyStockToLeave(Paths64& inputPaths)
{
    if (stockToLeave > NTOL) {
        if (opType == OperationType::otClearingOutside
            || opType == OperationType::otProfilingOutside) {
            inputPaths = OffsetPaths(inputPaths, stockToLeave * scaleFactor);
        }
        else {
            inputPaths = OffsetPaths(inputPaths, -stockToLeave * scaleFactor);
        }
    }
    else {
        // no offset needed, but do some minimal smoothing
        inputPaths = OffsetPaths(inputPaths, -1);
        filterCloseValues(inputPaths);
        inputPaths = OffsetPaths(inputPaths, 1);
        filterCloseValues(inputPaths);
    }
}

// RayCAD: upstream's writePreprocessingSVG (DEBUG_SVG), which wrote adaptive_preprocessing.svg into the current
// folder, is removed with the DebugSVGInfo it drew.

//********************************************
// Adaptive2d - Execute
//********************************************

std::list<AdaptiveOutput> Adaptive2d::Execute(
    const Paths64& stockPaths,
    const Paths64& paths,
    const Paths64& clearedPaths
)
{
    //**********************************
    // Initializations
    //**********************************

    // keep the tolerance in workable range
    tolerance = max(tolerance, 0.01);
    tolerance = min(tolerance, 1.0);

    // 1/"tolerance" = number of min-size adaptive steps per stepover
    // RayCAD: scaleFactor (MIN_STEP_CLIPPER / tolerance / min(1.0, stepOverFactor * toolDiameter), rounded) is set
    // by the caller, which converts the inputs with it.

    // RayCAD: an instance may run Execute again; nothing carries over (upstream kept appending to results and
    // stockInputPaths).
    results.clear();
    stockInputPaths.clear();
    stats = Stats {};
    region_count = 0;

    current_region = 0;

    toolRadiusScaled = Rnd(toolDiameter * double(scaleFactor) / 2);
    stepOverScaled = double(toolRadiusScaled) * stepOverFactor;
    stopProcessing = false;

    if (helixRampTargetDiameter < NTOL) {
        helixRampTargetDiameter = toolDiameter;
    }
    helixRampTargetDiameter = min(helixRampTargetDiameter, toolDiameter);
    helixRampMinDiameter = max(helixRampMinDiameter, toolDiameter / 8);
    helixRampTargetDiameter = max(helixRampTargetDiameter, helixRampMinDiameter);

    helixRampMaxRadiusScaled = Rnd(helixRampTargetDiameter * double(scaleFactor) / 2);
    helixRampMinRadiusScaled = Rnd(helixRampMinDiameter * double(scaleFactor) / 2);
    finishPassOffsetScaled = finishingProfile ? Rnd(stepOverScaled * FINISHING_THICKNESS_SCALE) : 0;

    // generate tool shape
    Paths64 toolGeometryPaths = OffsetPath(Path64 {{{0, 0}}}, double(toolRadiusScaled), EndType::Round);
    Path64& toolGeometry = toolGeometryPaths[0];
    // calculate reference area
    Path64 slotCut = TranslatePath(toolGeometry, (int64_t)(toolRadiusScaled / 2), (int64_t)0);
    Paths64 crossing = Difference(Paths64 {toolGeometry}, Paths64 {slotCut}, FillRule::EvenOdd);
    referenceCutArea = fabs(Area(crossing[0]));
    optimalCutAreaPD = 2 * stepOverFactor * referenceCutArea / double(toolRadiusScaled);
    //******************************
    // Convert input paths to clipper
    //******************************
    // RayCAD: the paths arrive in grid units
    Paths64 converted;
    for (size_t i = 0; i < paths.size(); i++) {
        converted.push_back(SimplifyPath(paths[i], FINISHING_CLEAN_PATH_TOLERANCE, false));
    }
    Paths64 inputPaths;
    DeduplicatePaths(converted, inputPaths);
    ConnectPaths(inputPaths, inputPaths);
    inputPaths = Union(inputPaths, FillRule::EvenOdd);
    ApplyStockToLeave(inputPaths);

    //*************************
    // convert stock paths
    //*************************
    for (size_t i = 0; i < stockPaths.size(); i++) {
        stockInputPaths.push_back(stockPaths[i]);
    }

    // Convert cleared area
    Paths64 initialClearedPaths = clearedPaths;
    initialClearedPaths = Union(initialClearedPaths, FillRule::EvenOdd);

    stockInputPaths = Union(stockInputPaths, FillRule::EvenOdd);

    // Handle different clearing modes (inside/outside, clearing/profiling) and invoke the core
    // adaptive algorithm
    // 1) If outer clearing: add stock path to inputs
    // 2) Fix input path orientation
    // 3) Set Z=1 on all input paths to tag them as needing a finishing pass
    // 4) Turn profiles into areas; mark new paths as unfinished (Z=0) and then union
    // 5) If going outside the stock is allowed, add regionOutsideStock to both inputPaths and
    // 6) Compute toolBounds = offset(input paths, -(toolRadius + finishingThickness)).
    // 7) Loop over connected components using nesting level.
    // 8) finishingPass = offset(currentTBP, finishingThickness), filtered for paths with Z=1
    // 9) Compute bounds = offset(currentTBP, toolRadius)
    // 10) Run core algorithm on (bounds, toolBounds, finishingPass, clearedArea)

    // 1) If outer clearing: add stock path to inputs
    if (opType == OperationType::otClearingOutside) {
        for (const Path64& p : stockInputPaths) {
            inputPaths.push_back(p);
        }
    }

    // 2) Fix input path orientation
    for (Path64& inputPath : inputPaths) {
        // nesting count includes self, so odd is positive orientation
        int nesting = getPathNestingLevel(inputPath, inputPaths);
        if ((nesting % 2 == 1) ^ IsPositive(inputPath)) {
            std::reverse(inputPath.begin(), inputPath.end());
        }
    }

    // 3) Set Z=1 on all input paths to tag them as needing a finishing pass
    //
    // When finishing passes are eventually generated, they will be created only for edges with
    // z=1 on both end vertices. This is a little bit imprecise -- ideally the segments themselves
    // would be tagged instead of their end vertices, but clipper doesn't support that feature
    // natively. From what I've seen this is good enough in practice, but if it becomes problematic
    // we can try to leverage vertex z-tagging to create a more complicated system for indirectly
    // tagging edges.
    for (Path64& inputPath : inputPaths) {
        for (Point64& p : inputPath) {
            p.z = 1;
        }
    }

    // 4) Turn profiles into areas; mark new paths as unfinished (Z=0) and then union
    //
    // Adaptive "profiling" clears a small area offset from the input edges, but since the goal of
    // the operation is profiling, rather than area clearing, only the input edge needs to be
    // finished. The far sides of those areas do not require a finishing pass
    if (opType == OperationType::otProfilingOutside || opType == OperationType::otProfilingInside) {
        // offset by an extra finishPassOffsetScaled to compensate for undoing that later
        int64_t offset = 2 * (toolRadiusScaled + helixRampMaxRadiusScaled + finishPassOffsetScaled)
            + int64_t(MIN_STEP_CLIPPER);
        if (opType == OperationType::otProfilingInside) {
            offset = -offset;
        }

        Paths64 fullPaths;

        for (Path64& inputPath : inputPaths) {
            Paths64 offsetPaths = OffsetPath(inputPath, double(offset));

            if (IsPositive(inputPath) ^ (offset > 0)) {
                std::reverse(inputPath.begin(), inputPath.end());
            }
            Paths64 clips = {inputPath};
            for (Path64& offsetPath : offsetPaths) {
                for (Point64& p : offsetPath) {
                    p.z = 0;
                }
                if (IsPositive(offsetPath) ^ (offset < 0)) {
                    std::reverse(offsetPath.begin(), offsetPath.end());
                }
                clips.push_back(offsetPath);
            }
            fullPaths = Union(fullPaths, clips, FillRule::EvenOdd);
        }

        inputPaths = fullPaths;
    }

    // 5) If going outside the stock is allowed, add regionOutsideStock to both inputPaths and
    // clearedArea. Use Z=0 to mark the stock boundary as not needing to be finished
    //
    // We are working towards generating the toolBoundsPath -- the region in which the tool center
    // is allowed to be. So far we have the basis for allowing the tool within the raw input
    // regions. In this step, if going outside the stock is allowed, we add a boundary region
    // outside the stock to the inputs so the tool will be allowed there too.
    //
    // All new paths created in this process have vertices tagged with Z=0 to prevent them from
    // receiving finishing passes. Also, the new regions are added to the representation of
    // already-cleared area so the algorithm knows that the tool *can* go there but does not need
    // to.
    if (!forceInsideOut) {

        // 5a) Shrink the stock boundary to ensure overlap with input paths that hit the boundary
        Paths64 stockRev = OffsetPaths(stockInputPaths, -2);
        for (Path64& p : stockRev) {
            std::reverse(p.begin(), p.end());
        }

        // 5b) Create outside-of-stock region
        double overshootDistance = double(4 * toolRadiusScaled) + stockToLeave * double(scaleFactor);
        Paths64 outsideOfStock
            = OffsetPaths(stockInputPaths, overshootDistance, EndType::Polygon, JoinType::Square);

        // 5c) Union input paths with stock regions
        Clipper64 clip;
        clip.AddSubject(inputPaths);
        clip.AddClip(stockRev);
        clip.AddClip(outsideOfStock);

        // Z callback: output Z=1 if both vertices of either input edge have Z=1.
        // This callback will be invoked on new vertices created while unioning areas (i.e.
        // intersections between segments)
        clip.SetZCallback([](const Point64& e1bot,
                             const Point64& e1top,
                             const Point64& e2bot,
                             const Point64& e2top,
                             Point64& pt) {
            bool edge1HasZ1 = (e1bot.z == 1 && e1top.z == 1);
            bool edge2HasZ1 = (e2bot.z == 1 && e2top.z == 1);
            pt.z = (edge1HasZ1 || edge2HasZ1) ? 1 : 0;
        });

        clip.Execute(ClipType::Union, FillRule::EvenOdd, inputPaths);

        // 5d) Update cleared area
        outsideOfStock
            = OffsetPaths(stockInputPaths, double(100 * toolRadiusScaled), EndType::Polygon, JoinType::Square);

        clip.Clear();
        clip.AddSubject(initialClearedPaths);
        clip.AddClip(stockRev);
        clip.AddClip(outsideOfStock);
        clip.Execute(ClipType::Union, FillRule::EvenOdd, initialClearedPaths);
    }

    // 5e) Use initialClearedPaths to zero out Z-values for parts of the input paths that do not
    // need to be finished. This was added after the fact and I probably ought to make it a
    // high-level step of its own, but I didn't want to renumber everything.
    //
    // So far, "input" paths are tagged as needing to be finished if the user input them, but not if
    // they are generated to allow tool movement outside the stock or if they are generated as the
    // boundary region of a path to be profiled. However, in addition to these exceptions, some
    // (complete or incomplete) sections of user-specified input paths also do not require finishing
    // passes because they protrude into regions that are already clear. For example, if the user
    // runs adaptive on the same face twice, first with a large tool and then again with a small
    // tool (with rest machining enabled), much of the boundary of the small-tool adaptive operation
    // may already be cleared, and does not require a finishing pass. Only the input boundaries that
    // are actually cut in the current operation require a finishing pass. (If the user wants a
    // full-boundary finishing pass with the small tool in this scenario, they should leave some
    // stock uncut at the boundary of the large-tool pass.)
    //
    // This computation is done by computing the initial non-cleared area and intersecting it with
    // the input path boundary. The portions of the boundary present in this intersection need to be
    // finished; the rest do not.
    //
    // Unfortunately, Clipper does not natively support this sort of "update z if in intersection"
    // operation. In order build it out of clipper primitives, we start by creating a copy of the
    // input path with z-values corresponding to the index/position in the original input. This
    // enables us to keep track of which vertex is which after taking the intersection. Then create
    // the updated version of the path by simultaneously iterating through the original input path
    // and the intersection result and re-assembling the input boundary path with appropriately
    // (z=0/z=1) tagged vertices to specify if they require a finishing pass.
    //
    // There is some subtlety to this tracking+reassembly process because new vertices may be
    // generated by the intersection operation, subdividing input edges one or more times. These
    // vertices are assigned new z-values that uniquely identify them, but these z-values do not
    // correspond to the index of any vertex in the initial path. For these newly generated points,
    // we separately track their position along the boundary (represented as a floating point number
    // interpolating the positions of the vertices of their parent edges). We also check if the
    // parent edge needs to be finished (at least as understood prior to this computation). If so,
    // we also tag this splitting vertex as needing to be finished to preserve that property when
    // the edge is split.
    //
    // During path reassembly, information about new/split-edge vertices needs to be looked up by a
    // different process than original vertices, but all of the same information (position/order on
    // the path, potentially requires finishing) is present, so after the information lookup is
    // complete they can be handled the same way as original vertices.
    {
        // Compute noncleared area = stock - cleared
        Clipper64 clipDiff;
        clipDiff.AddSubject(stockInputPaths);
        clipDiff.AddClip(initialClearedPaths);
        Paths64 nonclearedPaths;
        clipDiff.Execute(ClipType::Difference, FillRule::EvenOdd, nonclearedPaths);

        Paths64 updatedPaths;
        for (const auto& path : inputPaths) {
            // Create a copy of the path with z value specifying the index. Points are initialized
            // with z=0 by default, so to make uninitialized-z bugs clear we skip z=0 and set z =
            // index + 1
            Path64 indexedPath;
            for (size_t i = 0; i < path.size(); i++) {
                indexedPath.emplace_back(path[i].x, path[i].y, i + 1);
            }
            // Explicitly close the path, before doing a boolean operation on it as an open (well,
            // "not implicitly closed") path. Unfortunately clipper treats points as interchangeable
            // if they have equal x and y even if z is different, so this end vertex also shares the
            // start vertex's z-value.
            indexedPath.push_back(indexedPath[0]);

            // Prepare structures to store information about newly generated/edge-splitting points
            const int firstNewZ = static_cast<int>(path.size()) + 2;
            int nextNewZ = firstNewZ;
            std::map<int, double> newZPosition;
            std::map<int, int64_t> newZNeedsFinishing;

            // Create a new clipper object to avoid overwriting the z callback of the main one
            Clipper64 clip2;
            clip2.SetZCallback([&nextNewZ, &newZPosition, &newZNeedsFinishing, &path](
                                   const Point64& e1bot,
                                   const Point64& e1top,
                                   const Point64& e2bot,
                                   const Point64& e2top,
                                   Point64& pt
                               ) {
                (void)e2bot;
                (void)e2top;
                // If the split-edge point is actuall at an edge then we have no need for a new
                // z-value. Otherwise, generate a new z value
                if (pt.x == e1bot.x && pt.y == e1bot.y) {
                    pt.z = e1bot.z;
                    return;
                }
                if (pt.x == e1top.x && pt.y == e1top.y) {
                    pt.z = e1top.z;
                    return;
                }
                pt.z = nextNewZ++;

                // Compute the position of the new point by interpolating the z-values of its input
                // parent edge (e1 is the parent edge of the subject/input path; e2 comes from the
                // clipping/cleared area path)

                // Compute distance from e1bot to e1top
                double dx_total = e1top.x - e1bot.x;
                double dy_total = e1top.y - e1bot.y;
                double dist_total = sqrt(dx_total * dx_total + dy_total * dy_total);

                // Compute distance from e1bot to pt
                double dx_pt = pt.x - e1bot.x;
                double dy_pt = pt.y - e1bot.y;
                double dist_pt = sqrt(dx_pt * dx_pt + dy_pt * dy_pt);

                // Compute interpolated position Z value. In most cases edges are z=n and z=n+1, and
                // we do a simple interpolation. However, the final edge connects z=path.size() to
                // z=1, and that interpolation result does not correctly specify the position of the
                // new point. Instead, we detect that case and interpolate between path.size() and
                // path.size() + 1.
                double e1bot_effective = (static_cast<std::size_t>(e1bot.z) == 1
                                          && static_cast<std::size_t>(e1top.z) == path.size())
                    ? static_cast<double>((path.size() + 1))
                    : static_cast<double>(e1bot.z);
                double e1top_effective = (e1top.z == 1
                                          && static_cast<std::size_t>(e1bot.z) == path.size())
                    ? (path.size() + 1)
                    : static_cast<double>(e1top.z);
                double interp = dist_pt / dist_total;
                double interpZ = e1bot_effective + interp * (e1top_effective - e1bot_effective);

                // Update data structures for the new point
                newZPosition[pt.z] = interpZ;

                // I'm pretty sure e1bot and e1top are always original vertices, but just in case
                // we can handle the alternative possibility
                int64_t e1bot_finishing = (e1bot.z >= 1
                                           && static_cast<std::size_t>(e1bot.z) <= path.size())
                    ? path[e1bot.z - 1].z
                    : newZNeedsFinishing[static_cast<int>(e1bot.z)];
                int64_t e1top_finishing = (e1top.z >= 1
                                           && static_cast<std::size_t>(e1top.z) <= path.size())
                    ? path[e1top.z - 1].z
                    : newZNeedsFinishing[static_cast<int>(e1top.z)];
                newZNeedsFinishing[pt.z] = e1bot_finishing && e1top_finishing;
            });

            // Intersect the input boundary (i.e. open path intersection) with the noncleared area
            clip2.AddOpenSubject({indexedPath});
            clip2.AddClip(nonclearedPaths);
            Paths64 unusedClosedPaths, nonclearedInputs;
            clip2.Execute(ClipType::Intersection, FillRule::EvenOdd, unusedClosedPaths, nonclearedInputs);

            // Prepare for the final reassembly process by collecting all points from the
            // intersection result and filtering out duplicates. Note that the start/end point is an
            // expected duplicate, but other points may appear twice if they are present in both
            // indexedPath and nonclearedPaths.
            std::vector<Point64> nonclearedPoints;
            std::set<int64_t> seenZ;
            for (const auto& nonclearedPath : nonclearedInputs) {
                for (const auto& pt : nonclearedPath) {
                    if (seenZ.insert(pt.z).second) {
                        nonclearedPoints.push_back(pt);
                    }
                }
            }
            indexedPath.pop_back();  // remove duplicate start point from the input

            // Further prepare for reassembly by sorting the collected points by their position
            // (z-value for original points, newZPosition for new points). This allows us to
            // reassemble with a single pass, simultaneously iterating through both the original
            // input points and the intersection results in order of increasing position.
            // RayCAD: equal positions are ordered by z (unique here), not left to the library
            std::sort(
                nonclearedPoints.begin(),
                nonclearedPoints.end(),
                [&newZPosition, &firstNewZ](const Point64& p1, const Point64& p2) {
                    double val1 = (p1.z >= firstNewZ) ? newZPosition.at(int(p1.z)) : (double)p1.z;
                    double val2 = (p2.z >= firstNewZ) ? newZPosition.at(int(p2.z)) : (double)p2.z;
                    if (val1 != val2) {
                        return val1 < val2;
                    }
                    return p1.z < p2.z;
                }
            );

            // Reassemble the updated input path with new z-values.
            //
            // Case 1: Points present in the original input and not the intersection appear in
            // cleared area and do not require finishing (set z=0).
            //
            // Case 2: Points present in the intersection and not the original input are
            // generated/edge-splitting points and need to be added. They appear in the non-cleared
            // area, so they should be finished if their parent edge wanted finishing (i.e. use the
            // value in newZNeedsFinishing).
            //
            // Case 3: Points present in both appear in the non-cleared area, so they should be
            // finished if they are already tagged for finishing (i.e. keep the original z-value
            // from the input).
            Path64 updatedPath;
            auto nonclearedIt = nonclearedPoints.begin();
            auto indexedIt = indexedPath.begin();

            while (indexedIt != indexedPath.end() || nonclearedIt != nonclearedPoints.end()) {
                // Get position of next noncleared point (if any remain)
                double nextNonclearedPos = (nonclearedIt != nonclearedPoints.end())
                    ? ((nonclearedIt->z >= firstNewZ) ? newZPosition.at(nonclearedIt->z)
                                                      : static_cast<double>(nonclearedIt->z))
                    : std::numeric_limits<double>::max();

                // Get position of next indexed path point (if any)
                double nextPathPos = (indexedIt != indexedPath.end())
                    ? static_cast<double>(indexedIt->z)
                    : std::numeric_limits<double>::max();

                if (nextPathPos < nextNonclearedPos) {
                    // Case 1: original point is not in the noncleared region
                    Point64 newPt = path[indexedIt->z - 1];
                    newPt.z = 0;
                    updatedPath.push_back(newPt);
                    indexedIt++;
                }
                else if (nextNonclearedPos < nextPathPos) {
                    // Case 2: newly generated/split-edge point in the noncleared region
                    Point64 newPt = *nonclearedIt;
                    newPt.z = newZNeedsFinishing.at(newPt.z);
                    updatedPath.push_back(newPt);
                    nonclearedIt++;
                }
                else {
                    // Case 3: the same point is present in both lists
                    Point64 newPt = path[indexedIt->z - 1];
                    updatedPath.push_back(newPt);
                    indexedIt++;
                    nonclearedIt++;
                }
            }

            updatedPaths.push_back(updatedPath);
        }

        inputPaths = updatedPaths;
    }

    // 6) Compute toolBounds = offset(input paths, -(toolRadius + finishingThickness)).
    //
    // This represents the locations the tool center is allowed to be during the main adatpive
    // pass, prior to applying finishing profiling.

    // Offset using a Z callback to preserve Z=1 marking
    // Note that PreserveCollinear must be set of ClipperOffset (it is already set by default on
    // Clipper64) to avoid accidentally optimizing out collinear points with differing z-values
    offset::ClipperOffset clipof(2.0, 0.25);  // RayCAD: engine_offset.h
    clipof.PreserveCollinear(true);
    clipof.AddPaths(inputPaths, JoinType::Round, EndType::Polygon);

    // Z callback: output Z=1 if both vertices of either input edge have Z=1
    clipof.SetZCallback([](const Point64& e1bot,
                           const Point64& e1top,
                           const Point64& e2bot,
                           const Point64& e2top,
                           Point64& pt) {
        // Check if both vertices of edge 1 have Z=1
        bool edge1HasZ1 = (e1bot.z == 1 && e1top.z == 1);
        // Check if both vertices of edge 2 have Z=1
        bool edge2HasZ1 = (e2bot.z == 1 && e2top.z == 1);
        // Set output Z=1 if either edge has both vertices with Z=1
        pt.z = (edge1HasZ1 || edge2HasZ1) ? 1 : 0;
    });

    Paths64 toolBounds;
    clipof.Execute(-double(toolRadiusScaled + finishPassOffsetScaled), toolBounds);

    // RayCAD: the number of regions, for progress
    for (const auto& current : toolBounds) {
        if (getPathNestingLevel(current, toolBounds) % 2 != 0) {
            region_count++;
        }
    }
    stats.regions = region_count;

    // 7) Loop over connected components of toolBounds
    //
    // The main adaptive algorithm (ProcessPolyNode) is run on one connected component at a time
    // to facilitate considering a helix-down entrance exclusively for the initial engagement of
    // each component and using entrances from the cleared area for all subsequent reengagements.
    //
    // Note that connectivity here is determined based on the toolBounds representation, which
    // indicates where the tool center is allowed to be. If two regions are connected only by
    // narrow channels that do not fit the tool, they will be split up for independent processing
    // here. This guarantees that the region provided to ProcessPolyNode can be fully cleared by
    // repeated reengagement from the cleared area after the initial entrance (which may require
    // helixing down).
    //
    // When we loop over toolBounds, we look for outer boundaries only. When we find one, we also
    // accumulate all top-level holes inside of them. The boundary path and immediate hole paths are
    // processed together. Any further-nested paths (i.e. appearing inside a hole) will be processed
    // in a separate iteration of the loop.
    for (const auto& current : toolBounds) {
        // Nesting counts itself and the number of polygons containing it, so nesting % 2 == 1
        // specifies exterior boundaries and nesting % 2 == 0 specifies holes
        int nesting = getPathNestingLevel(current, toolBounds);
        if (nesting % 2 != 0) {
            // current is an exterior boundary; now find all the holes directly inside it
            Paths64 currentTBP;
            currentTBP.push_back(current);

            for (size_t iother = 0; iother < toolBounds.size(); iother++) {
                const auto& other = toolBounds[iother];

                if (PointInPolygon(other.front(), current) != PointInPolygonResult::IsOutside) {
                    if (getPathNestingLevel(other, toolBounds) == nesting + 1) {
                        currentTBP.push_back(other);
                    }
                }
            }

            // 8) finishingPass = offset(currentTBP, finishingThickness)
            //
            // Now that we have currentTBP representing where the tool center may be while during
            // the main clearing operation, we need to reconstruct the required finishing passes for
            // that region.
            //
            // Note that it is
            // important that we offset the input inward to create the TBP and then offset part of
            // the way back outwards from the TBP to create the finishing pass, and that we do not
            // directly compute the finishing pass from the input by only offsetting inwards. If the
            // input has narrow channels that are just wide enough to fit the tool for a finishing
            // pass, those regions will not be wide enough for interior clearing. If we actually
            // executed this "finishing" pass, the tool would be slotting when it entered that
            // region. By instead constructing our finising pass by offsetting outward from the TBP,
            // we guarantee that all points on the finishing pass represent a small stepover cut
            // from already-cleared area. Thin channels that just barely/exactly fit the tool will
            // not be cut, but that is a reality of an adaptive cutting process -- such regions must
            // be cut by a slotting operation instead.
            clipof.Clear();
            clipof.AddPaths(currentTBP, JoinType::Round, EndType::Polygon);

            Paths64 finishingPass;
            clipof.Execute(double(finishPassOffsetScaled), finishingPass);

            // 9) Compute bounds = offset(currentTBP, toolRadius)
            //
            // These paths represent the region that will actually be cleared by the interior
            // clearing process. They do not include the region cleared by the finishing pass.
            //
            // When the interior clearing process completes, we will assert that this region is
            // fully cleared. In order to make the numerics work correctly on this check, we
            // actually need this region to be *slightly* smaller than its ideal/nominal size.
            // Specifically, each offset operation we perform can produce up to sqrt(2) units of
            // error. (This is a necessary result of working on an integer grid because there must
            // be an orientation for input segments where a tiny perturbation in the input
            // orientation (which can always be achieved, even on an integer grid) changes the
            // output location. This is an output change of 1 for an input change of epsilon, but
            // the non-dicritized output would only have changed by epsilon. Therefore, offset
            // operations must have potential to accumulate error of up to sqrt(2) units per offset
            // operation). We perform 2 offset operations to produce this path (input -> TBP ->
            // BP), and we will do 1 more when computing the cleared area. So when computing BP, we
            // leave a buffer of 3 sqrt(2) units to ensure it will be seen as fully cleared when
            // checking a correct interior clearing pass. This process is a bit finicky -- take
            // great care if you modify it!
            Paths64 boundPath
                = OffsetPaths(currentTBP, double(toolRadiusScaled) - ceil(3 * std::numbers::sqrt2));

            // Skip path generation if bounds are fully cleared
            //
            // Note: should we also be checking that finishing passes (if any are required) are
            // also clear? I'd think so, but I'm just documenting now and not going to change it.
            // If I were to implement that, I'd do subtract(offset(finishing, toolRadius), cleared)
            // and if the result contains any z=1 segments then there is finishing work left to do.
            // I don't know if ProcessPolyNode is well equipped to handle a fully pre-cleared
            // interior region with only finishing work to do; probably it has never been tested and
            // it is not!
            {
                Paths64 boundsToClear = Difference(boundPath, initialClearedPaths, FillRule::EvenOdd);
                if (!boundsToClear.size()) {
                    continue;
                }
            }

            // RayCAD: stop between regions when cancelled or when a hard limit was reached
            if (StopRequested()) {
                break;
            }

            // 10) Run core algorithm on (bounds, toolBounds, finishingPass, clearedArea)
            ProcessPolyNode(boundPath, currentTBP, finishingPass, initialClearedPaths);
        }
    }

    // RayCAD: regions skipped as already cleared count as done
    if (progress && !StopRequested()) {
        progress(1.0);
    }
    return results;
}

bool Adaptive2d::FindEntryPoint(
    const Paths64& toolBoundPaths,
    const Paths64& boundPaths,
    ClearedArea& clearedArea /*output-initial cleared area by helix*/,
    Point64& entryPoint /*output*/,
    Point64& toolPos,
    PointD& toolDir,
    int64_t& helixRadiusScaled,
    AdaptiveOutput& adaptiveOutput
)
{
    Paths64 incOffset;
    Paths64 lastValidOffset;
    bool found = false;
    Paths64 clearedPaths;

    Paths64 checkPaths = Difference(toolBoundPaths, clearedArea.GetCleared(), FillRule::EvenOdd);

    for (int iter = 0; iter < 10; iter++) {
        double step = MIN_STEP_CLIPPER;
        double currentDelta = -1;
        incOffset = OffsetPaths(checkPaths, currentDelta, EndType::Polygon, JoinType::Square);
        while (!incOffset.empty()) {
            if (StopRequested()) {
                return false;  // RayCAD
            }
            incOffset = OffsetPaths(checkPaths, currentDelta, EndType::Polygon, JoinType::Square);
            if (!incOffset.empty()) {
                lastValidOffset = incOffset;
            }
            currentDelta -= step;
        }
        for (size_t i = 0; i < lastValidOffset.size(); i++) {
            if (!lastValidOffset[i].empty()) {
                entryPoint = Compute2DPolygonCentroid(lastValidOffset[i]);
                found = true;
                break;
            }
        }
        // check if the start point is in any of the holes
        // this may happen in case when toolBoundPaths are symmetric (boundary + holes)
        // we need to break simetry and try again
        for (size_t j = 0; j < checkPaths.size(); j++) {
            PointInPolygonResult pip = PointInPolygon(entryPoint, checkPaths[j]);
            if ((j == 0 && pip == PointInPolygonResult::IsOutside)
                || (j > 0 && pip != PointInPolygonResult::IsOutside)) {
                found = false;
                break;
            }
        }
        // check if helix fits
        const auto checkHelixFit = [&](int64_t testHelixRadiusScaled) {
            Path64 p1;
            p1.push_back(entryPoint);
            clearedPaths
                = OffsetPath(p1, (double)(testHelixRadiusScaled + toolRadiusScaled), EndType::Round);
            clearedPaths = SimplifyPaths(clearedPaths, CLEAN_POLYGONS_TOLERANCE);
            // we got first cleared area - check if it is crossing boundary
            Paths64 crossing = Difference(clearedPaths, boundPaths, FillRule::EvenOdd);

            return crossing.empty();
        };

        if (found) {
            // check that helix fits, and make initial polygon cleared by helix ramp
            if (!checkHelixFit(helixRampMinRadiusScaled)) {
                // min-size helix does not fit
                found = false;
            }
            else {
                // find the largest helix that fits
                // minSize = largest known fit; maxSize = largest possible fit
                int64_t minSize = helixRampMinRadiusScaled;
                int64_t maxSize = helixRampMaxRadiusScaled;
                while (minSize < maxSize) {
                    int64_t testSize = (minSize + maxSize + 1) / 2;  // always testSize > minSize
                    if (checkHelixFit(testSize)) {
                        minSize = testSize;
                    }
                    else {
                        maxSize = testSize - 1;  // always maxSize >= minSize
                    }
                }
                helixRadiusScaled = minSize;
                checkHelixFit(helixRadiusScaled);  // set clearedPaths for final size
                clearedArea.AddClearedPaths(clearedPaths);
            }
        }

        if (!found) {  // break simetry and try again
            Rect64 bounds = GetBounds(checkPaths);
            Path64 rect;
            rect.push_back(Point64(bounds.left, bounds.bottom));
            rect.push_back(Point64(bounds.left, (bounds.top + bounds.bottom) / 2));
            rect.push_back(Point64((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2));
            rect.push_back(Point64((bounds.left + bounds.right) / 2, bounds.bottom));
            checkPaths = Intersect(Paths64 {rect}, checkPaths, FillRule::EvenOdd);
        }
        if (found) {
            break;
        }
    }

    if (!found) {
        adaptiveOutput.StartPointNotFound = true;
    }
    if (found) {
        toolPos = Point64(entryPoint.x, entryPoint.y - helixRadiusScaled);
        toolDir = PointD(1.0, 0.0);
        adaptiveOutput.HelixEntry = true;  // RayCAD
        adaptiveOutput.HelixRadius = helixRadiusScaled;
    }
    return found;
}

//************************************************************
//  IsClearPath - returns true if path is clear from obstacles
//***********************************************************
bool Adaptive2d::IsClearPath(const Path64& tp, ClearedArea& cleared, double safetyClearance)
{
    Paths64 toolShape = OffsetPath(tp, toolRadiusScaled + safetyClearance, EndType::Round);
    Paths64 crossing = Difference(toolShape, cleared.GetCleared(), FillRule::EvenOdd);
    double collisionArea = 0;
    for (auto& p : crossing) {
        collisionArea += fabs(Area(p));
    }
    return collisionArea < 1.0;
}

bool Adaptive2d::IsAllowedToCutTrough(
    const Point64& p1,
    const Point64& p2,
    ClearedArea& cleared,
    const Paths64& toolBoundPaths,
    double areaFactor,
    bool skipBoundsCheck
)
{

    if (!skipBoundsCheck && !IsPointWithinCutRegion(toolBoundPaths, p2)) {
        // last point outside boundary - its not clear to cut
        return false;
    }
    else if (!skipBoundsCheck && !IsPointWithinCutRegion(toolBoundPaths, p1)) {
        // first point outside boundary - its not clear to cut
        return false;
    }
    else {
        double distance = sqrt(DistanceSqrd(p1, p2));
        double stepSize = min(0.5 * stepOverScaled, 8 * MIN_STEP_CLIPPER);
        if (distance < stepSize / 2) {  // not significant cut
            return true;
        }
        if (distance < stepSize) {  // adjust for numeric instability with small distances
            areaFactor *= 2;
        }

        Point64 toolPos1 = p1;
        int64_t steps = int64_t(distance / stepSize) + 1;  // a count: floor, as upstream
        stepSize = distance / double(steps);
        for (int64_t i = 1; i <= steps; i++) {
            if (StopRequested()) {
                return false;  // RayCAD
            }
            double p = double(i) / double(steps);
            Point64 toolPos2(Rnd(p1.x + double(p2.x - p1.x) * p), Rnd(p1.y + double(p2.y - p1.y) * p));
            double area = CalcCutArea(toolPos1, toolPos2, cleared).first;
            // if we are cutting above optimal -> not clear to cut
            if (area > areaFactor * stepSize * optimalCutAreaPD) {
                return false;
            }
            // if tool is outside boundary -> its not clear to cut
            if (!skipBoundsCheck && !IsPointWithinCutRegion(toolBoundPaths, toolPos2)) {
                return false;
            }
            toolPos1 = toolPos2;
        }
    }
    return true;
}

bool Adaptive2d::ResolveLinkPath(
    const Point64& startPoint,
    const Point64& endPoint,
    ClearedArea& clearedArea,
    Path64& output
)
{
    // Future work: reimplement this function with a modified version of the
    // algorithm from https://annas-archive.li/scidb/10.1002/net.3230140304/
    // 1) offset cleared area by tool radius, to effectively reduce the tool to a point
    // 2) actually further offset by whatever distance we want G0 to be from stock
    // 3) if start/end are outside that area, generate straight-line paths to them
    // 4) perform delaunay triangulation with clipper 2 (needs upgraded clipper!)
    // 5) do shortest path search along the triangulation from start to end,
    // producing a triangle strip (exact details tbd)
    // 6) finish the shortest path algorithm as specified in the paper
    // The result is O(n log n) shortest path (up to selection of the
    // appropriate triangle strip), which should be an improvement in both run
    // time and result over our current algorithm

    vector<pair<Point64, Point64>> queue;
    queue.emplace_back(startPoint, endPoint);
    Path64 checkPath;
    double totalLength = 0;
    double directDistance = sqrt(DistanceSqrd(startPoint, endPoint));
    Paths64 linkPaths;

    double scanStep = 2 * MIN_STEP_CLIPPER;
    if (scanStep > scaleFactor * 0.1) {
        scanStep = scaleFactor * 0.1;
    }
    if (scanStep < scaleFactor * 0.01) {
        scanStep = scaleFactor * 0.01;
    }
    int64_t limit = 10000;
    stats.linkSearches++;  // RayCAD

    double clearance = stepOverScaled;
    double offClearance = 2 * stepOverScaled;
    if (offClearance > directDistance / 2) {
        offClearance = directDistance / 2;
        clearance = 0;
    }

    int64_t cnt = 0;

    // to hold CLP results
    Point64 clp;
    size_t pindex = 0;
    size_t sindex = 0;
    double par = 0;

    // put a time limit on the resolving the link path
    // RayCAD: a limit on the segments examined (Limits::linkSearchSteps), not on wall-clock time: upstream gave up
    // after max(keepToolDownDistRatio, 3) / 6 seconds, so a loaded machine produced a different toolpath.
    int64_t sideSteps = 0;

    while (!queue.empty()) {
        if (StopRequested()) {
            return false;
        }
        if (cnt >= limits.linkSearchSteps) {
            stats.linkSearchLimitHits++;
            return false;
        }

        cnt++;
        stats.linkSearchSteps++;
        if (cnt > limit) {
            return false;
        }
        pair<Point64, Point64> pointPair = queue.back();
        queue.pop_back();

        // check for self intersections - if found discard the link path
        for (size_t i = 0; i < linkPaths.size(); i++) {
            if (linkPaths[i].front() != pointPair.first && linkPaths[i].back() != pointPair.first
                && linkPaths[i].front() != pointPair.second && linkPaths[i].back() != pointPair.second
                && IntersectionPoint(
                    linkPaths[i].front(),
                    linkPaths[i].back(),
                    pointPair.first,
                    pointPair.second,
                    clp
                )) {
                return false;
            }
        }

        PointD direction = DirectionV(pointPair.first, pointPair.second);
        checkPath.clear();
        if (pointPair.first == startPoint) {
            checkPath.push_back(Point64(
                Rnd(pointPair.first.x + offClearance * direction.x),
                Rnd(pointPair.first.y + offClearance * direction.y)
            ));
        }
        else {
            checkPath.push_back(pointPair.first);
        }
        if (pointPair.second == endPoint) {
            checkPath.push_back(Point64(
                Rnd(pointPair.second.x - offClearance * direction.x),
                Rnd(pointPair.second.y - offClearance * direction.y)
            ));
        }
        else {
            checkPath.push_back(pointPair.second);
        }

        if (IsClearPath(checkPath, clearedArea, clearance)) {
            totalLength += sqrt(DistanceSqrd(pointPair.first, pointPair.second));
            if (totalLength > keepToolDownDistRatio * directDistance) {
                return false;
            }
            Path64 link;
            link.push_back(pointPair.first);
            link.push_back(pointPair.second);
            linkPaths.push_back(link);
        }
        else {
            if (sqrt(DistanceSqrd(pointPair.first, pointPair.second)) < 4) {
                // segment became too short but still not clear
                return false;
            }
            PointD pDir(-direction.y, direction.x);
            // find mid point
            Point64 midPoint(
                0.5 * double(pointPair.first.x + pointPair.second.x),
                0.5 * double(pointPair.first.y + pointPair.second.y)
            );
            for (int64_t i = 1;; i++) {
                if (StopRequested()) {
                    return false;
                }
                // RayCAD: upstream probed until the offset passed keepToolDownDistRatio times the link's length,
                // with no other limit (a long link examined thousands of points, each against the whole cleared
                // area)
                stats.linkSideProbes++;
                if (++sideSteps > limits.linkSideSteps) {
                    stats.linkSearchLimitHits++;
                    return false;
                }
                double offset = double(i) * scanStep;
                Point64 checkPoint1(midPoint.x + offset * pDir.x, midPoint.y + offset * pDir.y);
                Point64 checkPoint2(midPoint.x - offset * pDir.x, midPoint.y - offset * pDir.y);

                if (
                    DistancePointToPathsSqrd(clearedArea.GetCleared(), checkPoint1, clp, pindex, sindex, par)
                    < DistancePointToPathsSqrd(clearedArea.GetCleared(), checkPoint2, clp, pindex, sindex, par)
                ) {
                    // exchange points
                    Point64 tmp = checkPoint2;
                    checkPoint2 = checkPoint1;
                    checkPoint1 = tmp;
                }

                checkPath.clear();
                checkPath.push_back(checkPoint1);
                if (IsClearPath(checkPath, clearedArea, clearance + 1)) {  // check if point clear
                    queue.emplace_back(pointPair.first, checkPoint1);
                    queue.emplace_back(checkPoint1, pointPair.second);
                    break;
                }
                else {  // check the other side

                    checkPath.clear();
                    checkPath.push_back(checkPoint2);
                    if (IsClearPath(checkPath, clearedArea, clearance + 1)) {
                        queue.emplace_back(pointPair.first, checkPoint2);
                        queue.emplace_back(checkPoint2, pointPair.second);
                        break;
                    }
                }
                if (offset > keepToolDownDistRatio * directDistance) {
                    return false;  // can't find keep tool down link
                }
            }
        }
    }
    if (linkPaths.empty()) {
        return false;
    }
    ConnectPaths(linkPaths, linkPaths);
    output = linkPaths[0];
    return true;
}

bool Adaptive2d::MakeLeadPath(
    bool leadIn,
    const Point64& startPoint,
    const PointD& startDir,
    Point64 beaconPoint,
    ClearedArea& clearedAreaOriginal,
    const Paths64& toolBoundPaths,
    Path64& output,
    AdaptiveOutput& adaptiveOutput
)
{
    output.push_back(startPoint);
    double stepSize = min(MIN_STEP_CLIPPER * 8, 0.2 * stepOverScaled + 1);

    // make a copy of clearedArea to update as the path progresses (for lead out only)
    ClearedArea clearedArea(toolRadiusScaled);
    clearedArea.SetClearedPaths(clearedAreaOriginal.GetCleared());

    // compute acceptable tool end locations
    Paths64 cleared = OffsetPaths(clearedArea.GetCleared(), -(toolRadiusScaled + stepSize));

    // move the beacon to an acceptable location if necessary
    if (cleared.size() == 0) {
        return false;
    }

    if (getPathNestingLevel(Path64 {beaconPoint}, cleared) % 2 == 0) {
        Point64 clp;  // to store closest point
        size_t clpPathIndex = 0;
        size_t clpSegmentIndex = 0;
        double clpParameter = 0;
        DistancePointToPathsSqrd(cleared, beaconPoint, clp, clpPathIndex, clpSegmentIndex, clpParameter);
        beaconPoint = clp;
    }

    Point64 currentPoint = startPoint;
    PointD targetDir = DirectionV(currentPoint, beaconPoint);

    double distanceToBeacon = sqrt(DistanceSqrd(startPoint, beaconPoint));
    double minExitLength = min(toolRadiusScaled / 5., min(stepOverScaled, distanceToBeacon / 2));
    double maxLength = max(distanceToBeacon * 2, stepSize * 10);
    std::optional<double> clearedStartLen;
    PointD nextDir = startDir;
    Point64 nextPoint
        = Point64(currentPoint.x + nextDir.x * stepSize, currentPoint.y + nextDir.y * stepSize);
    Path64 checkPath;
    double adaptFactor = 0.4;
    double alfa = std::numbers::pi / 64;
    double pathLen = 0;
    checkPath.push_back(currentPoint);
    for (int64_t i = 0; i < limits.leadSteps; i++) {  // RayCAD: upstream's 10000, as a limit
        if (StopRequested()) {
            return false;  // RayCAD
        }
        if (IsAllowedToCutTrough(currentPoint, nextPoint, clearedArea, toolBoundPaths)) {
            if (!leadIn) {
                // For lead out paths, update/recompute the cleared area
                checkPath.push_back(nextPoint);
                clearedArea.ExpandCleared(checkPath);
                checkPath.clear();
                checkPath.push_back(nextPoint);

                cleared = OffsetPaths(clearedArea.GetCleared(), -(toolRadiusScaled + stepSize));
            }

            output.push_back(nextPoint);
            currentPoint = nextPoint;
            pathLen += stepSize;
            targetDir = DirectionV(currentPoint, beaconPoint);
            nextDir = PointD(
                nextDir.x + adaptFactor * targetDir.x,
                nextDir.y + adaptFactor * targetDir.y
            );
            NormalizeV(nextDir);

            // check if cleared
            if (getPathNestingLevel(Path64 {currentPoint}, cleared) % 2 == 1) {
                if (!clearedStartLen) {
                    clearedStartLen = {pathLen};
                }

                // if the path is long enough, exit with success
                if (pathLen > minExitLength && pathLen - *clearedStartLen > MIN_STEP_CLIPPER) {
                    return true;
                }
            }
            else {
                clearedStartLen = {};
            }

            // if traveled too far without getting to a clear area, exit with failure
            if (pathLen > maxLength) {
                if (getPathNestingLevel(Path64 {currentPoint}, clearedArea.GetCleared()) % 2 == 1) {
                    return true;
                }
                else {
                    adaptiveOutput.LeadPathFailed = true;
                    return false;
                }
            }
        }
        else {
            nextDir = rotate(nextDir, leadIn ? -alfa : alfa);
        }
        nextPoint
            = Point64(currentPoint.x + nextDir.x * stepSize, currentPoint.y + nextDir.y * stepSize);
    }

    return false;
}

std::optional<TPaths> Adaptive2d::FindLinkPath(
    const std::optional<Point64>& prevPoint,
    const Point64& pathStart,
    const PointD& pathDir,
    ClearedArea& cleared,
    const Paths64& toolBoundPaths,
    AdaptiveOutput& adaptiveOutput
)
{
    TPaths result;

    Point64 endPoint(pathStart);

    // if the link distance is very short, no special linking is required
    double linkDistance = prevPoint ? sqrt(DistanceSqrd(*prevPoint, endPoint)) : stepOverScaled;
    if (linkDistance >= NTOL) {
        size_t clpPathIndex = 0;
        size_t clpSegmentIndex = 0;
        double clpParameter = 0;
        Point64 clp;

        double beaconOffset = max(min(stepOverScaled, linkDistance / 2) * 1.5, 8 * MIN_STEP_CLIPPER);

        // plan the lead in, as a reverse-direction lead out
        double eDistToBounds = DistancePointToPathsSqrd(
            toolBoundPaths,
            endPoint,
            clp,
            clpPathIndex,
            clpSegmentIndex,
            clpParameter
        );

        PointD revEndDir = {-pathDir.x, -pathDir.y};

        PointD endBoundaryDir = toolBoundPaths.empty()  // RayCAD: no boundary (upstream read out of range)
            ? pathDir
            : GetPathDirectionV(toolBoundPaths[clpPathIndex], clpSegmentIndex);
        if (eDistToBounds > beaconOffset) {
            endBoundaryDir = pathDir;  // if boundary is far away, use beacon to leave the path
        }
        PointD endBeaconDir = {revEndDir.x - endBoundaryDir.y, revEndDir.y + endBoundaryDir.x};
        NormalizeV(endBeaconDir);

        Point64 endBeacon(
            endPoint.x + beaconOffset * endBeaconDir.x,
            endPoint.y + beaconOffset * endBeaconDir.y
        );

        Path64 leadInPath;
        bool leadInOk = MakeLeadPath(
            true,
            endPoint,
            revEndDir,
            endBeacon,
            cleared,
            toolBoundPaths,
            leadInPath,
            adaptiveOutput
        );
        std::reverse(leadInPath.begin(), leadInPath.end());

        if (!leadInOk) {
            return {};
        }

        // Compute linking path
        Path64 linkPath;
        MotionType linkType = MotionType::mtCutting;

        if (prevPoint) {
            if (ResolveLinkPath(*prevPoint, leadInPath.front(), cleared, linkPath)) {
                linkType = MotionType::mtLinkClear;
                double remainingLeadInExtension = stepOverScaled / 2;
                while (linkPath.size() >= 2 && remainingLeadInExtension > NTOL) {
                    Point64 p1 = linkPath.at(linkPath.size() - 2);
                    Point64 p2 = linkPath.at(linkPath.size() - 1);
                    double l = sqrt(DistanceSqrd(p1, p2));
                    if (l >= remainingLeadInExtension) {
                        Point64 splitPoint(
                            p1.x + (p2.x - p1.x) * (l - remainingLeadInExtension) / l,
                            p1.y + (p2.y - p1.y) * (l - remainingLeadInExtension) / l
                        );
                        linkPath.pop_back();
                        linkPath.push_back(splitPoint);
                        leadInPath.insert(leadInPath.begin(), splitPoint);
                        remainingLeadInExtension = 0;
                        Path64 checkPath;
                        checkPath.push_back(p2);
                        checkPath.push_back(splitPoint);
                        if (!IsClearPath(checkPath, cleared, 0)) {
                            remainingLeadInExtension = stepOverScaled / 2;
                        }
                    }
                    else {
                        linkPath.pop_back();
                        leadInPath.insert(leadInPath.begin(), p1);
                        remainingLeadInExtension -= l;
                        if (remainingLeadInExtension < NTOL) {
                            Path64 checkPath;
                            checkPath.push_back(p2);
                            checkPath.push_back(p1);
                            if (!IsClearPath(checkPath, cleared, 0)) {
                                remainingLeadInExtension = stepOverScaled / 2;
                            }
                        }
                    }
                }
            }
            else {
                linkType = MotionType::mtLinkNotClear;
                double dist = sqrt(DistanceSqrd(*prevPoint, leadInPath.front()));
                if (dist >= NTOL  // RayCAD: upstream divided by a zero distance
                    && dist < 2 * stepOverScaled
                    && IsAllowedToCutTrough(
                        Point64(
                            prevPoint->x + (leadInPath.front().x - prevPoint->x) / dist,
                            prevPoint->y + (leadInPath.front().y - prevPoint->y) / dist
                        ),
                        Point64(
                            leadInPath.front().x - (leadInPath.front().x - prevPoint->x) / dist,
                            leadInPath.front().y - (leadInPath.front().y - prevPoint->y) / dist
                        ),
                        cleared,
                        toolBoundPaths
                    )) {
                    linkType = MotionType::mtCutting;
                }
                // add direct linking move at clear height

                linkPath.clear();
                linkPath.push_back(*prevPoint);
                linkPath.push_back(leadInPath.front());
            }
        }

        /* paths smoothing*/
        Paths64 linkPaths;
        linkPaths.push_back(linkPath);
        linkPaths.push_back(leadInPath);

        if (linkType == MotionType::mtLinkClear) {
            SmoothPaths(linkPaths, 0.1 * stepOverScaled, 1, 4);
        }

        linkPath = linkPaths[0];
        leadInPath = linkPaths[1];

        if (prevPoint) {
            // add linking path
            // RayCAD: in grid units (upstream: millimetre doubles); the caller adds the depth estimates of the link
            // it chooses (EstimateDepth)
            TPath linkPath1;
            linkPath1.motion = linkType;
            linkPath1.role = prLink;
            linkPath1.points = linkPath;
            result.push_back(linkPath1);
        }

        // add lead-in move
        TPath linkPath2;
        linkPath2.motion = MotionType::mtCutting;
        linkPath2.role = prLeadIn;
        linkPath2.points = leadInPath;

        result.push_back(linkPath2);
    }

    return {result};
}

std::optional<std::pair<Point64, PointD>> Adaptive2d::AppendToolPath(
    AdaptiveOutput& output,
    const TPath& passToolPathTagged,
    TPaths& linkPath,
    ClearedArea& cleared,
    const Paths64& toolBoundPaths
)
{
    std::optional<std::pair<Point64, PointD>> result;  // new toolPos and toolDir after lead out
    const Path64& passToolPath = passToolPathTagged.points;  // RayCAD: the pass comes with its role and depths


    for (TPath& lp : linkPath) {
        output.AdaptivePaths.push_back(lp);
    }

    TPath cutPath = passToolPathTagged;  // RayCAD: in grid units (upstream: millimetre doubles)
    cutPath.motion = MotionType::mtCutting;

    if (!cutPath.points.empty()) {
        output.AdaptivePaths.push_back(cutPath);

        // plan the lead out
        if (passToolPath.size() >= 2 && !toolBoundPaths.empty()) {  // RayCAD: a boundary to read
            Point64 prevPoint = passToolPath.back();
            PointD prevDir = GetPathDirectionV(passToolPath, passToolPath.size() - 1);

            size_t clpPathIndex = 0;
            size_t clpSegmentIndex = 0;
            double clpParameter = 0;
            Point64 clp;
            double distToBounds = DistancePointToPathsSqrd(
                toolBoundPaths,
                prevPoint,
                clp,
                clpPathIndex,
                clpSegmentIndex,
                clpParameter
            );

            PointD boundaryDir = GetPathDirectionV(toolBoundPaths[clpPathIndex], clpSegmentIndex);
            double beaconOffset
                = max(min(stepOverScaled, PathLength(passToolPath) / 2) * 1.5, 8 * MIN_STEP_CLIPPER);
            if (distToBounds > beaconOffset) {
                boundaryDir = prevDir;  // if boundary is far away, use beacon to leave the path
            }
            PointD beaconDir = {prevDir.x - boundaryDir.y, prevDir.y + boundaryDir.x};
            NormalizeV(beaconDir);

            Point64 beacon(
                prevPoint.x + beaconOffset * beaconDir.x,
                prevPoint.y + beaconOffset * beaconDir.y
            );

            Path64 leadOutPath;
            bool ok = MakeLeadPath(
                false,
                prevPoint,
                prevDir,
                beacon,
                cleared,
                toolBoundPaths,
                leadOutPath,
                output
            );

            if (ok && leadOutPath.size() >= 1) {
                // smooth path
                Paths64 leadOutPaths;
                leadOutPaths.push_back(leadOutPath);
                SmoothPaths(leadOutPaths, 0.1 * stepOverScaled, 1, 4);
                leadOutPath = leadOutPaths[0];

                // scale and output
                // RayCAD: in grid units, with the depth estimate against the cleared area before it
                TPath out;
                out.motion = MotionType::mtCutting;
                out.role = prLeadOut;
                out.points = leadOutPath;
                out.depth = EstimateDepth(leadOutPath, cleared);
                output.AdaptivePaths.push_back(out);
                cleared.ExpandCleared(leadOutPath);

                Point64 p2 = leadOutPath.back();
                Point64 p1 = leadOutPath.size() >= 2 ? leadOutPath[leadOutPath.size() - 2]
                                                     : prevPoint;
                PointD dir = DirectionV(p1, p2);

                result = {{p2, dir}};
            }
        }
    }

    return result;
}

// RayCAD: upstream's CheckReportProgress (a callback with a copy of the progress paths every 0.1 s of clock()) and
// AddPathsToProgress are replaced by these.
bool Adaptive2d::StopRequested()
{
    if (!stopProcessing && cancel != nullptr && cancel->cancelled()) {
        stopProcessing = true;
    }
    return stopProcessing;
}

void Adaptive2d::HitLimit(AdaptiveOutput& output, const char* name)
{
    output.LimitReached = true;
    output.LimitName = name;
    stopProcessing = true;
}

void Adaptive2d::ReportProgress(double regionFraction)
{
    if (!progress || region_count <= 0) {
        return;
    }
    double f = (double(current_region - 1) + max(0.0, min(1.0, regionFraction))) / double(region_count);
    progress(max(0.0, min(1.0, f)));
}

std::vector<double> Adaptive2d::EstimateDepth(const Path64& path, ClearedArea& cleared)
{
    // The cut area per unit of travel, as the step search measures it (CalcCutArea against the cleared area, the
    // disc at the previous position counting as cut), over pieces no longer than the longest step the search takes,
    // the largest over the pieces of each segment.
    std::vector<double> depth(path.size(), -1.0);
    const double maxPiece = 8 * MIN_STEP_CLIPPER;
    for (size_t i = 1; i < path.size(); i++) {
        if (StopRequested()) {
            break;
        }
        const Point64& a = path[i - 1];
        const Point64& b = path[i];
        double len = sqrt(DistanceSqrd(a, b));
        if (len < NTOL) {
            depth[i] = 0;
            continue;
        }
        int64_t pieces = int64_t(ceil(len / maxPiece));
        double best = 0;
        Point64 from = a;
        for (int64_t k = 1; k <= pieces; k++) {
            double t = double(k) / double(pieces);
            Point64 to = k == pieces ? b : Point64(Rnd(a.x + double(b.x - a.x) * t), Rnd(a.y + double(b.y - a.y) * t));
            double pieceLen = sqrt(DistanceSqrd(from, to));
            if (pieceLen >= NTOL) {
                best = max(best, CalcCutArea(from, to, cleared, false).first / pieceLen);
            }
            from = to;
        }
        depth[i] = best;
    }
    return depth;
}
// performs the intersection of the path (subject) and the area (obj), preserving
// orientation and (closed-path) connectivity
Paths64 PathIntersectArea(const Path64& subject, const Paths64& obj, bool isClosed = true)
{
    if (subject.empty()) {
        return {};  // RayCAD: upstream read the first point of an empty path
    }
    // local copies, since the z-data below must be overwritten
    Path64 indexedSubject = subject;
    Paths64 clipObj = obj;

    if (isClosed) {
        // close path explicitly before treating it as open
        indexedSubject.push_back(indexedSubject[0]);
    }

    // init z-data: p[i].z = 2 * i + 1, and new points are the average of their neighbors
    // this ensures new points have unique z but come between the points they're made from
    for (size_t i = 0; i < indexedSubject.size(); i++) {
        indexedSubject[i].z = i * 2 + 1;
    }
    for (Path64& path : clipObj) {
        for (Point64& p : path) {
            p.z = 0;
        }
    }
    auto zfill =
        [](const Point64& e1b, const Point64& e1t, const Point64& e2b, const Point64& e2t, Point64& p) {
            // If the x/y value is an exact match preserve z
            if (e1b.z != 0 && e1b.x == p.x && e1b.y == p.y) {
                p.z = e1b.z;
            }
            else if (e1t.z != 0 && e1t.x == p.x && e1t.y == p.y) {
                p.z = e1t.z;
            }
            else if (e2b.z != 0 && e2b.x == p.x && e2b.y == p.y) {
                p.z = e2b.z;
            }
            else if (e2t.z != 0 && e2t.x == p.x && e2t.y == p.y) {
                p.z = e2t.z;
            }
            // Otherwise, set the z to the even number in between
            else if (e1b.z != 0 && e1t.z != 0) {
                p.z = (e1b.z + e1t.z) / 2;
            }
            else if (e2b.z != 0 && e2t.z != 0) {
                p.z = (e2b.z + e2t.z) / 2;
            }
        };

    Clipper64 clip;
    clip.SetZCallback(zfill);
    clip.AddOpenSubject(Paths64 {indexedSubject});
    clip.AddClip(clipObj);
    Paths64 closed, diff;
    clip.Execute(ClipType::Intersection, FillRule::EvenOdd, closed, diff);

    // restore orientation
    for (Path64& p : diff) {
        for (size_t i = 0; i + 1 < p.size(); i++) {  // RayCAD: no underflow on an empty path
            if (p[i].z != 0 && p[i + 1].z != 0) {
                if (p[i].z + 1 != p[i + 1].z && p[i].z + 2 != p[i + 1].z) {
                    std::reverse(p.begin(), p.end());
                }

                break;
            }
        }
    }

    // collect result, joining any path that goes through the end point
    const int64_t zstart = 1;
    const int64_t zend = int64_t(indexedSubject.size()) * 2 - 1;
    std::optional<Path64> start, end;
    Paths64 result;
    for (Path64& p : diff) {
        if (p.empty()) {
            continue;  // RayCAD
        }
        if (p[0].z == zstart) {
            start = {p};
        }
        else if (p.back().z == zend) {
            end = {p};
        }
        else {
            result.push_back(p);
        }
    }
    if (start && end) {
        Path64 joined = *end;
        // append points from start, skipping the first, which is a repeat
        for (size_t i = 1; i < start->size(); i++) {
            joined.push_back((*start)[i]);
        }
        result.push_back(joined);
    }
    else {
        if (start) {
            result.push_back(*start);
        }
        if (end) {
            result.push_back(*end);
        }
    }

    return result;
}

struct IterateNextStepOutput
{
    // RayCAD: every field initialised
    std::optional<double> iterationAngle;
    bool tooManyIterations = false;
    bool failed = false;
    double area = 0;
    double errorFraction = 0;
    Point64 newToolPos;
    PointD newToolDir;
};

void Adaptive2d::ProcessPolyNode(
    Paths64 boundPaths,
    Paths64 toolBoundPaths,
    Paths64 finishingPaths,
    const Paths64& initialClearedPaths
)
{
    current_region++;

    // node paths are already constrained to tool boundary path for adaptive path before finishing
    // pass
    Point64 entryPoint;
    int64_t helixRadiusScaled = 0;

    toolBoundPaths = SimplifyPaths(toolBoundPaths, CLEAN_POLYGONS_TOLERANCE);
    toolBoundPaths = Union(toolBoundPaths, FillRule::EvenOdd);

    boundPaths = SimplifyPaths(boundPaths, CLEAN_POLYGONS_TOLERANCE);
    boundPaths = Union(boundPaths, FillRule::EvenOdd);

    // toolBoundPaths shrunk by some buffer room: sqrt(2) allowance each for a difference and an offset
    Paths64 tbpMinus = OffsetPaths(toolBoundPaths, -ceil(2 * std::numbers::sqrt2));
    tbpMinus = SimplifyPaths(tbpMinus, CLEAN_POLYGONS_TOLERANCE);
    tbpMinus = Union(tbpMinus, FillRule::EvenOdd);

    Point64 toolPos;
    PointD toolDir;

    // Initialize cleared area from previously cleared paths
    ClearedArea cleared(toolRadiusScaled);
    cleared.SetClearedPaths(initialClearedPaths);

    int64_t stepScaled = int64_t(MIN_STEP_CLIPPER);

    // RayCAD: the area this region has to clear, for progress
    double areaToClear = 0;
    for (const Path64& p : Difference(boundPaths, initialClearedPaths, FillRule::EvenOdd)) {
        areaToClear += Area(p);
    }
    double clearedAtStart = 0;
    for (const Path64& p : cleared.GetCleared()) {
        clearedAtStart += Area(p);
    }
    ReportProgress(0);

    Path64 passToolPath;  // to store pass toolpath
    std::vector<double> passDepth;  // RayCAD: the cut area per unit of travel of each step of the pass
    Path64 toClearPath;
    Point64 clp;  // to store closest point
    size_t clpPathIndex = 0;
    size_t clpSegmentIndex = 0;
    double clpParameter = 0;
    vector<PointD> gyro;          // used to average tool direction
    vector<double> angleHistory;  // use to predict deflection angle
    double angle = std::numbers::pi;
    Interpolation interp;  // interpolation instance

    int64_t total_iterations = 0;
    int64_t total_points = 0;
    int64_t total_exceeded = 0;
    int64_t total_output_points = 0;
    int64_t over_cut_count = 0;
    int64_t bad_engage_count = 0;

    AdaptiveOutput output;
    output.clipperScale = double(scaleFactor);
    output.ToolBoundPaths = toolBoundPaths;  // RayCAD
    ClearedArea clearedBeforePass(toolRadiusScaled);
    clearedBeforePass.SetClearedPaths(cleared.GetCleared());

    PointD lastExpandToolDir = toolDir;

    const auto iterateNextStep = [&](const Point64& toolPos, const PointD& toolDir, bool warnRotate) {
        IterateNextStepOutput out;
        out.tooManyIterations = false;
        out.failed = false;
        // RayCAD: the hard limit on steps
        if (++stats.steps > limits.steps) {
            HitLimit(output, "steps");
        }
        if (StopRequested()) {
            out.failed = true;
            return out;
        }


        double distanceToBoundary = sqrt(
            DistancePointToPathsSqrd(toolBoundPaths, toolPos, clp, clpPathIndex, clpSegmentIndex, clpParameter)
        );
        PointD boundaryDir = GetPathDirectionV(toolBoundPaths[clpPathIndex], clpSegmentIndex);

        double distanceToEngage = sqrt(DistanceSqrd(toolPos, entryPoint));

        double targetAreaPD = optimalCutAreaPD;

        // set the step size: 1x to 8x base size
        double slowDownDistance = max(double(toolRadiusScaled) / 4, MIN_STEP_CLIPPER * 8);
        // RayCAD: step lengths are counts of grid units; the conversions floor as upstream's did
        if (distanceToBoundary < slowDownDistance || distanceToEngage < slowDownDistance) {
            stepScaled = int64_t(MIN_STEP_CLIPPER);
        }
        else if (fabs(angle) > NTOL) {
            stepScaled = int64_t(min(MIN_STEP_CLIPPER / fabs(angle), 1e9));
        }
        else {
            stepScaled = int64_t(MIN_STEP_CLIPPER * 8);
        }

        // clamp the step size - for stability
        if (stepScaled > min(int64_t(toolRadiusScaled / 4), int64_t(MIN_STEP_CLIPPER * 8))) {
            stepScaled = min(int64_t(toolRadiusScaled / 4), int64_t(MIN_STEP_CLIPPER * 8));
        }
        if (double(stepScaled) < MIN_STEP_CLIPPER) {
            stepScaled = int64_t(MIN_STEP_CLIPPER);
        }

        //*****************************
        // ANGLE vs AREA ITERATIONS
        //*****************************
        double predictedAngle = averageDV(angleHistory);
        double maxError = AREA_ERROR_FACTOR * optimalCutAreaPD;
        double errorFraction = 1;
        double area = 0;
        bool isConventional = false;
        const double conventionalCutoff = 0.51;  // allow some room for rounding, but otherwise < 50%
        double areaPD = 0;
        interp.clear();
        /******************************/
        int iteration;
        bool pointNotInterp = true;  // RayCAD: initialised
        bool foundArea = false;
        Point64 newToolPos;
        PointD newToolDir;
        for (iteration = 0; iteration < MAX_ITERATIONS; iteration++) {
            if (StopRequested()) {
                out.failed = true;  // RayCAD
                return out;
            }
            total_iterations++;
            if (iteration == 0) {
                angle = predictedAngle;
                pointNotInterp = true;
            }
            else if (iteration == 1) {
                angle = interp.MIN_ANGLE;  // max engage
                pointNotInterp = true;
            }
            else if (iteration == 2) {
                if (interp.bothSides()) {
                    angle = interp.interpolateAngle();
                    pointNotInterp = false;
                }
                else {
                    angle = interp.MAX_ANGLE;  // min engage
                    pointNotInterp = true;
                }
            }
            else if (iteration == 3 && !foundArea) {
                // Expand cleared area
                cleared.ExpandCleared(toClearPath);
                toClearPath.clear();
                lastExpandToolDir = toolDir;

                // Find nearby uncleared area in the forward direction
                Paths64 clearedArea = cleared.GetCleared();

                // 1.5 > sqrt(2) for the constructed triangle to contain possible steps
                double dist = (stepScaled + toolRadiusScaled) * 1.5;
                PointD leftAngle = rotate(toolDir, -std::numbers::pi / 4);
                PointD rightAngle = rotate(toolDir, std::numbers::pi / 4);
                Path64 triangle = {toolPos};
                triangle.push_back(
                    Point64 {
                        Rnd(toolPos.x + rightAngle.x * dist),
                        Rnd(toolPos.y + rightAngle.y * dist)
                    }
                );
                triangle.push_back(
                    Point64 {
                        Rnd(toolPos.x + leftAngle.x * dist),
                        Rnd(toolPos.y + leftAngle.y * dist)
                    }
                );

                clearedArea = Difference(Paths64 {triangle}, clearedArea, FillRule::EvenOdd);

                if (clearedArea.empty()) {
                    continue;
                }

                // Find the closest point on the boundary, and try stepping towards it
                DistancePointToPathsSqrd(
                    clearedArea,
                    toolPos,
                    clp,
                    clpPathIndex,
                    clpSegmentIndex,
                    clpParameter
                );
                double dy = double(clp.y - toolPos.y);
                double dx = double(clp.x - toolPos.x);
                double len = sqrt(dx * dx + dy * dy);
                if (len < NTOL) {
                    continue;  // RayCAD: the closest point is the tool position (upstream: asin of 0 / 0)
                }
                angle = dmath::asin(max(-1.0, min(1.0, (dy * toolDir.x - dx * toolDir.y) / len)));
            }
            else if (!foundArea) {
                // if the previous iteration didn't cut area then nothing will; exit early
                angle = 0;
                area = 0;
                areaPD = 0;
                break;
            }
            else {
                angle = interp.interpolateAngle();
                pointNotInterp = false;
            }
            angle = interp.clampAngle(angle);

            newToolDir = rotate(toolDir, angle);
            newToolPos = Point64(
                Rnd(toolPos.x + newToolDir.x * stepScaled),
                Rnd(toolPos.y + newToolDir.y * stepScaled)
            );

            // Skip iteration if this Point64 has already been processed
            bool intRepeat = false;
            if (interp.m_min && newToolPos == interp.m_min->angle.second) {
                interp.m_min = InterpItem {
                    {angle, newToolPos},
                    interp.m_min->error,
                    interp.m_min->isConventional
                };
                intRepeat = true;
            }
            if (interp.m_max && newToolPos == interp.m_max->angle.second) {
                interp.m_max = InterpItem {
                    {angle, newToolPos},
                    interp.m_max->error,
                    interp.m_max->isConventional
                };
                intRepeat = true;
            }

            if (intRepeat) {
                if (interp.m_min && interp.m_max
                    && abs(interp.m_min->angle.second.x - interp.m_max->angle.second.x) <= 1
                    && abs(interp.m_min->angle.second.y - interp.m_max->angle.second.y) <= 1) {
                    if (pointNotInterp) {
                        // if this happens while testing min/max of the range it doesn't mean
                        // anything; only exit early if interpolation is down to adjacent
                        // integers
                        continue;
                    }
                    // exit early, selecting the better of the two adjacent integers
                    double error;
                    if (interp.m_min->isConventional ^ interp.m_max->isConventional) {
                        if (!interp.m_min->isConventional) {
                            newToolDir = rotate(toolDir, interp.m_min->angle.first);
                            newToolPos = interp.m_min->angle.second;
                            error = interp.m_min->error;
                            isConventional = interp.m_min->isConventional;
                        }
                        else {
                            newToolDir = rotate(toolDir, interp.m_max->angle.first);
                            newToolPos = interp.m_max->angle.second;
                            error = interp.m_max->error;
                            isConventional = interp.m_max->isConventional;
                        }
                    }
                    else if (abs(interp.m_min->error) < abs(interp.m_max->error)) {
                        newToolDir = rotate(toolDir, interp.m_min->angle.first);
                        newToolPos = interp.m_min->angle.second;
                        error = interp.m_min->error;
                        isConventional = interp.m_min->isConventional;
                    }
                    else {
                        newToolDir = rotate(toolDir, interp.m_max->angle.first);
                        newToolPos = interp.m_max->angle.second;
                        error = interp.m_max->error;
                        isConventional = interp.m_max->isConventional;
                    }
                    areaPD = error + targetAreaPD;
                    area = areaPD * double(stepScaled);
                    out.iterationAngle = angle;
                    break;
                }
                continue;
            }

            const auto caRet = CalcCutArea(toolPos, newToolPos, cleared);
            area = std::get<0>(caRet);
            double conventionalArea = std::get<1>(caRet);
            double fractionConventional = (area == 0) ? 0 : conventionalArea / area;
            isConventional = fractionConventional >= conventionalCutoff;
            if (area > 0) {
                foundArea = true;
            }

            areaPD = area / double(stepScaled);  // area per distance
            double error = areaPD - targetAreaPD;
            errorFraction = abs(error / optimalCutAreaPD);
            interp.addPoint(error, {angle, newToolPos}, pointNotInterp, isConventional);
            if (fabs(error) < maxError && !isConventional) {
                out.iterationAngle = angle;
                break;
            }
            if (iteration == MAX_ITERATIONS - 1) {
                out.tooManyIterations = true;
            }
        }

        bool recalcArea = false;

        if (area > 0) {
            //**********************************************
            // CHECK AND RECORD NEW TOOL POS
            //**********************************************
            int64_t rotateStep = 0;
            double rotateIncrement;
            {
                double boundaryAngle = dmath::atan2(boundaryDir.y, boundaryDir.x);
                double toolAngle = dmath::atan2(newToolDir.y, newToolDir.x);
                double delta = boundaryAngle - toolAngle;
                if (delta > std::numbers::pi) {
                    delta -= 2 * std::numbers::pi;
                }
                if (delta < -std::numbers::pi) {
                    delta += 2 * std::numbers::pi;
                }
                rotateIncrement = (delta > 0 ? 1 : -1) * std::numbers::pi / 90;
            }
            while (!IsPointWithinCutRegion(toolBoundPaths, newToolPos) && rotateStep < 180) {
                rotateStep++;
                // if new tool pos. outside boundary rotate until back in
                recalcArea = true;
                newToolDir = rotate(newToolDir, rotateIncrement);
                newToolPos = Point64(
                    Rnd(toolPos.x + newToolDir.x * stepScaled),
                    Rnd(toolPos.y + newToolDir.y * stepScaled)
                );
            }
            if (rotateStep >= 180) {
                UNUSED(warnRotate);  // RayCAD: upstream's DEV_MODE warning is gone
                out.failed = true;
            }

            if (recalcArea) {
                const auto caRet = CalcCutArea(toolPos, newToolPos, cleared);
                area = std::get<0>(caRet);
                areaPD = area / double(stepScaled);  // area per distance
                double error = areaPD - targetAreaPD;
                errorFraction = abs(error / optimalCutAreaPD);

                double conventionalArea = std::get<1>(caRet);
                double fractionConventional = area == 0 ? 0 : conventionalArea / area;
                isConventional = fractionConventional >= conventionalCutoff;
            }

            // safety condition
            if (area > double(stepScaled) * optimalCutAreaPD && areaPD > 2 * optimalCutAreaPD) {
                over_cut_count++;
                out.failed = true;
            }
        }

        out.area = area;
        out.failed |= isConventional;
        out.failed |= area < 1;
        out.newToolPos = newToolPos;
        out.newToolDir = newToolDir;
        out.errorFraction = errorFraction;
        return out;
    };

    const auto initToolDir = [&](const Point64& toolPos, const PointD& baseDir) {
        PointD testDirs[] = {
            {baseDir.x, baseDir.y},
            {-baseDir.y, baseDir.x},
            {-baseDir.x, -baseDir.y},
            {baseDir.y, -baseDir.x}
        };
        std::optional<std::pair<PointD, double>> bestDir;
        bool allZero = true;
        for (const auto& testDir : testDirs) {
            const auto itResult = iterateNextStep(toolPos, testDir, false);
            if (itResult.area != 0) {
                allZero = false;
            }
            if (!itResult.failed) {
                if (!bestDir || itResult.errorFraction < bestDir->second) {
                    bestDir = {itResult.newToolDir, itResult.errorFraction};
                }
            }
        }

        if (bestDir) {
            return std::optional<PointD> {bestDir->first};
        }
        else if (allZero) {
            Paths64 clearedArea = cleared.GetCleared();
            if (DistancePointToPathsSqrd(clearedArea, toolPos, clp, clpPathIndex, clpSegmentIndex, clpParameter)
                < double(toolRadiusScaled * toolRadiusScaled)) {
                Point64 p2 = Compute2DPolygonCentroid(clearedArea[clpPathIndex]);
                return std::optional<PointD> {DirectionV(toolPos, p2)};
            }
            return std::optional<PointD> {};
        }
        else {
            return std::optional<PointD> {};
        }
    };

    const auto _getEngagePoint = [&](const std::optional<Point64>& prevPos,
                                     int64_t engagementProtrusion) {
        stats.engageSearches++;  // RayCAD
        // engagePoint, engageDir, heuristicCost
        std::vector<std::tuple<Point64, PointD, double>> engagePoints;

        const auto addEngagePoint = [&](const Point64& engagePoint, const PointD& engageDir) {
            double cost_mm = (prevPos ? sqrt(DistanceSqrd(*prevPos, engagePoint)) / double(scaleFactor) : 0);
            engagePoints.emplace_back(
                std::tuple<Point64, PointD, double> {engagePoint, engageDir, cost_mm}
            );
        };

        Paths64 clearedArea = cleared.GetCleared();

        // offset inward to find places the tool can start
        int64_t engageBuffer = 2;  // smooths out the integer rounding in the two offsets, +toolRadius
                                   // and -toolRadius
        Paths64 preEngage = OffsetPaths(clearedArea, -double(toolRadiusScaled + engageBuffer));

        // offset outward to find places that would protrude outside the cleared area
        Paths64 engagePaths = OffsetPaths(preEngage, double(engageBuffer + engagementProtrusion));

        // clip engage candidates with tool bounds
        for (Path64& engagePath : engagePaths) {
            // rotate the closed path so it starts with the closest point
            // this is useful because if the path does not get clipped, any point on
            // the path is a valid start location (not just the first) but we want to
            // test against the closest one
            Path64 rotated;
            if (!prevPos) {
                rotated = engagePath;
            }
            else {
                size_t iClosest = 0;
                double dsqClosest = __DBL_MAX__;
                for (size_t i = 0; i < engagePath.size(); i++) {
                    double dsq = DistanceSqrd(*prevPos, engagePath[i]);
                    if (dsq < dsqClosest) {
                        dsqClosest = dsq;
                        iClosest = i;
                    }
                }

                for (size_t i = 0; i < engagePath.size(); i++) {
                    rotated.push_back(engagePath[(i + iClosest) % engagePath.size()]);
                }
            }

            // clip with tbpMinus instead of toolBoundPaths to ensure that all
            // resulting points are inside toolBoundPaths, and not rounded to an
            // integer coordinate outside of it
            Paths64 openPaths = PathIntersectArea(rotated, tbpMinus);

            for (Path64& open : openPaths) {
                bool added = false;
                double dToGo = 0;  // first step is 0 -- start point
                size_t seg = 0;
                double segD = 0;
                while (!added && seg + 1 < open.size()) {  // RayCAD: no underflow on an empty path
                    if (StopRequested()) {
                        break;  // RayCAD
                    }
                    // step to next point
                    Point64 p;
                    PointD segDir;
                    do {
                        Point64 p1 = open[seg];
                        Point64 p2 = open[seg + 1];
                        double segLen = sqrt(DistanceSqrd(p1, p2));
                        if (segLen >= NTOL) {  // RayCAD: no direction from a zero-length segment (upstream: NaN)
                            segDir = PointD {double(p2.x - p1.x) / segLen, double(p2.y - p1.y) / segLen};
                        }
                        if (segLen - segD > dToGo) {
                            // interpolate current segment
                            segD += dToGo;
                            dToGo = 0;
                            double interp = segD / segLen;
                            p = Point64 {
                                Rnd(p2.x * interp + p1.x * (1 - interp)),
                                Rnd(p2.y * interp + p1.y * (1 - interp))
                            };
                        }
                        else {
                            dToGo -= segLen - segD;
                            segD = 0;
                            seg++;
                            p = p2;  // ensures that we try the endpoint too
                        }
                    } while (dToGo > 0 && seg + 1 < open.size());

                    // Attempt to add the point
                    const auto toolDir = initToolDir(p, segDir);
                    if (toolDir) {
                        addEngagePoint(p, *toolDir);
                        added = true;
                    }

                    dToGo = MIN_STEP_CLIPPER;  // all subsequent steps are MIN_STEP_CLIPPER
                }
            }
        }

        // sort engagePoints based on connection cost
        // RayCAD: stable, so engagement points of equal cost keep their order on every standard library
        std::stable_sort(
            engagePoints.begin(),
            engagePoints.end(),
            [](const std::tuple<Point64, PointD, double>& aa, const std::tuple<Point64, PointD, double>& bb) {
                return std::get<double>(aa) < std::get<double>(bb);
            }
        );

        double bestCost = __DBL_MAX__;
        TPaths bestLink;
        Point64 bestPos;
        PointD bestDir;

        for (const auto& ep : engagePoints) {
            if (StopRequested()) {
                break;  // RayCAD
            }
            if (std::get<double>(ep) < bestCost) {
                std::optional<TPaths> link = FindLinkPath(
                    prevPos,
                    std::get<Point64>(ep),
                    std::get<PointD>(ep),
                    cleared,
                    toolBoundPaths,
                    output
                );
                if (!link) {
                    continue;
                }

                // RayCAD: the paths are in grid units; the cost stays in millimetres
                double cost_mm = 0;
                std::optional<Point64> prev = prevPos;
                for (const TPath& tp : *link) {
                    if (tp.motion == MotionType::mtLinkNotClear) {
                        cost_mm += 10000;  // prioritize links that don't require retraction
                    }
                    for (size_t i = 0; i < tp.points.size(); i++) {
                        const Point64& cur = tp.points[i];
                        if (prev) {
                            cost_mm += sqrt(DistanceSqrd(cur, *prev)) / double(scaleFactor);
                        }
                        prev = {cur};
                    }
                }

                if (cost_mm < bestCost) {
                    bestCost = cost_mm;
                    bestLink = *link;
                    bestPos = std::get<Point64>(ep);
                    bestDir = std::get<PointD>(ep);
                }
            }
        }

        if (bestCost < __DBL_MAX__) {
            return std::optional<std::tuple<Point64, PointD, TPaths>> {{bestPos, bestDir, bestLink}};
        }
        else {
            return std::optional<std::tuple<Point64, PointD, TPaths>> {};
        }
    };

    const auto getEngagePoint = [&](const std::optional<Point64>& prevPos) {

        // Compute how far into the material the first engagement should be
        const double targetArea = optimalCutAreaPD * MIN_STEP_CLIPPER;
        // Area of a segment of a circle: A = R^2 / 2 * (theta - sin(theta))
        // 2nd order Taylor expansion: A = R^2 / 2 * (theta^3/6) = R^2 * theta^3 / 12
        // Solve for theta: theta = (12 * A / R^2)^(1/3)
        const double theta = CubeRoot(12 * targetArea / double(toolRadiusScaled) / double(toolRadiusScaled));
        const double protrusion = double(toolRadiusScaled) - dmath::cos(theta / 2) * double(toolRadiusScaled);
        const int64_t engagementProtrusion
            = Rnd(min(protrusion, stepOverScaled * FINISHING_THICKNESS_SCALE));

        // Get engagement point. First attempt with the desired offsets, then fallback
        auto result = _getEngagePoint(prevPos, engagementProtrusion);
        if (!result) {
            // consider retry at tiny protrusion?
            // result = _getEngagePoint(prevPos, 4);
        }

        // update cleared area
        if (result) {
            // RayCAD: the depth estimates of the chosen link and lead-in, against the area cleared before them
            for (TPath& linkPath : std::get<TPaths>(*result)) {
                linkPath.depth = EstimateDepth(linkPath.points, cleared);
            }
            for (const TPath& linkPath : std::get<TPaths>(*result)) {
                if (linkPath.motion == MotionType::mtCutting) {
                    cleared.ExpandCleared(linkPath.points);  // RayCAD: in grid units, no conversion
                }
            }
        }

        return result;
    };

    std::optional<std::tuple<Point64, PointD, TPaths>> engagePoint = getEngagePoint(std::nullopt);
    TPaths linkPath;
    if (engagePoint) {
        toolPos = std::get<Point64>(*engagePoint);
        toolDir = std::get<PointD>(*engagePoint);
        linkPath = std::get<TPaths>(*engagePoint);
        entryPoint = linkPath.size() > 0 && !linkPath[0].points.empty()
            ? linkPath[0].points[0]
            : toolPos;
        output.StartPoint = entryPoint;  // RayCAD: grid units
    }
    else {
        // Engagement failed; instead helix down
        if (!FindEntryPoint(
                toolBoundPaths,
                boundPaths,
                cleared,
                entryPoint,
                toolPos,
                toolDir,
                helixRadiusScaled,
                output
            )) {
            if (StopRequested()) {
                output.StartPointNotFound = false;  // RayCAD: cancelled or stopped, not a failed start
                output.Stopped = true;
            }
            output.ClearedPaths = cleared.GetCleared();
            results.push_back(output);
            return;
        }
        output.StartPoint = toolPos;  // RayCAD: grid units
    }

    output.ReturnMotionType = 0;
    output.HelixCenterPoint = entryPoint;  // RayCAD: grid units

    //*******************************
    // LOOP - PASSES
    //*******************************
    for (int64_t pass = 0;; pass++) {
        if (StopRequested()) {
            break;
        }
        if (pass >= limits.passes) {
            HitLimit(output, "passes");  // RayCAD: upstream's limit was LONG_MAX
            break;
        }
        stats.passes++;

        passToolPath.clear();
        passDepth.clear();
        toClearPath.clear();
        angleHistory.clear();
        angleHistory.push_back(0);

        // include linking path in cleared area
        for (const TPath& lp : linkPath) {
            if (lp.motion == MotionType::mtCutting || lp.motion == MotionType::mtLinkClear) {
                cleared.ExpandCleared(lp.points);  // RayCAD: in grid units, no conversion
            }
        }

        angle = std::numbers::pi / 4;  // initial pass angle
        double cumulativeCutArea = 0;
        // init gyro
        gyro.clear();
        for (int i = 0; i < DIRECTION_SMOOTHING_BUFLEN; i++) {
            gyro.push_back(toolDir);
        }

        //*******************************
        // LOOP - POINTS
        //*******************************
        for (int64_t point_index = 0;; point_index++) {
            if (StopRequested()) {
                break;
            }
            if (point_index >= limits.pointsPerPass) {
                HitLimit(output, "points per pass");  // RayCAD: upstream's limit was LONG_MAX
                break;
            }

            total_points++;
            AverageDirection(gyro, toolDir);

            const auto itResult = iterateNextStep(toolPos, toolDir, true);

            if (itResult.tooManyIterations) {
                total_exceeded++;
            }

            if (!itResult.failed) {  // cut is ok - record it
                if (itResult.iterationAngle) {
                    angleHistory.push_back(*itResult.iterationAngle);
                    if (angleHistory.size() > ANGLE_HISTORY_POINTS) {
                        angleHistory.erase(angleHistory.begin());
                    }
                }

                // if the path has changed direction by more than 45 degrees (such that we consider
                // continuations (>45)+45>90 degrees from the original direction) then we need to
                // update cleared paths
                if (lastExpandToolDir.x * itResult.newToolDir.x
                        + lastExpandToolDir.y * itResult.newToolDir.y
                    < dmath::cos(std::numbers::pi / 4)) {
                    cleared.ExpandCleared(toClearPath);
                    toClearPath.clear();
                    lastExpandToolDir = toolDir;
                }

                if (toClearPath.empty()) {
                    toClearPath.push_back(toolPos);
                }
                toClearPath.push_back(itResult.newToolPos);

                cumulativeCutArea += itResult.area;

                // append to toolpaths
                if (passToolPath.empty()) {
                    passToolPath.push_back(toolPos);
                }
                passToolPath.push_back(itResult.newToolPos);
                passDepth.push_back(itResult.area / double(stepScaled));  // RayCAD
                toolPos = itResult.newToolPos;

                // append gyro
                gyro.push_back(itResult.newToolDir);
                gyro.erase(gyro.begin());
            }
            else {
                // cout<<"Break: no cut @" << point_index << endl;
                break;
            }
        } /* end of points loop*/

        if (StopRequested()) {
            break;  // RayCAD: the pass is incomplete; leave it out
        }

        if (!toClearPath.empty()) {
            cleared.ExpandCleared(toClearPath);
            toClearPath.clear();
        }

        Paths64 newlyClearedAreas
            = Difference(cleared.GetCleared(), clearedBeforePass.GetCleared(), FillRule::EvenOdd);
        // RayCAD: the signed areas Clipper2 returns (outer loops positive, holes negative) summed. Upstream signed
        // each loop by a nesting test of its first point, which a point on another loop misjudges: on the S5 islands
        // pocket passes that cut thousands of mm2 came out negative, were dropped as cutting nothing, and left the
        // tool position at their end and their area in the cleared model (links from where the tool never was, and
        // material cut later at engagements far above the target).
        cumulativeCutArea = 0;
        for (Path64& a : newlyClearedAreas) {
            cumulativeCutArea += Area(a);
        }

        if (cumulativeCutArea >= 1) {
            Path64 cleaned;
            cleaned = SimplifyPath(passToolPath, CLEAN_PATH_TOLERANCE, false);
            total_output_points += int64_t(cleaned.size());
            // RayCAD: the pass with its role and the depth of each kept segment: SimplifyPath keeps a subset of
            // the points in order, and a kept segment's depth is the largest of the steps it replaces
            TPath passTagged;
            passTagged.motion = MotionType::mtCutting;
            passTagged.role = prPass;
            passTagged.points = cleaned;
            passTagged.depth.assign(cleaned.size(), -1.0);
            {
                std::vector<size_t> kept;
                size_t j = 0;
                for (const Point64& q : cleaned) {
                    while (j < passToolPath.size() && !(passToolPath[j] == q)) {
                        j++;
                    }
                    kept.push_back(j);
                    if (j < passToolPath.size()) {
                        j++;
                    }
                }
                for (size_t i = 1; i < kept.size(); i++) {
                    if (kept[i] >= passToolPath.size() || kept[i - 1] >= kept[i]) {
                        continue;
                    }
                    double d = -1;
                    for (size_t k = kept[i - 1]; k < kept[i] && k < passDepth.size(); k++) {
                        d = max(d, passDepth[k]);
                    }
                    passTagged.depth[i] = d;
                }
            }
            auto newPos = AppendToolPath(output, passTagged, linkPath, cleared, toolBoundPaths);
            if (newPos) {
                toolPos = newPos->first;
                toolDir = newPos->second;
            }
            bad_engage_count = 0;
        }
        else {
            bad_engage_count++;
            stats.failedPassesInARow = max(stats.failedPassesInARow, bad_engage_count);  // RayCAD
        }

        // RayCAD: progress by area cleared
        {
            double clearedNow = 0;
            for (const Path64& p : cleared.GetCleared()) {
                clearedNow += Area(p);
            }
            ReportProgress(areaToClear > 0 ? (clearedNow - clearedAtStart) / areaToClear : 1.0);
        }

        if (bad_engage_count > limits.failedEngagements) {  // RayCAD: upstream's 10000, as a limit
            output.TooManyFailedEngagements = true;
            break;
        }

        clearedBeforePass.SetClearedPaths(cleared.GetCleared());
        engagePoint = getEngagePoint(toolPos);
        if (StopRequested()) {
            break;  // RayCAD: the search was cut short; its result says nothing
        }
        if (engagePoint) {
            toolPos = std::get<Point64>(*engagePoint);
            toolDir = std::get<PointD>(*engagePoint);
            linkPath = std::get<TPaths>(*engagePoint);
            lastExpandToolDir = toolDir;
        }
        else {
            // check if there are any uncleared area left
            Paths64 remaining;
            for (const Path64& p : cleared.GetCleared()) {
                if (!p.empty() && IsPointWithinCutRegion(toolBoundPaths, p.front())
                    && DistancePointToPathsSqrd(
                           boundPaths,
                           p.front(),
                           clp,
                           clpPathIndex,
                           clpSegmentIndex,
                           clpParameter
                       ) > double(4 * toolRadiusScaled * toolRadiusScaled)) {
                    remaining.push_back(p);
                }
            };
            if (remaining.empty()) {
                break;
            }

            output.UnclearedAreaRemains = true;
            break;
        }
    }

    // sanity check for finishing paths - check the area of finishing cut
    Paths64 clearedLocations = OffsetPaths(cleared.GetCleared(), -double(toolRadiusScaled));

    Paths64 tbpShrink = OffsetPaths(
        toolBoundPaths,
        double(Rnd(-stepOverScaled * FINISHING_THICKNESS_SCALE - MIN_STEP_CLIPPER))  // RayCAD: rounded
    );

    Paths64 uncut = Difference(tbpShrink, clearedLocations, FillRule::EvenOdd);

    if (uncut.size() > 0 && !StopRequested()) {
        output.FailedToSetUpFinishingPass = true;
    }


    //**********************************
    //*  FINISHING PASS                *
    //**********************************
    if (finishingProfile && !StopRequested()) {
        // update tool bound paths to correspond to the finishing paths instead of the interior
        {
            Paths64 tbpModified;
            for (const Path64& fp : finishingPaths) {
                int offset = (getPathNestingLevel(fp, finishingPaths) % 2 == 1) ? 3 : -3;
                Paths64 out = OffsetPath(fp, offset);

                bool orientation = IsPositive(fp);
                for (Path64& p : out) {
                    if (IsPositive(p) != orientation) {
                        std::reverse(p.begin(), p.end());
                    }
                    tbpModified.push_back(p);
                }
            }

            toolBoundPaths = tbpModified;
        }

        // Split finishingPaths into closed (all Z=1) and open (partial Z=1) paths
        Paths64 closedFinishingPaths;
        Paths64 openFinishingPaths;

        for (const Path64& p : finishingPaths) {
            // Find starting index: first Z=0, or 0 if none found
            size_t startIdx = 0;
            for (size_t i = 0; i < p.size(); i++) {
                if (p[i].z == 0) {
                    startIdx = i;
                    break;
                }
            }

            // Loop through path starting at startIdx, collecting Z=1 vertices
            Path64 currentPath;
            for (size_t offset = 0; offset < p.size(); offset++) {
                size_t i = (startIdx + offset) % p.size();
                const Point64& pt = p[i];

                if (pt.z == 1) {
                    // Add Z=1 point to current path
                    currentPath.push_back(pt);
                }
                else {
                    // Z=0: if we have accumulated points, save them to open list
                    if (!currentPath.empty()) {
                        openFinishingPaths.push_back(currentPath);
                        currentPath.clear();
                    }
                }
            }

            // After loop completes, check what we accumulated
            if (currentPath.size() == p.size()) {
                // All vertices were Z=1 - add to closed list
                closedFinishingPaths.emplace_back(p);
            }
            else if (currentPath.size() > 0) {
                // Partial Z=1 segment remains - add to open list
                openFinishingPaths.push_back(currentPath);
            }
            else {
                // No Z=1 vertices found
            }
        }


        // Create offset version of stock boundary to check if finishing passes are within tool radius
        Paths64 stockExpandedPaths = OffsetPaths(stockInputPaths, double(toolRadiusScaled));

        while (!StopRequested() && (!closedFinishingPaths.empty() || !openFinishingPaths.empty())) {
            Path64 finShiftedPath;
            bool isClosedPath = PopNextFinishingPass(
                closedFinishingPaths,
                openFinishingPaths,
                toolPos,
                finShiftedPath,
                stepOverScaled
            );

            if (finShiftedPath.empty()) {
                continue;
            }
            // skip finishing passes outside the stock boundary that do not cut any stock
            Paths64 intersection = PathIntersectArea(finShiftedPath, stockExpandedPaths, isClosedPath);
            if (intersection.empty()) {
                continue;
            }

            // Clean path
            Path64 finCleaned;
            finCleaned = SimplifyPath(finShiftedPath, FINISHING_CLEAN_PATH_TOLERANCE, false);

            // Ensure correct endpoint after cleaning
            Point64 endPoint = isClosedPath ? finCleaned.front() : finShiftedPath.back();
            if (finCleaned.back() != endPoint) {
                // make sure it ends correctly, but don't risk ruining the final direction by making
                // a very short segment
                if (sqrt(DistanceSqrd(endPoint, finCleaned.back())) < FINISHING_CLEAN_PATH_TOLERANCE) {
                    finCleaned.pop_back();
                }
                finCleaned.push_back(endPoint);
            }

            std::optional<TPaths> linkPath = FindLinkPath(
                std::optional(toolPos),
                finCleaned[0],
                GetPathDirectionV(finCleaned, 1),
                cleared,
                toolBoundPaths,
                output
            );
            if (StopRequested()) {
                break;  // RayCAD
            }
            if (!linkPath) {
                output.FinishingLeadInFailed = true;
            }
            else {
                // RayCAD: the finishing pass with its role, and the depth estimates of it and of its link and
                // lead-in, against the area cleared before them
                for (TPath& lp : *linkPath) {
                    lp.depth = EstimateDepth(lp.points, cleared);
                }
                TPath finTagged;
                finTagged.motion = MotionType::mtCutting;
                finTagged.role = prFinish;
                finTagged.points = finCleaned;
                finTagged.depth = EstimateDepth(finCleaned, cleared);
                auto newPos = AppendToolPath(output, finTagged, *linkPath, cleared, toolBoundPaths);
                if (newPos) {
                    toolPos = newPos->first;
                    toolDir = newPos->second;
                }
                else {
                    toolPos = finCleaned.back();
                    toolDir = GetPathDirectionV(finCleaned, finCleaned.size() - 1);
                }

                cleared.ExpandCleared(finCleaned);
                for (const TPath& lp : *linkPath) {
                    if (lp.motion == MotionType::mtCutting) {
                        cleared.ExpandCleared(lp.points);  // RayCAD: in grid units, no conversion
                    }
                }
            }
        }

        Path64 returnPath = {toolPos, entryPoint};
        output.ReturnMotionType = IsClearPath(returnPath, cleared) ? MotionType::mtLinkClear
                                                                   : MotionType::mtLinkNotClear;
    }

    // dump performance results
    // RayCAD: upstream's DEV_MODE performance dump is gone; the counters of interest are in Stats
    (void)total_output_points;
    (void)over_cut_count;
    (void)total_exceeded;
    (void)total_points;
    (void)total_iterations;
    output.Stopped = StopRequested();  // RayCAD: cancelled or a hard limit reached: the region is incomplete
    ReportProgress(1.0);

    // Calculate newly cleared area (final - initial)
    // First calculate initial cleared area
    // RayCAD: Clipper2's signed areas summed, as for cumulativeCutArea above (upstream: a nesting test per loop)
    double initialClearedArea = 0.0;
    for (const Path64& path : Union(initialClearedPaths, FillRule::EvenOdd)) {
        initialClearedArea += Area(path);
    }

    // Then calculate final cleared area
    const Paths64& clearedPaths = cleared.GetCleared();
    output.ClearedPaths = clearedPaths;  // RayCAD
    double finalClearedArea = 0.0;
    for (const Path64& path : clearedPaths) {
        finalClearedArea += Area(path);
    }

    // Convert from scaled units to real units (scaleFactor is applied twice for area)
    // and subtract initial from final to get only newly cleared area
    output.ClearedArea = (finalClearedArea - initialClearedArea) / (scaleFactor * scaleFactor);

    results.push_back(output);
}

}  // namespace ray::cam::adaptive2d::fork
// clang-format on
