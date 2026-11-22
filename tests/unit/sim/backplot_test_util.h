// Hand-built toolpaths and layout checks for the backplot tests (task T-037). Toolpaths are written by hand against
// include/ray/cam.h: these tests do not need src/cam.
#pragma once

#include "backplot.h"

#include <ray/sim.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ray::sim::test {

using cam::ArcPlane;
using cam::Move;
using cam::MoveKind;
using cam::MoveRole;

inline Nm nm(double mm)
{
    return static_cast<Nm>(std::llround(mm * 1e6));
}

inline Move blankMove(MoveKind kind, MoveRole role)
{
    Move m{};
    m.kind = kind;
    m.role = role;
    m.plane = ArcPlane::XY;
    return m;
}

inline Move rapid(double x, double y, double z, MoveRole role = MoveRole::Link)
{
    Move m = blankMove(MoveKind::Rapid, role);
    m.x = nm(x), m.y = nm(y), m.z = nm(z);
    return m;
}

inline Move line(double x, double y, double z, double feedMmMin, MoveRole role = MoveRole::Cut)
{
    Move m = blankMove(MoveKind::Linear, role);
    m.x = nm(x), m.y = nm(y), m.z = nm(z);
    m.feedMicronPerMin = static_cast<std::int32_t>(std::llround(feedMmMin * 1000.0));
    return m;
}

// End (x, y, z) and absolute centre (i, j, k), all in mm.
inline Move arc(bool ccw, ArcPlane plane, double x, double y, double z, double i, double j, double k,
                double feedMmMin = 1000.0, MoveRole role = MoveRole::Cut)
{
    Move m = blankMove(ccw ? MoveKind::ArcCCW : MoveKind::ArcCW, role);
    m.plane = plane;
    m.x = nm(x), m.y = nm(y), m.z = nm(z);
    m.i = nm(i), m.j = nm(j), m.k = nm(k);
    m.feedMicronPerMin = static_cast<std::int32_t>(std::llround(feedMmMin * 1000.0));
    return m;
}

inline Move cycle(MoveKind kind, double x, double y, double bottom, double retract, double peck = 0.0,
                  double feedMmMin = 200.0, std::int32_t dwellMs = 0)
{
    Move m = blankMove(kind, MoveRole::Plunge);
    m.x = nm(x), m.y = nm(y), m.z = nm(bottom);
    m.cycleRetract = nm(retract);
    m.cyclePeck = nm(peck);
    m.cycleDwellMs = dwellMs;
    m.feedMicronPerMin = static_cast<std::int32_t>(std::llround(feedMmMin * 1000.0));
    return m;
}

inline cam::Toolpath toolpath(double sx, double sy, double sz, std::vector<Move> moves)
{
    cam::Toolpath tp{};
    tp.startX = nm(sx), tp.startY = nm(sy), tp.startZ = nm(sz);
    tp.moves = std::move(moves);
    return tp;
}

// The layout contract of sim.h: vertex 0 is the start and no move's; move m owns the non-empty range
// [moveFirstVertex[m], moveFirstVertex[m + 1]) and every vertex in it says m; the last entry is the vertex count.
inline void checkLayout(const cam::Toolpath& tp, const Backplot& bp)
{
    REQUIRE(bp.moveFirstVertex.size() == tp.moves.size() + 1);
    REQUIRE(!bp.lineVertices.empty());
    CHECK(bp.lineVertices[0].moveIndex == detail::kBackplotNoMove);
    REQUIRE(bp.moveFirstVertex.front() == 1);
    REQUIRE(bp.moveFirstVertex.back() == bp.lineVertices.size());
    bool ok = true;
    for (std::size_t m = 0; m < tp.moves.size(); ++m) {
        const std::uint32_t a = bp.moveFirstVertex[m], b = bp.moveFirstVertex[m + 1];
        if (!(a < b)) {
            ok = false;
            FAIL_CHECK("move " << m << " has no vertex");
            continue;
        }
        for (std::uint32_t v = a; v < b; ++v) {
            if (bp.lineVertices[v].moveIndex != m) {
                ok = false;
            }
        }
    }
    // bounds: exactly the box of the stored vertices.
    Box3 box{{bp.lineVertices[0].x, bp.lineVertices[0].y, bp.lineVertices[0].z},
             {bp.lineVertices[0].x, bp.lineVertices[0].y, bp.lineVertices[0].z}};
    for (const BackplotVertex& v : bp.lineVertices) {
        if (v.pad != 0) {
            ok = false;
        }
        box.min = {std::min<double>(box.min.x, v.x), std::min<double>(box.min.y, v.y),
                   std::min<double>(box.min.z, v.z)};
        box.max = {std::max<double>(box.max.x, v.x), std::max<double>(box.max.y, v.y),
                   std::max<double>(box.max.z, v.z)};
    }
    CHECK(ok);
    CHECK(bp.bounds.min.x == box.min.x);
    CHECK(bp.bounds.min.y == box.min.y);
    CHECK(bp.bounds.min.z == box.min.z);
    CHECK(bp.bounds.max.x == box.max.x);
    CHECK(bp.bounds.max.y == box.max.y);
    CHECK(bp.bounds.max.z == box.max.z);
}

inline float f(double mm)
{
    return static_cast<float>(mm);
}

} // namespace ray::sim::test
