// Golden toolpaths of the engraving operation (task T-056): text on a plate, the edges of a pocket and a hole on a
// kernel-built part in two levels, a spline edge fitted with arcs, and the counterbore rims of a corpus part. Each is
// tests/golden/toolpaths/engrave_<name>.rctp with its .txt (engrave_test_util.h matchesToolpathGolden), and each passes
// the full toolpath invariant checker with the claim that cutting the part is intended only where the engraving goes:
// within the cut's width of the strokes (cylinders round the text, the pocket, the hole), no deeper than the depth.
#include "engrave_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::ops;
using namespace ray::cam::test;

namespace {

// The plate of the golden: 80 x 50 x 10, its top at Z 0, with a 20 x 12 rectangular pocket 3 deep at (10..30, 30..42)
// and an 8 mm hole 5 deep at (60, 36).
const kernel::Shape& plate()
{
    static const kernel::Shape s = [] {
        kernel::Shape b = box(Vec3{0, 0, -10}, Vec3{80, 50, 0});
        b = cut(b, box(Vec3{10, 30, -3}, Vec3{30, 42, 1}));
        b = cut(b, revolved(60, 36, {{0, 1}, {4, 1}, {4, -5}, {0, -5}}));
        return b;
    }();
    return s;
}

// The edges of the plate's top face that are not on its outer boundary: the pocket's and the hole's rims.
std::vector<kernel::SubId> rims(const kernel::Shape& s)
{
    std::vector<kernel::SubId> out;
    for (const kernel::SubId e : topEdges(s, 0)) {
        auto ci = kernel::curveInfo(s, e);
        REQUIRE(ci);
        const auto onBoundary = [](const Vec3& p) {
            return std::abs(p.x) < 1e-6 || std::abs(p.x - 80) < 1e-6 || std::abs(p.y) < 1e-6 ||
                   std::abs(p.y - 50) < 1e-6;
        };
        if (!(onBoundary(ci->start) && onBoundary(ci->end))) {
            out.push_back(e);
        }
    }
    return out;
}

} // namespace

