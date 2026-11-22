// Hole recognition (task T-034) on parts built through the kernel: every kind the corpus lacks (flat floors, blind
// counterbores and countersinks, chamfered counterbores, stepped holes with a drill point step), deliberate non-holes
// (a boss, a hole off Z, a hole opening downwards, a hole under an overhang, a cut-out on an edge, a convex fillet, a
// pocket with corner radii, a slot, a pocket floor that runs into an island), cross-drilled holes, independence from
// face order, the size limit, errors and cancellation, and the time on a plate of a few thousand holes.
#include "holes_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

const double kTip = 1.0 / std::tan(59.0 * 3.14159265358979323846 / 180.0); // 118 degree point per unit radius
constexpr double kTwoPi = 2 * 3.14159265358979323846;
constexpr double kTol = 1e-6;

kernel::Shape box(Vec3 a, Vec3 b)
{
    auto s = kernel::makeBox(a, b);
    REQUIRE(s);
    return *s;
}

kernel::Shape cylinder(Vec3 base, Vec3 axis, double r, double h)
{
    auto s = kernel::makeCylinder(base, axis, r, h);
    REQUIRE(s);
    return *s;
}

kernel::Shape boolean(kernel::BoolOp op, const kernel::Shape& a, const kernel::Shape& b)
{
    JobContext ctx;
    auto r = kernel::boolean(op, a, b, ctx);
    if (!r) {
        FAIL("boolean failed: " << r.error().message);
    }
    return r->shape;
}
kernel::Shape cut(const kernel::Shape& a, const kernel::Shape& b)
{
    return boolean(kernel::BoolOp::Cut, a, b);
}
kernel::Shape fuse(const kernel::Shape& a, const kernel::Shape& b)
{
    return boolean(kernel::BoolOp::Union, a, b);
}

kernel::ProfileCurve line(Vec2 a, Vec2 b)
{
    return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Line, {a, b}, 0, false, 0};
}
kernel::ProfileCurve arc(Vec2 a, Vec2 b, Vec2 c)
{
    return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Arc, {a, b, c}, 0, true, 0};
}

// A hole tool: the closed half-section `rz` (radius, z) revolved a full turn about the vertical line through (x, y).
kernel::Shape revolved(double x, double y, const std::vector<Vec2>& rz)
{
    kernel::ProfileLoop loop;
    for (std::size_t i = 0; i < rz.size(); ++i) {
        loop.curves.push_back(line(rz[i], rz[(i + 1) % rz.size()]));
    }
    const kernel::Profile p{kernel::Plane{Vec3{x, y, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0}}, {loop}};
    JobContext ctx;
    auto r = kernel::revolve(p, Vec3{x, y, 0}, Vec3{0, 0, 1}, kTwoPi, ctx);
    if (!r) {
        FAIL("revolve failed: " << r.error().message);
    }
    return r->op.shape;
}

// A prism of the profile loop `curves` (in the plane z = z0, model x and y), `h` high.
kernel::Shape prism(const std::vector<kernel::ProfileCurve>& curves, double z0, double h)
{
    kernel::ProfileLoop loop;
    loop.curves = curves;
    const kernel::Profile p{kernel::Plane{Vec3{0, 0, z0}, Vec3{0, 0, 1}, Vec3{1, 0, 0}}, {loop}};
    JobContext ctx;
    auto r = kernel::extrude(p, h, 0.0, ctx);
    if (!r) {
        FAIL("extrude failed: " << r.error().message);
    }
    return r->op.shape;
}

