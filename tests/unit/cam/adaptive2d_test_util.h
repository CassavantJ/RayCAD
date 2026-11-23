// Helpers of the 2D adaptive roughing operation's tests (task T-061): parts built through the kernel (the pocket2d
// tests' builders, pocket2d_test_util.h, and spike S5's pockets as solids), an adaptive2d operation, and running the
// type with the independent toolpath invariant checker (tests/unit/cam/invariants) on every toolpath.
#pragma once

#include "pocket2d_test_util.h"

#include "cam/ops/adaptive2d/op.h" // src/cam's private header of the type

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace ray::cam::test {

inline Operation adaptiveOp(const std::string& params, double rpm = 8000, double feed = 1200)
{
    Operation op = pocketOp(params, rpm, feed);
    op.type = "adaptive2d";
    op.name = "Adaptive";
    return op;
}

// Spike S5's pockets (tests/tools/sweepcheck/s5_cases.h) as solids: each pocket cut `depth` deep into a block that is
// the S5 stock rectangle, 20 mm high (Z 0..20), its floor at 20 - depth. Exact arcs where S5 has polylines.
//   p2: BP2's circular pocket, diameter 38.1 at (41.275, 41.275), in an 82.55 mm square;
//   p3: 90 x 60 R8 (0..90, 0..60) with a round island r 7 at (25, 30) and a rounded-rectangle island 55..71 x 24..36
//       R2, in -10..100 x -10..70.
inline kernel::Shape s5Part(const std::string& id, double depth)
{
    const double floorZ = 20 - depth;
    if (id == "p2") {
        kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{82.55, 82.55, 20});
        return cut(s, prism({circleLoop(41.275, 41.275, 19.05)}, floorZ, 21));
    }
    kernel::Shape s = box(Vec3{-10, -10, 0}, Vec3{100, 70, 20});
    return cut(
        s, prism({roundedRect(0, 0, 90, 60, 8), circleLoop(25, 30, 7), roundedRect(55, 24, 71, 36, 2)}, floorZ, 21));
}

// One run of the adaptive2d type, checked by the independent checker.
struct AdaptiveCase {
    Operation op;
    lib::Tool tool;
    lib::MachineDef machine = test::machine();
    ResolvedGeometry geom;
    std::vector<invariants::ClearClaim> clears; // the floors the toolpath must clear (coverage)
    std::optional<double> maxEngagementDeg;

    GenerateInput input() const { return GenerateInput{op, tool, nullptr, machine, geom}; }
    Status validate() const { return ops::makeAdaptive2dOperationType()->validate(input()); }
    Result<std::vector<std::string>> check() const { return ops::makeAdaptive2dOperationType()->check(input()); }
    Result<Toolpath> generate() const
    {
        JobContext ctx;
        return ops::makeAdaptive2dOperationType()->generate(input(), ctx);
    }
    Result<ops::Adaptive2dPlan> plan() const
    {
        JobContext ctx;
        return ops::planAdaptive2d(input(), ctx);
    }
    double param(const std::string& key, double fallback) const
    {
        const std::string k = "\"" + key + "\":";
        const auto at = op.paramsJson.find(k);
        return at == std::string::npos ? fallback : std::stod(op.paramsJson.substr(at + k.size()));
    }
    // The claims the operation makes about this case (op.h), from its parameters.
    invariants::Claims claims() const
    {
        invariants::Claims k = invariants::claimsFor(op, geom);
        k.wcsInMachine = Vec3{100, 100, -400};
        k.stockToLeaveWall = param("stockToLeaveWall", 0);
        k.stockToLeaveFloor = param("stockToLeaveFloor", 0);
        k.direction = invariants::Direction::Any; // the engine's passes are not all climb (op.h)
        k.maxRampAngleDeg = param("rampAngle", 2) + 1e-6;
        k.helixMinDiameter = tool.diameter * param("helixMinDiameter", 30) / 100 - 1e-5;
        k.maxEngagementDeg = maxEngagementDeg;
        k.clears = clears;
        return k;
    }
    // generate(), which must succeed and pass the independent checker with the claims.
    Toolpath path(const invariants::Claims& k) const
    {
        auto p = generate();
        if (!p) {
            FAIL("generate failed: " << p.error().message);
        }
        const std::vector<invariants::Finding> found =
            invariants::check(invariants::CheckInput{*p, geom, tool, nullptr, &machine, k});
        INFO(invariants::describe(found));
        CHECK(found.empty());
        return *p;
    }
    Toolpath path() const { return path(claims()); }
};

// A counter-clockwise slot x0..x1, y0..y1 with half-circle ends (along X).
inline kernel::ProfileLoop obround(double x0, double y0, double x1, double y1)
{
    const double r = (y1 - y0) / 2, cy = (y0 + y1) / 2;
    kernel::ProfileLoop l;
    l.curves = {lineC({x0 + r, y0}, {x1 - r, y0}), arcC({x1 - r, y0}, {x1 - r, y1}, {x1 - r, cy}, r),
                lineC({x1 - r, y1}, {x0 + r, y1}), arcC({x0 + r, y1}, {x0 + r, y0}, {x0 + r, cy}, r)};
    return l;
}

// ---------- The golden cases (adaptive2d_golden_test.cpp; also used by the other tests) ----------