TEST_CASE("golden: engraving a line of text", "[cam][engrave][golden]")
{
    OpCase c = engraveCase(plate(), {},
                           R"j({"text":"RayCAD 1/4-20 #8 (x)","textHeight":4,"textX":4,"textY":8,"depth":0.15})j");
    // Allowed: a cylinder round the text's box (width from the font, 4 mm high, descenders 1.33 mm) and the groove.
    TextLayout l;
    l.text = "RayCAD 1/4-20 #8 (x)";
    l.height = 4;
    const double w = textWidthMm(*engravingFont(), l);
    const Toolpath p = c.path([&](invariants::Claims& k) {
        k.allowedCuts.push_back(invariants::AllowedCut{4 + w / 2, 8 + 1.33, std::hypot(w / 2, 3.4) + 0.5, -0.16, 5});
        k.floorZ = -0.15;
    });
    matchesToolpathGolden("engrave_text", p);
    for (const Move& m : p.moves) {
        CHECK(m.z >= -150'000);
    }
}

TEST_CASE("golden: engraving the edges of a pocket and a hole in two levels", "[cam][engrave][golden]")
{
    const std::vector<kernel::SubId> edges = rims(plate());
    REQUIRE(edges.size() == 5); // the pocket's four sides and the hole's circle
    OpCase c = engraveCase(plate(), edges, R"({"depth":0.4,"stepDown":0.25})", engraver(6, 90, 0));
    const Toolpath p = c.path([](invariants::Claims& k) {
        // A 90 degree cutter 0.4 deep cuts 0.4 mm either side of the edge.
        k.allowedCuts.push_back(invariants::AllowedCut{20, 36, std::hypot(10.0, 6.0) + 0.5, -0.41, 5});
        k.allowedCuts.push_back(invariants::AllowedCut{60, 36, 4.5, -0.41, 5});
        k.floorZ = -0.4;
    });
    matchesToolpathGolden("engrave_edges", p);
    // Two levels (0.2 and 0.4: ceil(0.4 / 0.25) = 2, 0.4 * 1/2) of two closed paths.
    std::set<Nm> levels;
    for (const Move& m : p.moves) {
        if (m.role == MoveRole::Cut) {
            levels.insert(m.z);
        }
    }
    CHECK(levels == std::set<Nm>{-400'000, -200'000});
}

TEST_CASE("golden: a spline edge comes as short lines and is engraved as arcs", "[cam][engrave][golden]")
{
    // A slab whose front face is a spline: extruded 5 mm from a profile of one B-spline and three lines.
    kernel::ProfileLoop loop;
    using K = kernel::ProfileCurve::Kind;
    loop.curves.push_back(kernel::ProfileCurve{K::Bspline, {{10, 10}, {20, 25}, {35, 5}, {50, 18}}, 0, false, 0});
    loop.curves.push_back(kernel::ProfileCurve{K::Line, {{50, 18}, {50, 40}}, 0, false, 0});
    loop.curves.push_back(kernel::ProfileCurve{K::Line, {{50, 40}, {10, 40}}, 0, false, 0});
    loop.curves.push_back(kernel::ProfileCurve{K::Line, {{10, 40}, {10, 10}}, 0, false, 0});
    JobContext ctx;
    auto slab = kernel::extrude(kernel::Profile{kernel::Plane{Vec3{0, 0, -5}, Vec3{0, 0, 1}, Vec3{1, 0, 0}}, {loop}}, 5,
                                0, ctx);
    REQUIRE(slab);
    const kernel::Shape s = slab->op.shape;
    std::vector<kernel::SubId> spline;
    for (const kernel::SubId e : topEdges(s, 0)) {
        auto ci = kernel::curveInfo(s, e);
        REQUIRE(ci);
        if (ci->type != kernel::CurveType::Line) { // the spline (OCCT may report it as Bspline or Other)
            spline.push_back(e);
        }
    }
    REQUIRE(spline.size() == 1);
    OpCase fitted = engraveCase(s, spline, R"({"depth":0.1})");
    OpCase lines = engraveCase(s, spline, R"({"depth":0.1,"fitArcs":false})");
    const auto claims = [](invariants::Claims& k) {
        k.allowedCuts.push_back(invariants::AllowedCut{30, 15, 22, -0.11, 5});
        k.floorZ = -0.1;
    };
    const Toolpath p = fitted.path(claims);
    const Toolpath q = lines.path(claims);
    matchesToolpathGolden("engrave_spline", p);
    std::size_t arcs = 0, cuts = 0, lineCuts = 0;
    for (const Move& m : p.moves) {
        cuts += m.role == MoveRole::Cut ? 1 : 0;
        arcs += m.role == MoveRole::Cut && m.kind != MoveKind::Linear ? 1 : 0;
    }
    for (const Move& m : q.moves) {
        lineCuts += m.role == MoveRole::Cut ? 1 : 0;
        CHECK((m.role != MoveRole::Cut || m.kind == MoveKind::Linear));
    }
    INFO(cuts << " cut moves fitted (" << arcs << " arcs), " << lineCuts << " as lines");
    CHECK(arcs > 0);
    CHECK(cuts * 4 < lineCuts);
}

TEST_CASE("golden: engraving the counterbore rims of a corpus part", "[cam][engrave][golden][corpus]")
{
    // plate_counterbore_1: four counterbores 11 mm across from Z 15 (tests/unit/cam/holes_labels.txt).
    JobContext ctx;
    auto imported =
        kernel::importStep(std::string(RAY_CORPUS_DIR) + "/raycad-synthetic/plate_counterbore_1.stp", {}, ctx);
    REQUIRE(imported);
    const kernel::Shape part = imported->bodies.at(0).shape;
    std::vector<kernel::SubId> rimEdges;
    std::vector<invariants::AllowedCut> allowed;
    for (const RecognizedHole& h : holesOf(part)) {
        const auto e = topEdges(part, 15, toMm(h.x), toMm(h.y), 5.5);
        rimEdges.insert(rimEdges.end(), e.begin(), e.end());
        allowed.push_back(invariants::AllowedCut{toMm(h.x), toMm(h.y), 6.5, 14.69, 20});
    }
    REQUIRE(rimEdges.size() >= 4);
    OpCase c = engraveCase(part, rimEdges, R"({"depth":0.3})", engraver(6, 90, 0));
    c.op.heights.top = 15;
    c.op.heights.feedPlane = 17;
    c.op.heights.retract = 20;
    c.op.heights.clearance = 30;
    const Toolpath p = c.path([&](invariants::Claims& k) { k.allowedCuts = allowed; });
    matchesToolpathGolden("engrave_corpus", p);
}

TEST_CASE("engraving gives the same bytes every time", "[cam][engrave][golden]")
{
    const auto make = [] {
        return engraveCase(plate(), rims(plate()),
                           R"({"text":"Op 10","textX":40,"textY":10,"textAngle":30,"depth":0.2})")
            .generate();
    };
    const auto first = make();
    REQUIRE(first);
    for (int i = 0; i < 3; ++i) {
        const auto again = make();
        REQUIRE(again);
        CHECK(again->serialize() == first->serialize());
    }
}