// A rectangle centred on (cx, cy), w by l, with corner radius r.
std::vector<kernel::ProfileCurve> roundedRect(double cx, double cy, double w, double l, double r)
{
    const double x0 = cx - w / 2, x1 = cx + w / 2, y0 = cy - l / 2, y1 = cy + l / 2;
    return {line({x0 + r, y0}, {x1 - r, y0}), arc({x1 - r, y0}, {x1, y0 + r}, {x1 - r, y0 + r}),
            line({x1, y0 + r}, {x1, y1 - r}), arc({x1, y1 - r}, {x1 - r, y1}, {x1 - r, y1 - r}),
            line({x1 - r, y1}, {x0 + r, y1}), arc({x0 + r, y1}, {x0, y1 - r}, {x0 + r, y1 - r}),
            line({x0, y1 - r}, {x0, y0 + r}), arc({x0, y0 + r}, {x0 + r, y0}, {x0 + r, y0 + r})};
}

// An obround slot from (x0, y) to (x1, y), width w.
std::vector<kernel::ProfileCurve> slot(double x0, double x1, double y, double w)
{
    const double r = w / 2;
    return {line({x0, y - r}, {x1, y - r}), arc({x1, y - r}, {x1, y + r}, {x1, y}), line({x1, y + r}, {x0, y + r}),
            arc({x0, y + r}, {x0, y - r}, {x0, y})};
}

std::vector<RecognizedHole> holesOf(const kernel::Shape& shape, const Xform& frame = identityFrame(),
                                    const HoleOptions& options = {})
{
    JobContext ctx;
    auto r = recognizeHoles(shape, frame, ctx, options);
    if (!r) {
        FAIL("recognizeHoles failed: " << r.error().message);
    }
    return *r;
}

// What a hole is expected to be: its place and its segments, as in holes_labels.txt.
struct Expected {
    double x, y, zTop;
    HoleBottom bottom;
    std::string kind;
    std::vector<std::array<double, 5>> segments; // dTop, dBottom, zTop, zBottom, angle (0: a cylinder)
};

std::string kindOf(const RecognizedHole& h)
{
    std::string k;
    for (const auto& [on, word] : {std::pair{h.counterbored, "counterbored"}, std::pair{h.countersunk, "countersunk"},
                                   std::pair{h.chamfered, "chamfered"}}) {
        if (on) {
            k += (k.empty() ? "" : ",") + std::string(word);
        }
    }
    return k.empty() ? "plain" : k;
}

void expectHoles(const std::vector<RecognizedHole>& found, const std::vector<Expected>& expected)
{
    REQUIRE(found.size() == expected.size());
    for (const Expected& e : expected) {
        INFO("expected at (" << e.x << ", " << e.y << ")");
        const auto it = std::find_if(found.begin(), found.end(), [&](const RecognizedHole& h) {
            return std::abs(toMm(h.x) - e.x) < kTol && std::abs(toMm(h.y) - e.y) < kTol;
        });
        REQUIRE(it != found.end());
        const RecognizedHole& h = *it;
        INFO(describe(h));
        CHECK(std::abs(toMm(h.zTop) - e.zTop) < kTol);
        CHECK(h.bottom == e.bottom);
        CHECK(kindOf(h) == e.kind);
        REQUIRE(h.segments.size() == e.segments.size());
        for (std::size_t i = 0; i < e.segments.size(); ++i) {
            const auto& [dTop, dBottom, zTop, zBottom, angle] = e.segments[i];
            const HoleSegment& g = h.segments[i];
            INFO("segment " << i);
            CHECK(g.conical == (angle != 0));
            CHECK(std::abs(g.diameter - dTop) < kTol);
            CHECK(std::abs(g.bottomDiameter - dBottom) < kTol);
            CHECK(std::abs(toMm(g.zTop) - zTop) < kTol);
            CHECK(std::abs(toMm(g.zBottom) - zBottom) < kTol);
            CHECK(std::abs(g.depth - (zTop - zBottom)) < kTol);
            if (angle != 0) {
                CHECK(std::abs(g.coneAngleDeg - angle) < 1e-6);
            }
        }
    }
}

