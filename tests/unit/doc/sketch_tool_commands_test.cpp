// The sketch commands added for the drawing tools (task T-092): sketch.entity.add with inferred constraints,
// sketch.construction, sketch.extend, sketch.offset and sketch.fillet.
#include "parametric_test_util.h"

#include "features/sketch_feature.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>

using namespace ray;
using namespace ray::doc;
using namespace ray::doc::test;
using Catch::Approx;

namespace {

struct Model {
    DocStatePtr state = std::make_shared<const DocState>();

    void run(const Command& c)
    {
        const ICommandHandler* handler = builtinCommandRegistry()->find(c.name);
        REQUIRE(handler != nullptr);
        auto next = handler->apply(state, c.argsJson);
        if (!next) {
            FAIL(c.name << " failed: " << next.error().message);
        }
        state = *next;
    }
    Error refused(const Command& c) const
    {
        const ICommandHandler* handler = builtinCommandRegistry()->find(c.name);
        REQUIRE(handler != nullptr);
        auto next = handler->apply(state, c.argsJson);
        REQUIRE(!next);
        return next.error();
    }
    sketch::Sketch sketch() const
    {
        REQUIRE(!state->features.empty());
        auto s = features::sketchOfFeature(state->features.back(), false);
        REQUIRE(s);
        return *s;
    }
};

std::string sk()
{
    return uuid('f', 2);
}

const sketch::Entity* find(const sketch::Sketch& s, const std::string& id)
{
    for (const sketch::Entity& e : s.entities) {
        if (e.id.value == Uuid::fromString(id)) {
            return &e;
        }
    }
    return nullptr;
}

Vec2 pointOf(const sketch::Sketch& s, const SketchEntId& id)
{
    for (const sketch::Entity& e : s.entities) {
        if (e.id == id) {
            return std::get<sketch::Point>(e.geom).p;
        }
    }
    FAIL("no point " << id.value.toString());
    return {};
}

int count(const sketch::Sketch& s, sketch::ConstraintType type)
{
    int n = 0;
    for (const sketch::Constraint& c : s.constraints) {
        n += c.type == type ? 1 : 0;
    }
    return n;
}

template <class T> int countOf(const sketch::Sketch& s)
{
    int n = 0;
    for (const sketch::Entity& e : s.entities) {
        n += std::holds_alternative<T>(e.geom) ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("sketch.entity.add takes the inferred constraints in the same step", "[doc][sketch][tools]")
{
    Model m;
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", SketchJson{})));
    SketchJson add;
    add.point(1, 0, 0).point(2, 10, 0).line(3, 1, 2).constraint(1, "horizontal", {3});
    // {"schema":1,"entities":[...],"constraints":[...]}: the same two arrays are the arguments.
    const std::string json = add.json();
    m.run(cmd("sketch.entity.add", R"({"id":")" + sk() + R"(",)" + json.substr(json.find("\"entities\""))));
    const sketch::Sketch s = m.sketch();
    CHECK(s.entities.size() == 3);
    REQUIRE(s.constraints.size() == 1);
    CHECK(s.constraints[0].type == sketch::ConstraintType::Horizontal);

    SECTION("a constraint on an entity that does not exist is refused, and nothing is added")
    {
        const Error e = m.refused(cmd("sketch.entity.add", R"({"id":")" + sk() + R"(","entities":[{"id":")" + ent(4) +
                                                               R"(","type":"point","x":1,"y":1}],"constraints":[{"id":")" +
                                                               uuid('c', 2) + R"(","type":"fix","refs":[")" + ent(99) +
                                                               R"("]}]})"));
        CHECK(e.code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("sketch.construction marks and unmarks entities", "[doc][sketch][tools]")
{
    Model m;
    SketchJson s;
    s.point(1, 0, 0).point(2, 10, 0).line(3, 1, 2);
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", s)));
    m.run(cmd("sketch.construction",
              R"({"id":")" + sk() + R"(","entities":[")" + ent(3) + R"("],"construction":true})"));
    CHECK(find(m.sketch(), ent(3))->construction);
    m.run(cmd("sketch.construction",
              R"({"id":")" + sk() + R"(","entities":[")" + ent(3) + R"("],"construction":false})"));
    CHECK(!find(m.sketch(), ent(3))->construction);
    CHECK(m.refused(cmd("sketch.construction", R"({"id":")" + sk() + R"(","entities":[")" + ent(9) +
                                                   R"("],"construction":true})"))
              .code == ErrorCode::InvalidArgument);
}

TEST_CASE("sketch.extend grows a line or an arc to the next curve", "[doc][sketch][tools]")
{
    Model m;
    // A wall at x = 20 (from y -10 to 10); a line from (0, 0) to (5, 0); a line from (0, 5) to (5, 5) that joins
    // another line at (5, 5).
    SketchJson s;
    s.point(1, 20, -10).point(2, 20, 10).line(3, 1, 2);
    s.point(4, 0, 0).point(5, 5, 0).line(6, 4, 5);
    s.point(7, 0, 5).point(8, 5, 5).line(9, 7, 8).point(10, 5, 9).line(11, 8, 10);
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", s)));

    SECTION("a free end moves to the crossing; the line keeps its id and its points")
    {
        m.run(cmd("sketch.extend", R"({"id":")" + sk() + R"(","entity":")" + ent(6) + R"(","at":{"x":4,"y":0}})"));
        const sketch::Sketch t = m.sketch();
        const auto& l = std::get<sketch::Line>(find(t, ent(6))->geom);
        CHECK(l.b.value == Uuid::fromString(ent(5)));
        CHECK(pointOf(t, l.b).x == Approx(20));
        CHECK(pointOf(t, l.b).y == Approx(0).margin(1e-12));
    }
    SECTION("a shared end gets a new point; the other line keeps the old one")
    {
        m.run(cmd("sketch.extend", R"({"id":")" + sk() + R"(","entity":")" + ent(9) +
                                       R"(","at":{"x":4.9,"y":5},"newPoint":")" + ent(40) + R"("})"));
        const sketch::Sketch t = m.sketch();
        const auto& l = std::get<sketch::Line>(find(t, ent(9))->geom);
        CHECK(l.b.value == Uuid::fromString(ent(40)));
        CHECK(pointOf(t, l.b).x == Approx(20));
        CHECK(pointOf(t, SketchEntId{Uuid::fromString(ent(8))}).x == Approx(5));
    }
    SECTION("the start end grows backwards; nothing to reach is refused")
    {
        const Error e =
            m.refused(cmd("sketch.extend", R"({"id":")" + sk() + R"(","entity":")" + ent(6) + R"(","at":{"x":1,"y":0}})"));
        CHECK(e.message.find("no curve to extend to") != std::string::npos);
    }
    SECTION("an arc grows along its circle")
    {
        Model a;
        SketchJson arc;
        // Quarter arc of radius 10 about the origin from (10, 0) to (0, 10), counter-clockwise; a wall at x = -5.
        arc.point(1, 0, 0).point(2, 10, 0).point(3, 0, 10).arc(4, 1, 2, 3, true);
        arc.point(5, -5, -20).point(6, -5, 20).line(7, 5, 6);
        a.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", arc)));
        a.run(cmd("sketch.extend", R"({"id":")" + sk() + R"(","entity":")" + ent(4) + R"(","at":{"x":0,"y":9}})"));
        const sketch::Sketch t = a.sketch();
        const Vec2 end = pointOf(t, std::get<sketch::Arc>(find(t, ent(4))->geom).end);
        CHECK(end.x == Approx(-5));
        CHECK(end.y == Approx(std::sqrt(75.0)));
    }
}

