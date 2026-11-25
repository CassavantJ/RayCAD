// The checker on real-looking milling (task T-058): the hand-written reference generators (reference_paths.h; zig-zag
// facing, a contour with arc corners and tangent leads, a pocket with a helix entry) pass with every claim they can
// make, and small mistakes in them, the kind a generator makes, are caught.
#include "checker_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;
using namespace ray::cam::invariants::test;

namespace {

lib::Holder collet()
{
    lib::Holder h;
    h.segments = {lib::HolderSegment{20, 44, 44}, lib::HolderSegment{40, 24, 32}};
    h.gaugeLength = 60;
    return h;
}

} // namespace

TEST_CASE("invariants: reference zig-zag facing passes, and its mistakes are caught", "[cam][invariants][reference]")
{
    // A plate with a pocket; 1.5 mm of stock over its top, faced to Z 0 with a 12 mm tool.
    kernel::Shape part = box(Vec3{0, 0, -15}, Vec3{80, 50, 0});
    part = cut(part, box(Vec3{20, 15, -6}, Vec3{60, 35, 1}));
    const ResolvedGeometry g = geometryOn(part, Box3{Vec3{0, 0, -15}, Vec3{80, 50, 1.5}});
    const lib::Tool tool = flatMill(12, 10);
    const lib::MachineDef m = machine();
    const lib::Holder h = collet();
    Claims c = claims(g);
    c.stickOut = 30;
    c.holderClearance = 1;
    c.wcsInMachine = Vec3{100, 100, -400};
    c.heights.clearance = 12;
    c.clears = {ClearClaim{Region::box(), 0, 0.002, "coverage"}};
    FaceRef r;
    r.stock = g.stockBounds;
    r.toolRadius = 6;
    r.stepover = 8;
    r.z = 0;
    r.clearance = 12;
    const auto t0 = std::chrono::steady_clock::now();
    clean(run(zigzagFace(r), g, tool, c, &h, &m));
    UNSCOPED_INFO("facing check: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()
                                   << " s");
    // Too deep by 20 um: the part's top is gouged.
    r.z = -0.02;
    onlyThis(run(zigzagFace(r), g, tool, c, &h, &m), "gouge");
    // Stepover wider than the tool: strips of stock left.
    r.z = 0;
    r.stepover = 12.5;
    onlyThis(run(zigzagFace(r), g, tool, c, &h, &m), "coverage");
    // Overhang taken the wrong way: the plunges that belong outside the stock land on it, the dead centre of a flat end
    // mill whose entry says it does not cut at its centre on stock (one that says nothing cuts at its centre by its
    // type's default, after M3).
    r.stepover = 8;
    r.overhang = -7;
    lib::Tool notCentreCutting = tool;
    notCentreCutting.centreCutting = false;
    const auto f = run(zigzagFace(r), g, notCentreCutting, c, &h, &m);
    CHECK(has(f, "plunge"));
    CHECK_FALSE(has(run(zigzagFace(r), g, tool, c, &h, &m), "plunge"));
}

TEST_CASE("invariants: reference contour passes, and its mistakes are caught", "[cam][invariants][reference]")
{
    // A block 40 x 30 x 12 in stock 3 mm wider at each side, contoured in two depths with 0.3 mm left on the walls.
    const kernel::Shape part = box(Vec3{0, 0, -12}, Vec3{40, 30, 0});
    const ResolvedGeometry g = geometryOn(part, Box3{Vec3{-3, -3, -12}, Vec3{43, 33, 0}});
    const lib::Tool tool = flatMill(6, 8);
    const lib::Holder h = collet();
    Claims c = claims(g);
    c.stickOut = 25;
    c.holderClearance = 1;
    c.stockToLeaveWall = 0.3;
    c.direction = Direction::Climb;
    c.walls = {WallClaim{Region::box(), -10, 0, 0.25}};
    c.heights.clearance = 10;
    c.heights.retract = 2;
    ContourRef r;
    r.depths = {-5, -10};
    r.stockToLeave = 0.3;
    r.leadRadius = 5;
    clean(run(contourRect(r), g, tool, c, &h));
    // Conventional where climb was asked.
    r.climb = false;
    onlyThis(run(contourRect(r), g, tool, c, &h), "direction");
    r.climb = true;
    // The offset forgot the stock to leave.
    r.stockToLeave = 0;
    onlyThis(run(contourRect(r), g, tool, c, &h), "gouge");
    // Straight to the full depth: 10 mm with 8 mm of flutes.
    r.stockToLeave = 0.3;
    r.depths = {-10};
    onlyThis(run(contourRect(r), g, tool, c, &h), "fluteLength");
    // One depth only: the lower walls keep their stock.
    r.depths = {-5};
    onlyThis(run(contourRect(r), g, tool, c, &h), "stockLeft");
    // A lead-in that is not tangent: the lead arc's centre moved.
    r.depths = {-5, -10};
    Toolpath p = contourRect(r);
    for (Move& mv : p.moves) {
        if (mv.role == MoveRole::LeadIn) {
            mv.i += toNm(0.5); // the centre moves along X, the ends stay: the arc ends 5.7 degrees off the cut
            break;
        }
    }
    const auto f = run(p, g, tool, c, &h);
    CHECK(has(f, "leadTangent"));
    CHECK(has(f, "arc"));
}