// Block 100 x 60 x 30 with one hole of every kind the corpus parts do not have (holes numbered as below).
kernel::Shape kindsBlock(bool reverseOrder = false)
{
    std::vector<kernel::Shape> tools = {
        revolved(15, 15, {{0, 31}, {4, 31}, {4, 18}, {0, 18}}),                              // 1 flat floor
        revolved(40, 15, {{0, 31}, {7, 31}, {7, 25}, {4, 25}, {4, 15}, {0, 15 - 4 * kTip}}), // 2 blind counterbore
        revolved(65, 15, {{0, 31}, {6, 31}, {6, 30}, {3, 27}, {3, 10}, {0, 10}}),            // 3 blind countersink
        revolved(88, 15, {{0, 31}, {6.5, 31}, {6.5, 30}, {6, 29.5}, {6, 24}, {3, 24}, {3, -1}, {0, -1}}), // 4
        revolved(15, 45, {{0, 31}, {5, 31}, {5, 20}, {3, 20 - 2 * kTip}, {3, 5}, {0, 5}}),                // 5 stepped
        revolved(40, 45, {{0, 31}, {1.8, 31}, {1.8, 30}, {1.5, 29.7}, {1.5, -1}, {0, -1}}), // 6 small chamfer
        revolved(65, 45, {{0, 31}, {3.4, 31}, {3.4, 16}, {0, 16 - 3.4 * kTip}}),            // 7 tap drill
        cylinder(Vec3{88, 45, -1}, Vec3{0, 0, 1}, 2.5, 32),                                 // 8 through
    };
    kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{100, 60, 30});
    if (!reverseOrder) {
        for (const kernel::Shape& t : tools) {
            s = cut(s, t);
        }
        return s;
    }
    // The other way: the block extruded from a sketch, the holes cut in the opposite order.
    s = prism({line({0, 0}, {100, 0}), line({100, 0}, {100, 60}), line({100, 60}, {0, 60}), line({0, 60}, {0, 0})}, 0,
              30);
    for (std::size_t i = tools.size(); i-- > 0;) {
        s = cut(s, tools[i]);
    }
    return s;
}

const std::vector<Expected> kKindsFromAbove = {
    {15, 15, 30, HoleBottom::Flat, "plain", {{8, 8, 30, 18, 0}}},
    {40,
     15,
     30,
     HoleBottom::Conical,
     "counterbored",
     {{14, 14, 30, 25, 0}, {8, 8, 25, 15, 0}, {8, 0, 15, 15 - 4 * kTip, 118}}},
    {65, 15, 30, HoleBottom::Flat, "countersunk", {{12, 6, 30, 27, 90}, {6, 6, 27, 10, 0}}},
    {88,
     15,
     30,
     HoleBottom::Through,
     "counterbored,chamfered",
     {{13, 12, 30, 29.5, 90}, {12, 12, 29.5, 24, 0}, {6, 6, 24, 0, 0}}},
    {15,
     45,
     30,
     HoleBottom::Flat,
     "counterbored",
     {{10, 10, 30, 20, 0}, {10, 6, 20, 20 - 2 * kTip, 118}, {6, 6, 20 - 2 * kTip, 5, 0}}},
    {40, 45, 30, HoleBottom::Through, "chamfered", {{3.6, 3, 30, 29.7, 90}, {3, 3, 29.7, 0, 0}}},
    {65, 45, 30, HoleBottom::Conical, "plain", {{6.8, 6.8, 30, 16, 0}, {6.8, 0, 16, 16 - 3.4 * kTip, 118}}},
    {88, 45, 30, HoleBottom::Through, "plain", {{5, 5, 30, 0, 0}}},
};

// Seen from below, only the through holes are there, each up to where it breaks out into the wider part.
const std::vector<Expected> kKindsFromBelow = {
    {88, -15, 0, HoleBottom::Through, "plain", {{6, 6, 0, -24, 0}}},
    {40, -45, 0, HoleBottom::Through, "plain", {{3, 3, 0, -29.7, 0}}},
    {88, -45, 0, HoleBottom::Through, "plain", {{5, 5, 0, -30, 0}}},
};

} // namespace

