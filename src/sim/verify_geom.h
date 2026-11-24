// Exact geometry of the safety checks (task T-071; verify.h says what uses it): a tool zone swept along a straight
// motion against the triangles of a closed solid.
//
// THE TEST. A zone is a union of convex pieces (toolshape.h: solids of revolution about the tool axis, radius r(h)
// concave on [h0, h1]). A piece P swept from tip a to tip b meets triangle T exactly when P (at the origin) meets
// Q = conv(T - a, T - b), the triangle swept backwards: a convex polytope of at most six vertices. With f(h) the
// distance from the axis to Q's slice at height h (convex in h, Q being convex) and r concave, g = f - r is convex, and
// P meets Q when min g <= 0 over the heights both cover. Computed as:
//   1. Q's slab over those heights projected to XY (vertices inside, every crossing of a segment between two vertices
//      with the slab's planes), its convex hull, the hull's distance from the axis: above P's largest radius, no hit;
//      on a height span where P is a cylinder (most of every tool: flat end, shank, holder, spindle), this distance
//      against the radius IS the answer;
//   2. at most P's smallest radius over the span (r is concave: at an end): a hit;
//   3. otherwise golden-section search of g (convex), to within 1e-12 relative of the span, a hit when any value
//      reached is <= 0.
// Floating point only rounds; the callers inflate every zone by kVerifyContactMm, so a contact closer than that, which
// rounding could hide, is a hit.
// A sweep can also lie wholly inside a solid without crossing a triangle: when no triangle is met and the zone does not
// go on upward without end, one point of each piece is tested for being inside (nonzero winding along +Z).
#pragma once

#include "toolshape.h"

#include <ray/sim.h>

#include <cstdint>
#include <vector>

namespace ray::sim::detail {

inline bool boxesOverlap(const Box3& a, const Box3& b)
{
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y && a.min.z <= b.max.z &&
           b.min.z <= a.max.z;
}
inline Box3 grown(const Box3& b, double d)
{
    return {{b.min.x - d, b.min.y - d, b.min.z - d}, {b.max.x + d, b.max.y + d, b.max.z + d}};
}
inline Box3 clipped(const Box3& a, const Box3& b)
{
    return {{std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y), std::max(a.min.z, b.min.z)},
            {std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y), std::min(a.max.z, b.max.z)}};
}

// A closed triangle soup with a bounding volume hierarchy over its triangles (built by sorting, ties by index, so the
// same on every platform).
class SoupIndex {
public:
    SoupIndex() = default;
    explicit SoupIndex(const kernel::TriSoup& soup);
    bool empty() const { return tris_.empty(); }
    const Box3& bounds() const { return bounds_; }
    // Calls f(triangle index, its three corners) for every triangle whose box overlaps `box`, in a fixed order, until
    // f returns true; returns whether one did.
    template <class F>
    bool any(const Box3& box, F&& f) const;
    // Nonzero winding along +Z (on a triangle counts as inside).
    bool contains(const Vec3& p) const;

private:
    struct Tri {
        Vec3 v[3];
        Box3 box;
    };
    struct Node {
        Box3 box;
        std::uint32_t a = 0, b = 0; // leaf: tris [a, b); inner: children a and b
        bool leaf = false;
    };
    std::uint32_t build(std::vector<std::uint32_t>& order, std::uint32_t first, std::uint32_t last);
    std::vector<Tri> tris_;
    std::vector<Node> nodes_;
    Box3 bounds_{};
};

template <class F>
bool SoupIndex::any(const Box3& box, F&& f) const
{
    if (tris_.empty() || !boxesOverlap(box, bounds_)) {
        return false;
    }
    std::uint32_t stack[64];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& n = nodes_[stack[--top]];
        if (!boxesOverlap(box, n.box)) {
            continue;
        }
        if (n.leaf) {
            for (std::uint32_t t = n.a; t < n.b; ++t) {
                if (boxesOverlap(box, tris_[t].box) && f(t, tris_[t].v)) {
                    return true;
                }
            }
            continue;
        }
        stack[top++] = n.b; // left first
        stack[top++] = n.a;
    }
    return false;
}

// Boxes in an order (the cutting sweeps of the motions, by motion index): whether one with index below `limit`
// overlaps `box`.
class OrderedBoxes {
public:
    OrderedBoxes() = default;
    explicit OrderedBoxes(std::vector<Box3> boxes);
    bool anyBefore(const Box3& box, std::uint32_t limit) const;

private:
    struct Node {
        Box3 box;
        std::uint32_t minIndex = 0;
        std::uint32_t a = 0, b = 0;
        bool leaf = false;
    };
    std::uint32_t build(std::uint32_t first, std::uint32_t last);
    std::vector<Box3> boxes_;
    std::vector<std::uint32_t> order_;
    std::vector<Node> nodes_;
};

// The box a zone sweeps with its tip going from a to b.
Box3 sweepBox(const CutterShape& zone, const Vec3& a, const Vec3& b);
// Whether convex piece `pc`, its tip swept from a to b, meets the (filled) triangle `tri`.
bool pieceHitsTriangle(const ConvexPiece& pc, const Vec3& a, const Vec3& b, const Vec3 tri[3]);
// Whether the zone swept from a to b meets the solid (a triangle, or lying inside it).
bool sweepHitsSoup(const CutterShape& zone, const Vec3& a, const Vec3& b, const SoupIndex& soup);

// A shape containing `shape` grown by a ball of radius e (every point within e of it). Per convex piece (radius r
// concave, largest on its plateau): the rising branch moved down by e, the falling branch up by e, every radius plus e;
// so at each height the radius is at least the largest within e above or below, plus e. Still convex pieces.
CutterShape grownShape(const CutterShape& shape, double e);
// A (radius, height) profile shrunk by s > 0: radius - s, heights pulled in by s at both ends. Contains the solid
// shrunk by a ball of radius s (the points at least s inside it). Empty when nothing is left.
std::vector<Vec2> shrunkProfile(const std::vector<Vec2>& profile, double s);

} // namespace ray::sim::detail
