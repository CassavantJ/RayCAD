// The tool engine and snapping of the sketch mode (task T-092), without a window: what each tool commits and which
// constraints it infers.
#include "sketch/sketch_geom.h"
#include "sketch/sketch_tools.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace ray;
using namespace ray::ui::sketchui;
using Catch::Approx;

namespace {

SketchEntId newId()
{
    return SketchEntId{Uuid::generate()};
}

// A 40 x 20 rectangle from the origin: points p[0..3], lines l[0..3] (bottom, right, top, left).
struct Rect {
    sketch::Sketch s;
    SketchEntId p[4], l[4];
    Rect()
    {
        const Vec2 c[4] = {{0, 0}, {40, 0}, {40, 20}, {0, 20}};
        for (int i = 0; i < 4; ++i) {
            p[i] = newId();
            s.entities.push_back(sketch::Entity{p[i], sketch::Point{c[i]}, false});
        }
        for (int i = 0; i < 4; ++i) {
            l[i] = newId();
            s.entities.push_back(sketch::Entity{l[i], sketch::Line{p[i], p[(i + 1) % 4]}, false});
        }
    }
};

int count(const Batch& b, sketch::ConstraintType type)
{
    int n = 0;
    for (const sketch::Constraint& c : b.constraints)
        n += c.type == type ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("snaps: points first, then midpoints, axis, curves and alignment", "[sketch][tools]")
{
    Rect r;
    const SketchView view(r.s);
    SECTION("an end point")
    {
        const Snap s = snapAt(view, {40.3, 19.8}, 1.0, {});
        CHECK(s.kind == Snap::Kind::Point);
        CHECK(s.ref == r.p[2]);
    }
    SECTION("a line's midpoint")
    {
        const Snap s = snapAt(view, {20.2, 0.4}, 1.0, {});
        CHECK(s.kind == Snap::Kind::Midpoint);
        CHECK(s.ref == r.l[0]);
        CHECK(s.p.x == Approx(20));
    }
    SECTION("on a curve")
    {
        const Snap s = snapAt(view, {30, 0.5}, 1.0, {});
        CHECK(s.kind == Snap::Kind::OnCurve);
        CHECK(s.p.y == Approx(0).margin(1e-12));
    }
    SECTION("horizontal from the anchor, onto the right side")
    {
        SnapOptions o;
        o.anchor = Vec2{-30, 13};
        const Snap s = snapAt(view, {40.5, 13.6}, 1.0, o);
        CHECK(s.axis == Snap::Axis::Horizontal);
        CHECK(s.kind == Snap::Kind::OnCurve);
        CHECK(s.p.x == Approx(40));
        CHECK(s.p.y == Approx(13));
    }
    SECTION("lined up with a point")
    {
        const Snap s = snapAt(view, {60, 19.6}, 1.0, {});
        CHECK(s.kind == Snap::Kind::Free);
        REQUIRE(s.alignH.has_value());
        CHECK(s.p.y == Approx(20));
    }
    SECTION("off: the pointer as it is")
    {
        SnapOptions o;
        o.enabled = false;
        const Snap s = snapAt(view, {40.3, 19.8}, 1.0, o);
        CHECK(s.kind == Snap::Kind::Free);
        CHECK(s.p.x == Approx(40.3));
    }
}

TEST_CASE("the line tool chains, shares points and infers horizontal and tangent", "[sketch][tools]")
{
    Rect r;
    const SketchView view(r.s);
    ToolEngine t;
    t.setKind(ToolKind::Line);
    t.hover(view, {40.2, 20.1}, 1.0, true);
    CHECK(t.click(view).kind == Action::Kind::None); // the start, on the corner
    t.hover(view, {60, 20.4}, 1.0, true);
    const Action a = t.click(view);
    REQUIRE(a.kind == Action::Kind::Add);
    // Only the new end point and the line: the corner is reused.
    CHECK(a.batch.entities.size() == 2);
    CHECK(count(a.batch, sketch::ConstraintType::Horizontal) == 1);
    CHECK(t.busy()); // the chain goes on
    // Typed length and angle place the next point exactly and add the length.
    t.setTyped("length", 10.0);
    t.setTyped("angle", kPi / 2);
    t.hover(view, {61, 25}, 1.0, true);
    const Action b = t.enter(view);
    REQUIRE(b.kind == Action::Kind::Add);
    CHECK(count(b.batch, sketch::ConstraintType::Distance) == 1);
    CHECK(count(b.batch, sketch::ConstraintType::Vertical) == 1);
    for (const sketch::Entity& e : b.batch.entities) {
        if (const auto* p = std::get_if<sketch::Point>(&e.geom)) {
            CHECK(p->p.x == Approx(60));
            CHECK(p->p.y == Approx(30));
        }
    }
    CHECK(t.cancel());
    CHECK(!t.busy());
}

TEST_CASE("rectangle, circle, arcs, slot and spline make the right entities", "[sketch][tools]")
{
    const sketch::Sketch empty;
    const SketchView view(empty);
    ToolEngine t;
    SECTION("rectangle: four lines, two horizontal, two vertical")
    {
        t.setKind(ToolKind::Rectangle);
        t.hover(view, {0, 0}, 0.1, true);
        t.click(view);
        t.hover(view, {30, 10}, 0.1, true);
        const Action a = t.click(view);
        REQUIRE(a.kind == Action::Kind::Add);
        CHECK(a.batch.entities.size() == 8);
        CHECK(count(a.batch, sketch::ConstraintType::Horizontal) == 2);
        CHECK(count(a.batch, sketch::ConstraintType::Vertical) == 2);
    }
    SECTION("circle with a typed diameter")
    {
        t.setKind(ToolKind::Circle);
        t.hover(view, {5, 5}, 0.1, true);
        t.click(view);
        t.setTyped("diameter", 12.0);
        t.hover(view, {9, 5}, 0.1, true);
        const Action a = t.enter(view);
        REQUIRE(a.kind == Action::Kind::Add);
        bool found = false;
        for (const sketch::Entity& e : a.batch.entities) {
            if (const auto* c = std::get_if<sketch::Circle>(&e.geom)) {
                CHECK(c->radius == Approx(6));
                found = true;
            }
        }
        CHECK(found);
        CHECK(count(a.batch, sketch::ConstraintType::Diameter) == 1);
    }
    SECTION("three-point arc: through the third point")
    {
        t.setKind(ToolKind::Arc3);
        for (const Vec2 p : {Vec2{10, 0}, Vec2{-10, 0}}) {
            t.hover(view, p, 0.1, true);
            t.click(view);
        }
        t.hover(view, {0, 10}, 0.1, true);
        const Action a = t.click(view);
        REQUIRE(a.kind == Action::Kind::Add);
        bool ccw = false;
        for (const sketch::Entity& e : a.batch.entities) {
            if (const auto* arc = std::get_if<sketch::Arc>(&e.geom))
                ccw = arc->ccw;
        }
        CHECK(ccw); // from (10, 0) over the top to (-10, 0)
    }
    SECTION("slot and spline")
    {
        t.setKind(ToolKind::Slot);
        for (const Vec2 p : {Vec2{0, 0}, Vec2{20, 0}}) {
            t.hover(view, p, 0.1, true);
            t.click(view);
        }
        t.hover(view, {10, 3}, 0.1, true);
        const Action slot = t.click(view);
        REQUIRE(slot.kind == Action::Kind::Add);
        bool found = false;
        for (const sketch::Entity& e : slot.batch.entities) {
            if (const auto* s = std::get_if<sketch::Slot>(&e.geom)) {
                CHECK(s->width == Approx(6));
                found = true;
            }
        }
        CHECK(found);
        t.setKind(ToolKind::Spline);
        for (const Vec2 p : {Vec2{0, 0}, Vec2{5, 5}, Vec2{10, 0}, Vec2{15, 5}}) {
            t.hover(view, p, 0.1, true);
            t.click(view);
        }
        const Action spline = t.enter(view);
        REQUIRE(spline.kind == Action::Kind::Add);
        CHECK(spline.batch.entities.size() == 5);
    }
}

TEST_CASE("tangent arc from a line end, and the editing tools' commands", "[sketch][tools]")
{
    Rect r;
    const SketchView view(r.s);
    ToolEngine t;
    t.setKind(ToolKind::ArcTangent);
    t.hover(view, {40, 20}, 0.5, true);
    t.click(view);
    t.hover(view, {50, 30}, 0.5, true);
    const Action a = t.click(view);
    REQUIRE(a.kind == Action::Kind::Add);
    CHECK(count(a.batch, sketch::ConstraintType::Tangent) == 1);

    t.setKind(ToolKind::Trim);
    t.hover(view, {20, 0.1}, 0.5, true);
    const Action trim = t.click(view);
    CHECK(trim.kind == Action::Kind::Command);
    CHECK(trim.command == "sketch.trim");
    CHECK(trim.argsJson.find(r.l[0].value.toString()) != std::string::npos);

    t.setKind(ToolKind::Fillet);
    t.setTyped("radius", 3.0);
    t.hover(view, {40.1, 0.1}, 0.5, true);
    const Action fillet = t.click(view);
    CHECK(fillet.command == "sketch.fillet");
    CHECK(fillet.argsJson.find("\"radius\":3") != std::string::npos);

    t.setKind(ToolKind::Offset);
    t.hover(view, {20, 0.1}, 0.5, true);
    CHECK(t.click(view).kind == Action::Kind::None); // picks the chain
    CHECK(t.offsetChain().size() == 4);
    t.hover(view, {20, 2}, 0.5, true);
    const Action offset = t.click(view);
    CHECK(offset.command == "sketch.offset");
    CHECK(offset.argsJson.find("\"distance\":2") != std::string::npos);
}

TEST_CASE("the point tool places free points with the constraints its snap infers", "[sketch][tools][point]")
{
    Rect r;
    const SketchView view(r.s);
    ToolEngine t;
    t.setKind(ToolKind::Point);
    CHECK(toolFromName("point") == ToolKind::Point);
    CHECK(toolName(ToolKind::Point) == "point");
    CHECK(!t.busy());

    // Free, inside the rectangle (nothing to line up with): one point, no constraint.
    t.hover(view, {13, 7}, 1.0, true);
    CHECK(t.draft().points.size() == 1);
    const Action free = t.click(view);
    REQUIRE(free.kind == Action::Kind::Add);
    REQUIRE(free.batch.entities.size() == 1);
    const auto* p = std::get_if<sketch::Point>(&free.batch.entities[0].geom);
    REQUIRE(p != nullptr);
    CHECK(p->p.x == Approx(13));
    CHECK(p->p.y == Approx(7));
    CHECK(!free.batch.entities[0].construction);
    CHECK(free.batch.constraints.empty());
    CHECK(!t.busy()); // one click, one point

    // On the bottom line: PointOnCurve with that line.
    t.hover(view, {30, 0.4}, 1.0, true);
    const Action onLine = t.click(view);
    REQUIRE(onLine.kind == Action::Kind::Add);
    REQUIRE(onLine.batch.constraints.size() == 1);
    CHECK(onLine.batch.constraints[0].type == sketch::ConstraintType::PointOnCurve);
    CHECK(onLine.batch.constraints[0].refs[1] == r.l[0]);

    // At the middle of the top line: Midpoint.
    t.hover(view, {20.3, 19.7}, 1.0, true);
    const Action mid = t.click(view);
    REQUIRE(mid.kind == Action::Kind::Add);
    CHECK(count(mid.batch, sketch::ConstraintType::Midpoint) == 1);

    // Lined up with a corner (outside the rectangle): Horizontal with it.
    t.hover(view, {60, 19.6}, 1.0, true);
    const Action aligned = t.click(view);
    REQUIRE(aligned.kind == Action::Kind::Add);
    CHECK(count(aligned.batch, sketch::ConstraintType::Horizontal) == 1);
    const auto* q = std::get_if<sketch::Point>(&aligned.batch.entities[0].geom);
    REQUIRE(q != nullptr);
    CHECK(q->p.y == Approx(20));

    // On an existing point: nothing (it is there already).
    t.hover(view, {40.2, 0.2}, 1.0, true);
    CHECK(t.click(view).kind == Action::Kind::None);

    // Typed X and Y place it exactly; Enter commits.
    const auto fields = t.fields();
    REQUIRE(fields.size() == 2);
    CHECK(fields[0].key == "x");
    CHECK(fields[1].key == "y");
    t.hover(view, {100, 100}, 1.0, true);
    t.setTyped("x", 12.5);
    t.setTyped("y", -3.0);
    const Action typed = t.enter(view);
    REQUIRE(typed.kind == Action::Kind::Add);
    const auto* tp = std::get_if<sketch::Point>(&typed.batch.entities[0].geom);
    REQUIRE(tp != nullptr);
    CHECK(tp->p.x == Approx(12.5));
    CHECK(tp->p.y == Approx(-3.0));
    CHECK(typed.batch.constraints.empty());
    CHECK(t.typed().empty());

    // Construction on: a construction point.
    t.setConstruction(true);
    t.hover(view, {13, 9}, 1.0, true);
    const Action c = t.click(view);
    REQUIRE(c.kind == Action::Kind::Add);
    CHECK(c.batch.entities[0].construction);
}

TEST_CASE("geometry helpers", "[sketch][tools]")
{
    const auto c = circleThrough({1, 0}, {0, 1}, {-1, 0});
    REQUIRE(c.has_value());
    CHECK(c->x == Approx(0).margin(1e-12));
    CHECK(c->y == Approx(0).margin(1e-12));
    CHECK(!circleThrough({0, 0}, {1, 1}, {2, 2}).has_value());
    const auto f = tangentArc({0, 0}, {1, 0}, {10, 10});
    REQUIRE(f.has_value());
    CHECK(f->center.x == Approx(0).margin(1e-12));
    CHECK(f->center.y == Approx(10));
    CHECK(f->ccw);
    const auto poly = splinePolyline({{0, 0}, {1, 2}, {3, 2}, {4, 0}}, 3, false);
    CHECK(poly.front().x == Approx(0));
    CHECK(poly.back().x == Approx(4));
}