TEST_CASE("flat floors, blind counterbores and countersinks, chamfers and stepped holes", "[cam][holes]")
{
    const kernel::Shape s = kindsBlock();
    SECTION("from above")
    {
        expectHoles(holesOf(s), kKindsFromAbove);
    }
    SECTION("from below")
    {
        expectHoles(holesOf(s, fromBelow()), kKindsFromBelow);
    }
    SECTION("with a size limit, wider bores are left out")
    {
        HoleOptions options;
        options.maxDiameter = 6;
        const auto found = holesOf(s, identityFrame(), options);
        CHECK(found.size() == 5); // holes 1 and 2 (8) and 7 (6.8) are wider
        for (const RecognizedHole& h : found) {
            CHECK(h.segments[h.bore].diameter <= 6 + 1e-9);
        }
    }
}

TEST_CASE("the result does not depend on face order", "[cam][holes]")
{
    // The same block with its holes cut in the opposite order: other face indices, the same holes in the same order.
    const auto a = holesOf(kindsBlock(false));
    const auto b = holesOf(kindsBlock(true));
    REQUIRE(a.size() == b.size());
    bool someFaceMoved = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        INFO(describe(a[i]) << "\n" << describe(b[i]));
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].y == b[i].y);
        CHECK(a[i].zTop == b[i].zTop);
        CHECK(a[i].zBottom == b[i].zBottom);
        CHECK(a[i].bottom == b[i].bottom);
        CHECK(kindOf(a[i]) == kindOf(b[i]));
        REQUIRE(a[i].segments.size() == b[i].segments.size());
        for (std::size_t s = 0; s < a[i].segments.size(); ++s) {
            CHECK(a[i].segments[s].diameter == b[i].segments[s].diameter);
            CHECK(a[i].segments[s].bottomDiameter == b[i].segments[s].bottomDiameter);
            CHECK(a[i].segments[s].zTop == b[i].segments[s].zTop);
            CHECK(a[i].segments[s].zBottom == b[i].segments[s].zBottom);
            CHECK(a[i].segments[s].coneAngleDeg == b[i].segments[s].coneAngleDeg);
        }
        someFaceMoved = someFaceMoved || a[i].faces != b[i].faces;
    }
    CHECK(someFaceMoved); // the test means something only if the faces came in another order
}

