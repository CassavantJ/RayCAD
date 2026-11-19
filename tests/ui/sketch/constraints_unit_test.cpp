// The Qt-free parts of constraints and dimensions (task T-093): what the smart dimension tool makes from picks, how a
// dimension measures and is laid out, and which geometric constraint a selection takes.
#include "sketch/constraints.h"
#include "sketch/dimensions.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace ray;
using namespace ray::ui::sketchui;
using Catch::Approx;
using sketch::ConstraintType;

namespace {

struct Builder {
    sketch::Sketch s;
    SketchEntId point(double x, double y)
    {
        const SketchEntId id{Uuid::generate()};
        s.entities.push_back({id, sketch::Point{{x, y}}, false});
        return id;
    }
    SketchEntId line(Vec2 a, Vec2 b)
    {
        const SketchEntId pa = point(a.x, a.y), pb = point(b.x, b.y), id{Uuid::generate()};
        s.entities.push_back({id, sketch::Line{pa, pb}, false});
        return id;
    }
    SketchEntId circle(Vec2 c, double r)
    {
        const SketchEntId pc = point(c.x, c.y), id{Uuid::generate()};
        s.entities.push_back({id, sketch::Circle{pc, r}, false});
        return id;
    }
    SketchEntId arc(Vec2 c, Vec2 a, Vec2 b)
    {
        const SketchEntId pc = point(c.x, c.y), pa = point(a.x, a.y), pb = point(b.x, b.y), id{Uuid::generate()};
        s.entities.push_back({id, sketch::Arc{pc, pa, pb, true}, false});
        return id;
    }
    SketchView view() const { return SketchView(s); }
};

const ViewCurve& curveOf(const SketchView& v, const SketchEntId& id)
{
    const ViewCurve* c = v.curve(id);
    REQUIRE(c != nullptr);
    return *c;
}

} // namespace

TEST_CASE("the smart dimension picks length, distance, angle, radius or diameter", "[sketch][dimensions][unit]")
{
    Builder b;
    const SketchEntId bottom = b.line({0, 0}, {40, 0});
    const SketchEntId slanted = b.line({0, 10}, {30, 40});
    const SketchEntId parallel = b.line({0, 20}, {40, 20});
    const SketchEntId circle = b.circle({60, 10}, 5);
    const SketchEntId arc = b.arc({90, 0}, {100, 0}, {90, 10});
    const SketchEntId p = b.point(10, 30), q = b.point(25, 50);
    const SketchView v = b.view();

    SECTION("one line: its length; a circle: its diameter; an arc: its radius")
    {
        auto d = pickDimension(v, {bottom}, {20, -5});
        REQUIRE(d);
        CHECK(d->type == ConstraintType::Distance);
        CHECK(d->value == Approx(40));
        d = pickDimension(v, {circle}, {70, 20});
        REQUIRE(d);
        CHECK(d->type == ConstraintType::Diameter);
        CHECK(d->value == Approx(10));
        d = pickDimension(v, {arc}, {100, 10});
        REQUIRE(d);
        CHECK(d->type == ConstraintType::Radius);
        CHECK(d->value == Approx(10));
    }
    SECTION("two points: aligned, or horizontal / vertical by where the cursor is")
    {
        auto d = pickDimension(v, {p, q}, {30, 60}); // beside the box diagonally: aligned
        REQUIRE(d);
        CHECK(d->type == ConstraintType::Distance);
        CHECK(d->value == Approx(25));
        d = pickDimension(v, {q, p}, {18, 60}); // above the box: horizontal, positive
        REQUIRE(d);
        CHECK(d->type == ConstraintType::DistanceX);
        CHECK(d->value == Approx(15));
        CHECK(d->refs == std::vector<SketchEntId>{p, q});
        d = pickDimension(v, {p, q}, {0, 40}); // left of it: vertical
        REQUIRE(d);
        CHECK(d->type == ConstraintType::DistanceY);
        CHECK(d->value == Approx(20));
    }
    SECTION("a point and a line: perpendicular distance; the line's own end: its length")
    {
        auto d = pickDimension(v, {p, bottom}, {5, 15});
        REQUIRE(d);
        CHECK(d->type == ConstraintType::Distance);
        CHECK(d->value == Approx(30));
        const SketchEntId end = curveOf(v, bottom).points[1];
        d = pickDimension(v, {end, bottom}, {5, 15});
        REQUIRE(d);
        CHECK(d->refs == std::vector<SketchEntId>{bottom});
        CHECK(d->value == Approx(40));
    }
    SECTION("two lines: the angle, or the distance when parallel")
    {
        auto d = pickDimension(v, {bottom, slanted}, {20, 5});
        REQUIRE(d);
        CHECK(d->type == ConstraintType::Angle);
        CHECK(std::abs(d->value) == Approx(std::atan2(30.0, 30.0)));
        d = pickDimension(v, {bottom, parallel}, {50, 10});
        REQUIRE(d);
        CHECK(d->type == ConstraintType::Distance);
        CHECK(d->value == Approx(20));
    }
    SECTION("nothing sensible: nothing")
    {
        CHECK(!pickDimension(v, {p}, {0, 0}));
        CHECK(!pickDimension(v, {}, {0, 0}));
    }
}