// A block 40 x 30 x 20 with a closed pocket x 3..37, y 3..27 (corner radius 3) down to Z 15 and two islands standing
// to the top, a round one r 3 at (13, 15) and a rectangular one x 23..29, y 11..19 (corner radius 1), each at least 7
// mm from the walls and the other. Stepdown 5 at 30 % load with a 6 mm end mill: one depth, a helix entry.
inline AdaptiveCase islandsGoldenCase()
{
    static const kernel::Shape part = [] {
        const kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{40, 30, 20});
        return cut(
            s, prism({roundedRect(3, 3, 37, 27, 3), circleLoop(13, 15, 3), roundedRect(23, 11, 29, 19, 1)}, 15, 21));
    }();
    AdaptiveCase c;
    c.op = adaptiveOp(R"({"stepdown":5,"stepover":30})");
    c.tool = endMill(6);
    c.geom = geometryOn(part, {upFace(part, 15)});
    c.clears = {clearClaim({rectPoly(3, 3, 37, 27, 3), circlePoly(13, 15, 3), rectPoly(23, 11, 29, 19, 1)}, 15)};
    return c;
}

// A block 50 x 20 x 10 with a slot x 10..40, 7.5 mm wide (y 6.25..13.75, full-radius ends) down to Z 7: a 6 mm end
// mill at 20 % load has no room for its smallest helix (6 + 1.8 mm and the finishing allowance), so it ramps.
inline AdaptiveCase slotGoldenCase()
{
    static const kernel::Shape part =
        cut(box(Vec3{0, 0, 0}, Vec3{50, 20, 10}), prism({obround(10, 6.25, 40, 13.75)}, 7, 11));
    AdaptiveCase c;
    c.op = adaptiveOp(R"({"stepdown":4,"stepover":20})");
    c.tool = endMill(6);
    c.geom = geometryOn(part, {upFace(part, 7)});
    c.clears = {clearClaim({rectPoly(10, 6.25, 40, 13.75, 3.75)}, 7)};
    return c;
}

// pocketBlock's 60 x 40 x 15 block with a pocket x 8..32, y 7..23 (corner radius 3) down to Z 12, a 9 mm hole drilled
// at its centre (20, 15); entry at pre-drilled holes only.
inline AdaptiveCase preDrillGoldenCase()
{
    static const kernel::Shape part = pocketBlock(8, 7, 32, 23, 3, 12);
    AdaptiveCase c;
    c.op = adaptiveOp(R"({"stepdown":4,"stepover":25,"entry":"preDrill","preDrillPoints":"20 15",)"
                      R"("preDrillDiameter":9})");
    c.tool = endMill(6);
    c.geom = geometryOn(part, {upFace(part, 12)});
    c.clears = {clearClaim({rectPoly(8, 7, 32, 23, 3)}, 12)};
    return c;
}

// A block 40 x 30 x 12 with a step x 8..32, y 0..14 down to Z 8, open to the front (y < 0) where the stock ends.
// Stepdown 2.5: depths 10 and 8 with the same area, so the engine runs once and both depths use its passes.
inline AdaptiveCase openGoldenCase()
{
    static const kernel::Shape part = cut(box(Vec3{0, 0, 0}, Vec3{40, 30, 12}), box(Vec3{8, -1, 8}, Vec3{32, 14, 13}));
    AdaptiveCase c;
    c.op = adaptiveOp(R"({"stepdown":2.5,"stepover":25})");
    c.tool = endMill(6);
    c.geom = geometryOn(part, {upFace(part, 8)});
    c.clears = {clearClaim({rectPoly(8, 0, 32, 14)}, 8)};
    return c;
}

// Feed moves at depth (XY moves at one Z: cuts and links in the passes) and their XY length, mm.
struct MoveDensity {
    std::size_t moves = 0;
    double lengthMm = 0;
    double perMm() const { return lengthMm > 0 ? static_cast<double>(moves) / lengthMm : 0; }
};
inline MoveDensity cutDensity(const Toolpath& p)
{
    MoveDensity d;
    Nm x = p.startX, y = p.startY, z = p.startZ;
    for (const Move& m : p.moves) {
        if (m.kind != MoveKind::Rapid && (m.role == MoveRole::Cut || m.role == MoveRole::Link) && m.z == z) {
            double len = 0;
            if (m.kind == MoveKind::Linear) {
                len = std::hypot(static_cast<double>(m.x - x), static_cast<double>(m.y - y));
            } else {
                const double r = std::hypot(static_cast<double>(x - m.i), static_cast<double>(y - m.j));
                const double a0 = std::atan2(static_cast<double>(y - m.j), static_cast<double>(x - m.i));
                const double a1 = std::atan2(static_cast<double>(m.y - m.j), static_cast<double>(m.x - m.i));
                double sweep = m.kind == MoveKind::ArcCCW ? a1 - a0 : a0 - a1;
                while (sweep <= 0) {
                    sweep += 2 * kPi;
                }
                len = r * sweep;
            }
            if (len > 0) {
                ++d.moves;
                d.lengthMm += len / kNmPerMm;
            }
        }
        x = m.x;
        y = m.y;
        z = m.z;
    }
    return d;
}

} // namespace ray::cam::test