TEST_CASE("bosses, holes off Z or opening downwards, blocked holes, cut-outs, fillets, corners and slots are not holes",
          "[cam][holes]")
{
    // Block 120 x 80 x 30.
    kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{120, 80, 30});
    {
        // A convex fillet on the vertical corner at (120, 80).
        std::vector<kernel::SubId> corner;
        for (std::uint32_t e = 0; e < s.count(kernel::SubKind::Edge); ++e) {
            auto c = kernel::curveInfo(s, kernel::SubId{kernel::SubKind::Edge, e});
            REQUIRE(c);
            if (std::abs(c->start.x - 120) < 1e-9 && std::abs(c->end.x - 120) < 1e-9 &&
                std::abs(c->start.y - 80) < 1e-9 && std::abs(c->end.y - 80) < 1e-9) {
                corner.push_back(kernel::SubId{kernel::SubKind::Edge, e});
            }
        }
        REQUIRE(corner.size() == 1);
        JobContext ctx;
        auto f = kernel::fillet(s, corner, 6, ctx);
        REQUIRE(f);
        s = f->shape;
    }
    s = fuse(s, cylinder(Vec3{100, 15, 29}, Vec3{0, 0, 1}, 5, 11)); // a boss, to z 40
    s = cut(s, cylinder(Vec3{30, 60, -1}, Vec3{std::sin(0.0872664626), 0, std::cos(0.0872664626)}, 3, 40)); // 5 deg
    s = cut(s, cylinder(Vec3{90, 65, -1}, Vec3{0, 0, 1}, 3, 11)); // blind from below: opens downwards, floor at 10
    s = cut(s, box(Vec3{-1, 30, 10}, Vec3{121, 50, 20}));         // a tunnel along X ...
    s = cut(s, cylinder(Vec3{60, 40, 5}, Vec3{0, 0, 1}, 3, 5.5)); // ... and a hole in its floor, under its ceiling
    s = cut(s, cylinder(Vec3{0, 20, -1}, Vec3{0, 0, 1}, 5, 32));  // a half-open cut-out on the edge x = 0
    s = cut(s, prism(roundedRect(60, 12, 20, 12, 3), 24, 7));     // a pocket with corner radii, floor at 24
    s = cut(s, prism(slot(22, 38, 15, 6), -1, 32));               // a through slot
    s = cut(s, cylinder(Vec3{60, 65, 22}, Vec3{0, 0, 1}, 10, 9)); // a round pocket, floor at 22 ...
    s = fuse(s, cylinder(Vec3{60, 65, 21}, Vec3{0, 0, 1}, 4, 7)); // ... running into an island up to 28
    INFO("faces " << s.count(kernel::SubKind::Face));

    SECTION("from above: nothing")
    {
        const auto found = holesOf(s);
        for (const RecognizedHole& h : found) {
            UNSCOPED_INFO(describe(h));
        }
        CHECK(found.empty());
    }
    SECTION("from below: the hole that opens downwards, nothing else")
    {
        expectHoles(holesOf(s, fromBelow()), {{90, -65, 0, HoleBottom::Flat, "plain", {{6, 6, 0, -10, 0}}}});
    }
    SECTION("in the frame along the tilted hole, nothing either: its top is not square to its axis (v1)")
    {
        // The frame whose Z is the tilted hole's axis: a turn of 5 degrees about Y. The hole enters the top face at an
        // ellipse, so it has no top ring; holes are recognised only where they enter a face square to them.
        const Xform frame = inverse(rotation(Vec3{0, 1, 0}, 0.0872664626, Vec3{0, 0, 0}));
        CHECK(holesOf(s, frame).empty());
    }
}

TEST_CASE("cross-drilled holes: each hole along Z is found with its nominal size", "[cam][holes]")
{
    // Block 60 x 60 x 40. A passage along X (diameter 12, z 20, y 30) through the whole block; a passage along Y
    // (diameter 4, z 25, x 40) from y = 0 to 22.
    kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{60, 60, 40});
    s = cut(s, cylinder(Vec3{20, 30, -1}, Vec3{0, 0, 1}, 4, 42));       // 1: through, cut in two by the X passage
    s = cut(s, revolved(40, 15, {{0, 41}, {5, 41}, {5, 15}, {0, 15}})); // 2: blind flat, crossed by the Y one
    s = cut(s, revolved(50, 30, {{0, 41}, {3, 41}, {3, 18}, {0, 18 - 3 * kTip}})); // 3: breaks into the X passage
    s = cut(s, cylinder(Vec3{-1, 30, 20}, Vec3{1, 0, 0}, 6, 62));
    s = cut(s, cylinder(Vec3{40, -1, 25}, Vec3{0, 1, 0}, 2, 23));

    SECTION("from above")
    {
        const auto found = holesOf(s);
        REQUIRE(found.size() == 3);
        expectHoles({found[0], found[1]}, {{20, 30, 40, HoleBottom::Through, "plain", {{8, 8, 40, 0, 0}}},
                                           {40, 15, 40, HoleBottom::Flat, "plain", {{10, 10, 40, 15, 0}}}});
        // Hole 3 drilled into the passage: it breaks out there, between z = 20 + sqrt(27) and 26 around its wall.
        const RecognizedHole& h3 = found[2];
        INFO(describe(h3));
        CHECK(toMm(h3.x) == 50);
        CHECK(h3.bottom == HoleBottom::Through);
        REQUIRE(h3.segments.size() == 1);
        CHECK(std::abs(h3.segments[0].diameter - 6) < kTol);
        CHECK(toMm(h3.zBottom) >= 20 + std::sqrt(27.0) - 1e-6);
        CHECK(toMm(h3.zBottom) <= 26 + 1e-6);
        // The through hole's wall is two faces, above and below the passage: one hole all the same.
        CHECK(found[0].faces.size() == 2);
    }
    SECTION("from below: only the through hole")
    {
        expectHoles(holesOf(s, fromBelow()), {{20, -30, 0, HoleBottom::Through, "plain", {{8, 8, 0, -40, 0}}}});
    }
}

