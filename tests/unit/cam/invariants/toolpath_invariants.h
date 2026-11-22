// Toolpath invariant checker, general part (task T-035; T-058 completes the checker in this folder). Test-only code,
// written from cam.h's text like cycle_invariants.h, sharing nothing with src/cam, src/sim or src/post.
//
// checkToolpath(path, rules) returns every broken rule in move order: the canned-cycle rules of checkCycles
// (cycle_invariants.h, with rules.rpm) and these, each checked when the rule's input is given:
//   rapidFeed   a rapid with a feed (cam.h: 0 for rapids)
//   feedMove    a Linear or arc move without a positive feed
//   rapidLow    rapid motion below rules.rapidFloorZ over the stock (rules.stock, its XY box): a plain rapid whose end
//               points are not both at or above the floor and whose XY path touches the box, or the travel of a cycle
//               move to its hole (cam.h step 1, at the tool's Z) under the same condition. A cycle's own rapid to its
//               R plane along the hole axis is the cycle's business (cycleRetract >= cycleTop: "levels")
//   belowFloor  any motion below rules.floorZ: a plain move's end, a cycle's bottom z
//   travel      a point outside the machine's travel (rules.axes, those whose max > min; 0 0 is not known) with the
//               WCS origin at rules.wcsInMachine: every plain end point, and every cycle's hole at its bottom and at
//               its clearance
// The tool position follows cam.h, as in checkCycles.
#pragma once

#include "cycle_invariants.h"

#include <ray/libraries.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace ray::cam::invariants {

struct ToolpathRules {
    std::optional<double> rpm;      // checkCycles' tapFeed
    std::optional<Box3> stock;      // WCS mm: rapidLow looks at its XY box
    std::optional<Nm> rapidFloorZ;  // rapidLow
    std::optional<Nm> floorZ;       // belowFloor
    std::vector<lib::AxisDef> axes; // travel, with wcsInMachine
    Vec3 wcsInMachine{};            // machine coordinates (mm) of the WCS origin
};

// Whether the XY segment a-b (nm) touches the box [x0, x1] x [y0, y1] (nm): Liang-Barsky clipping in doubles, which
// is exact enough for a test oracle at these magnitudes.
inline bool segmentTouchesBox(double ax, double ay, double bx, double by, double x0, double y0, double x1, double y1)
{
    double t0 = 0, t1 = 1;
    const double dx = bx - ax, dy = by - ay;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {ax - x0, x1 - ax, ay - y0, y1 - ay};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0) {
                return false;
            }
            continue;
        }
        const double r = q[i] / p[i];
        if (p[i] < 0) {
            t0 = std::max(t0, r);
        } else {
            t1 = std::min(t1, r);
        }
        if (t0 > t1) {
            return false;
        }
    }
    return true;
}

inline std::vector<Violation> checkToolpath(const Toolpath& path, const ToolpathRules& rules = {})
{
    std::vector<Violation> out = checkCycles(path, rules.rpm);
    const auto overStock = [&](Nm ax, Nm ay, Nm bx, Nm by) {
        if (!rules.stock) {
            return false;
        }
        const Box3& b = *rules.stock;
        return segmentTouchesBox(static_cast<double>(ax), static_cast<double>(ay), static_cast<double>(bx),
                                 static_cast<double>(by), b.min.x * 1e6, b.min.y * 1e6, b.max.x * 1e6, b.max.y * 1e6);
    };
    const auto inTravel = [&](Nm x, Nm y, Nm z) {
        for (const lib::AxisDef& a : rules.axes) {
            if (!(a.maxTravel > a.minTravel)) {
                continue;
            }
            const double v = a.letter == 'X'   ? toMm(x) + rules.wcsInMachine.x
                             : a.letter == 'Y' ? toMm(y) + rules.wcsInMachine.y
                             : a.letter == 'Z' ? toMm(z) + rules.wcsInMachine.z
                                               : 0.0;
            if ((a.letter == 'X' || a.letter == 'Y' || a.letter == 'Z') &&
                (v < a.minTravel - 1e-9 || v > a.maxTravel + 1e-9)) {
                return false;
            }
        }
        return true;
    };
    Nm x = path.startX, y = path.startY, z = path.startZ;
    std::vector<Violation> more;
    for (std::size_t n = 0; n < path.moves.size(); ++n) {
        const Move& m = path.moves[n];
        const auto fail = [&](const char* rule) {
            more.push_back({n, rule});
        };
        if (m.kind == MoveKind::Dwell || m.kind == MoveKind::CycleCancel) {
            continue;
        }
        const bool cycle = isCycle(m.kind);
        if (m.kind == MoveKind::Rapid && m.feedMicronPerMin != 0) {
            fail("rapidFeed");
        }
        if ((m.kind == MoveKind::Linear || m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW) &&
            m.feedMicronPerMin <= 0) {
            fail("feedMove");
        }
        if (rules.rapidFloorZ && (m.kind == MoveKind::Rapid || cycle)) {
            // A plain rapid ends at its point; a cycle travels to its hole at the tool's Z.
            const Nm endZ = cycle ? z : m.z;
            if ((z < *rules.rapidFloorZ || endZ < *rules.rapidFloorZ) && overStock(x, y, m.x, m.y) &&
                (x != m.x || y != m.y || !cycle)) {
                fail("rapidLow");
            }
        }
        if (rules.floorZ && m.z < *rules.floorZ) {
            fail("belowFloor");
        }
        if (!rules.axes.empty()) {
            const bool ok =
                cycle ? inTravel(m.x, m.y, m.z) && inTravel(m.x, m.y, m.cycleClearance) : inTravel(m.x, m.y, m.z);
            if (!ok) {
                fail("travel");
            }
        }
        x = m.x;
        y = m.y;
        z = cycle ? (m.cycleReturn == CycleReturn::Initial ? m.cycleClearance : m.cycleRetract) : m.z;
    }
    out.insert(out.end(), more.begin(), more.end());
    std::stable_sort(out.begin(), out.end(), [](const Violation& a, const Violation& b) { return a.move < b.move; });
    return out;
}

} // namespace ray::cam::invariants