TEST_CASE("a dimension's layout follows its label; signed values keep their sign", "[sketch][dimensions][unit]")
{
    Builder b;
    const SketchEntId bottom = b.line({0, 0}, {40, 0});
    const SketchEntId circle = b.circle({60, 10}, 5);
    const SketchView v = b.view();
    sketch::Constraint length;
    length.type = ConstraintType::Distance;
    length.refs = {bottom};
    auto shape = layoutDimension(v, length, Vec2{0, -10}, 2);
    REQUIRE(shape);
    CHECK(shape->anchor.x == Approx(20));
    CHECK(shape->label.y == Approx(-10));
    CHECK(shape->arrows.size() == 2);
    // The dimension line runs at the label's height.
    bool atLabel = false;
    for (const auto& line : shape->lines)
        atLabel = atLabel || (line.size() == 2 && line[0].y == Approx(-10) && line[1].y == Approx(-10));
    CHECK(atLabel);

    sketch::Constraint dia;
    dia.type = ConstraintType::Diameter;
    dia.refs = {circle};
    shape = layoutDimension(v, dia, std::nullopt, 2);
    REQUIRE(shape);
    CHECK(shape->anchor.x == Approx(60));
    CHECK(std::hypot(shape->label.x - 60, shape->label.y - 10) > 5);
    CHECK(measureSigned(v, dia).value() == Approx(10));

    CHECK(signedValue(ConstraintType::DistanceX, -15, 20) == Approx(-20));
    CHECK(signedValue(ConstraintType::Distance, -15, 20) == Approx(20));
    CHECK(shownValue(ConstraintType::Angle, -0.5) == Approx(0.5));
}

TEST_CASE("geometric constraints fit the selection or say what to select", "[sketch][constraints][unit]")
{
    Builder b;
    const SketchEntId l1 = b.line({0, 0}, {30, 2});
    const SketchEntId l2 = b.line({0, 10}, {28, 14});
    const SketchEntId c1 = b.circle({50, 10}, 5);
    const SketchEntId c2 = b.circle({70, 10}, 3);
    const SketchEntId p = b.point(10, 30), q = b.point(20, 31), r = b.point(15, 60);
    const SketchView v = b.view();
    const sketch::Sketch& m = b.s;

    CHECK(planConstraint(GeoKind::Horizontal, v, m, {l1, l2}).size() == 2);
    CHECK(planConstraint(GeoKind::Horizontal, v, m, {p, q}).size() == 1);
    CHECK(planConstraint(GeoKind::Horizontal, v, m, {c1}).empty());
    CHECK(planConstraint(GeoKind::Parallel, v, m, {l1, l2}).front().type == ConstraintType::Parallel);
    CHECK(planConstraint(GeoKind::Perpendicular, v, m, {l1}).empty());
    CHECK(planConstraint(GeoKind::Tangent, v, m, {l1, c1}).size() == 1);
    CHECK(planConstraint(GeoKind::Tangent, v, m, {l1, l2}).empty());
    CHECK(planConstraint(GeoKind::Equal, v, m, {c1, c2}).size() == 1);
    CHECK(planConstraint(GeoKind::Equal, v, m, {c1, l1}).empty());
    CHECK(planConstraint(GeoKind::Midpoint, v, m, {p, l1}).front().type == ConstraintType::Midpoint);
    CHECK(planConstraint(GeoKind::Concentric, v, m, {c1, c2}).size() == 1);
    CHECK(planConstraint(GeoKind::Collinear, v, m, {l1, l2}).size() == 2);
    CHECK(planConstraint(GeoKind::Symmetric, v, m, {p, q, l2}).size() == 1);
    CHECK(planConstraint(GeoKind::Coincident, v, m, {p, q}).front().type == ConstraintType::Coincident);
    CHECK(planConstraint(GeoKind::Coincident, v, m, {p, c1}).front().type == ConstraintType::PointOnCurve);
    CHECK(planConstraint(GeoKind::Fix, v, m, {p, l1}).size() == 2);
    // Three points: the one nearest the middle of the others is the centre.
    const auto sym = planConstraint(GeoKind::Symmetric, v, m, {p, r, q});
    REQUIRE(sym.size() == 1);
    CHECK(sym.front().refs.size() == 3);
    // Already there: nothing more, and `already` says so.
    sketch::Sketch with = m;
    with.constraints = planConstraint(GeoKind::Horizontal, v, m, {l1});
    bool already = false;
    CHECK(planConstraint(GeoKind::Horizontal, v, with, {l1}, &already).empty());
    CHECK(already);
    for (const GeoKind k : kGeoKinds) {
        CHECK(geoFromName(geoName(k)) == k);
        CHECK(std::string(geoRequirement(k)).rfind("Select", 0) == 0);
    }
}