TEST_CASE("hole recognition refuses bad input and stops when cancelled", "[cam][holes]")
{
    const kernel::Shape s = kindsBlock();
    JobContext ctx;
    SECTION("a null shape")
    {
        auto r = recognizeHoles(kernel::Shape{}, identityFrame(), ctx);
        REQUIRE(!r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("a frame that scales, mirrors or is not finite")
    {
        for (const Xform& bad :
             {Xform{{2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}, Xform{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0}},
              Xform{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, std::nan("")}}}) {
            auto r = recognizeHoles(s, bad, ctx);
            REQUIRE(!r);
            CHECK(r.error().code == ErrorCode::InvalidArgument);
        }
    }
    SECTION("cancelled")
    {
        CancelSource source;
        source.cancel();
        JobContext cancelled{source.token(), {}};
        auto r = recognizeHoles(s, identityFrame(), cancelled);
        REQUIRE(!r);
        CHECK(r.error().code == ErrorCode::Cancelled);
    }
}

TEST_CASE("a plate with 3000 holes is recognised in seconds", "[cam][holes][wallclock]")
{
    // 300 x 200 x 10, holes of 3 mm on a 5 x 3.9 mm grid (60 x 50): one extrusion with 3000 hole loops.
    std::vector<kernel::ProfileLoop> loops(1);
    loops[0].curves = roundedRect(150, 100, 300, 200, 0.5);
    for (int i = 0; i < 60; ++i) {
        for (int j = 0; j < 50; ++j) {
            kernel::ProfileLoop c;
            c.curves.push_back(
                kernel::ProfileCurve{kernel::ProfileCurve::Kind::Circle, {{2.5 + 5 * i, 2.5 + 3.9 * j}}, 1.5, true, 0});
            loops.push_back(c);
        }
    }
    JobContext ctx;
    auto plate = kernel::extrude(kernel::Profile{kernel::Plane{Vec3{0, 0, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0}}, loops}, 10,
                                 0.0, ctx);
    if (!plate) {
        FAIL("extrude failed: " << plate.error().message);
    }
    const kernel::Shape& s = plate->op.shape;
    const auto start = std::chrono::steady_clock::now();
    const auto found = holesOf(s);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "recognizeHoles: " << found.size() << " holes on " << s.count(kernel::SubKind::Face) << " faces in "
              << seconds << " s\n";
    CHECK(found.size() == 3000);
    CHECK(seconds < 10.0);

    // With references to every bore (IResolver::makeRef per hole), as the document would ask for them.
    const BodyId body{Uuid::fromString("00000000-0000-4000-8000-000000000b02")};
    const auto named = std::chrono::steady_clock::now();
    auto resolver = naming::makeResolver({naming::ResolverBody{
        body, naming::nameImported(FeatureId{Uuid::fromString("00000000-0000-4000-8000-000000000f02")}, s), s}});
    std::cout << "naming the plate: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - named).count()
              << " s\n";
    const auto start2 = std::chrono::steady_clock::now();
    JobContext ctx2;
    auto withRefs = recognizeHoles(s, *resolver, body, identityFrame(), ctx2);
    const double seconds2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - start2).count();
    REQUIRE(withRefs);
    std::cout << "recognizeHoles with references: " << withRefs->size() << " holes in " << seconds2 << " s\n";
    CHECK(withRefs->size() == 3000);
    CHECK(seconds2 < 30.0);
}