TEST_CASE("sketch.offset copies a chain on one side, joined at the corners", "[doc][sketch][tools]")
{
    Model m;
    // An open chain: (0, 0) -> (10, 0) -> (10, 10); and a circle of radius 5 at (30, 0).
    SketchJson s;
    s.point(1, 0, 0).point(2, 10, 0).point(3, 10, 10).line(4, 1, 2).line(5, 2, 3);
    s.circleAt(6, 30, 0, 5);
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", s)));

    SECTION("inside the corner: the two new lines meet at (8, 2)")
    {
        m.run(cmd("sketch.offset", R"({"id":")" + sk() + R"(","entities":[")" + ent(4) + R"(",")" + ent(5) +
                                       R"("],"distance":2,"side":{"x":5,"y":1}})"));
        const sketch::Sketch t = m.sketch();
        CHECK(countOf<sketch::Line>(t) == 4);
        CHECK(count(t, sketch::ConstraintType::Parallel) == 2);
        bool corner = false;
        for (const sketch::Entity& e : t.entities) {
            if (const auto* p = std::get_if<sketch::Point>(&e.geom)) {
                corner = corner || (std::abs(p->p.x - 8) < 1e-9 && std::abs(p->p.y - 2) < 1e-9);
            }
        }
        CHECK(corner);
    }
    SECTION("a circle grows on the outside and shares its centre")
    {
        m.run(cmd("sketch.offset", R"({"id":")" + sk() + R"(","entities":[")" + ent(7) +
                                       R"("],"distance":1.5,"side":{"x":40,"y":0}})"));
        const sketch::Sketch t = m.sketch();
        REQUIRE(countOf<sketch::Circle>(t) == 2);
        for (const sketch::Entity& e : t.entities) {
            if (const auto* c = std::get_if<sketch::Circle>(&e.geom)) {
                CHECK(c->center.value == Uuid::fromString(ent(6)));
                CHECK((c->radius == Approx(5) || c->radius == Approx(6.5)));
            }
        }
    }
    SECTION("inwards by more than the radius is refused")
    {
        CHECK(m.refused(cmd("sketch.offset", R"({"id":")" + sk() + R"(","entities":[")" + ent(7) +
                                                 R"("],"distance":6,"side":{"x":30,"y":1}})"))
                  .code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("sketch.fillet rounds the corner of two lines with a tangent arc", "[doc][sketch][tools]")
{
    Model m;
    SketchJson s;
    s.point(1, 0, 0).point(2, 10, 0).point(3, 10, 10).line(4, 1, 2).line(5, 2, 3);
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", s)));
    m.run(cmd("sketch.fillet",
              R"({"id":")" + sk() + R"(","point":")" + ent(2) + R"(","radius":2,"newEntity":")" + ent(20) + R"("})"));
    const sketch::Sketch t = m.sketch();
    CHECK(find(t, ent(2)) == nullptr); // the corner point went
    const sketch::Entity* arc = find(t, ent(20));
    REQUIRE(arc != nullptr);
    const auto& a = std::get<sketch::Arc>(arc->geom);
    const Vec2 c = pointOf(t, a.center);
    CHECK(c.x == Approx(8));
    CHECK(c.y == Approx(2));
    const auto& l4 = std::get<sketch::Line>(find(t, ent(4))->geom);
    const auto& l5 = std::get<sketch::Line>(find(t, ent(5))->geom);
    CHECK(pointOf(t, l4.b).x == Approx(8));
    CHECK(l4.b == a.start);
    CHECK(l5.a == a.end);
    CHECK(pointOf(t, l5.a).y == Approx(2));
    CHECK(count(t, sketch::ConstraintType::Tangent) == 2);
    // The rebuilt sketch solves with the fillet in it.
    RebuildEnv env;
    auto build = env.rebuild(m.state);
    REQUIRE(build);
    REQUIRE(!build->reports.empty());
    CHECK(build->reports.back().status != FeatureStatus::Failed);

    SECTION("too large a radius is refused")
    {
        Model n;
        n.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", s)));
        CHECK(n.refused(cmd("sketch.fillet", R"({"id":")" + sk() + R"(","point":")" + ent(2) + R"(","radius":20})"))
                  .message.find("too large") != std::string::npos);
    }
}