TEST_CASE("invariants: reference pocket passes, and its mistakes are caught", "[cam][invariants][reference]")
{
    // A plate with a pocket 44 x 30, corner radius 6, floor Z -6, cut from solid with a 6 mm tool.
    kernel::Shape part = box(Vec3{0, 0, -15}, Vec3{70, 50, 0});
    {
        JobContext ctx;
        kernel::ProfileLoop loop;
        const double x0 = 13, y0 = 10, x1 = 57, y1 = 40, rc = 6;
        using K = kernel::ProfileCurve::Kind;
        loop.curves = {{K::Line, {{x0 + rc, y0}, {x1 - rc, y0}}, 0, false, 0},
                       {K::Arc, {{x1 - rc, y0}, {x1, y0 + rc}, {x1 - rc, y0 + rc}}, rc, true, 0},
                       {K::Line, {{x1, y0 + rc}, {x1, y1 - rc}}, 0, false, 0},
                       {K::Arc, {{x1, y1 - rc}, {x1 - rc, y1}, {x1 - rc, y1 - rc}}, rc, true, 0},
                       {K::Line, {{x1 - rc, y1}, {x0 + rc, y1}}, 0, false, 0},
                       {K::Arc, {{x0 + rc, y1}, {x0, y1 - rc}, {x0 + rc, y1 - rc}}, rc, true, 0},
                       {K::Line, {{x0, y1 - rc}, {x0, y0 + rc}}, 0, false, 0},
                       {K::Arc, {{x0, y0 + rc}, {x0 + rc, y0}, {x0 + rc, y0 + rc}}, rc, true, 0}};
        const kernel::Profile prof{kernel::Plane{Vec3{0, 0, -6}, Vec3{0, 0, 1}, Vec3{1, 0, 0}}, {loop}};
        auto tool = kernel::extrude(prof, 7, 0, ctx);
        REQUIRE(tool);
        part = cut(part, tool->op.shape);
    }
    const ResolvedGeometry g = geometryOn(part, Box3{Vec3{0, 0, -15}, Vec3{70, 50, 0}});
    const lib::Tool tool = flatMill(6, 10);
    const lib::Holder h = collet();
    Claims c = claims(g);
    c.stickOut = 25;
    c.holderClearance = 1;
    c.direction = Direction::Climb;
    c.maxRampAngleDeg = 8;
    c.helixMinDiameter = 2.5;
    c.clears = {ClearClaim{Region::of({{{13, 10}, {57, 10}, {57, 40}, {13, 40}}}), -6, 0.002, "coverage"}};
    c.walls = {WallClaim{Region::box(), -6, 0, 0.25}};
    PocketRef r;
    r.x0 = 13;
    r.y0 = 10;
    r.x1 = 57;
    r.y1 = 40;
    r.cornerRadius = 6;
    r.z = -6;
    r.stepover = 2.5;
    r.helixRadius = 1.5;
    r.helixPitch = 1;
    const auto t0 = std::chrono::steady_clock::now();
    clean(run(pocketRect(r), g, tool, c, &h));
    UNSCOPED_INFO("pocket check: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()
                                   << " s");
    // Conventional loops where climb was asked.
    r.climb = false;
    onlyThis(run(pocketRect(r), g, tool, c, &h), "direction");
    r.climb = true;
    // The tool taken as 5.96 mm: every wall loop 20 um into the wall (and the retract starts there).
    r.toolRadius = 2.98;
    mainly(run(pocketRect(r), g, tool, c, &h), "gouge", {"rapidPart"});
    r.toolRadius = 3;
    // A stepover wider than the tool: rings of stock left on the floor.
    r.stepover = 6.5;
    onlyThis(run(pocketRect(r), g, tool, c, &h), "coverage");
    r.stepover = 2.5;
    // A helix too steep (4 mm a turn) and too narrow.
    r.helixPitch = 4;
    onlyThis(run(pocketRect(r), g, tool, c, &h), "rampAngle");
    r.helixPitch = 0.5;
    r.helixRadius = 1;
    onlyThis(run(pocketRect(r), g, tool, c, &h), "helix");
}
