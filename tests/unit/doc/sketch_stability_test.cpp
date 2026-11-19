// Sketch entity ids, loop (region) ids and curve tags stay stable through solves, dimension edits, unrelated entities,
// trims and splits, so the faces swept from untouched curves keep their names (task T-096, PLAN.md 3.3 rule 1).
#include "parametric_test_util.h"

#include "features/sketch_feature.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>

using namespace ray;
using namespace ray::doc;
using namespace ray::doc::test;

namespace {

struct Model {
    DocStatePtr state = std::make_shared<const DocState>();
    RebuildEnv env;

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
    BuildResultPtr build() { return env.rebuild(state); }
    sketch::Sketch sketchModel(std::uint64_t n) const
    {
        for (const Feature& f : state->features) {
            if (f.id == featureId(n)) {
                auto s = features::sketchOfFeature(f, false);
                REQUIRE(s);
                return *s;
            }
        }
        FAIL("no sketch");
        return {};
    }
};

bool hasEntity(const sketch::Sketch& s, std::uint64_t n)
{
    for (const sketch::Entity& e : s.entities) {
        if (e.id.value == Uuid::fromString(ent(n))) {
            return true;
        }
    }
    return false;
}

// The faces of the plate by what made them.
enum class Face { Bottom, Right, Top, Left, Hole, Floor, Lid };
const char* faceName(Face f)
{
    switch (f) {
    case Face::Bottom:
        return "side of line 5 (y = 0)";
    case Face::Right:
        return "side of line 6 (x = w)";
    case Face::Top:
        return "side of line 7 (y = 20)";
    case Face::Left:
        return "side of line 8 (x = 0)";
    case Face::Hole:
        return "hole wall (circle 11)";
    case Face::Floor:
        return "start cap";
    case Face::Lid:
        return "end cap";
    }
    return "?";
}

std::map<Face, kernel::SubId> classify(const kernel::Shape& shape)
{
    std::map<Face, kernel::SubId> out;
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId id{kernel::SubKind::Face, i};
        auto s = kernel::surfaceInfo(shape, id);
        REQUIRE(s);
        if (s->type == kernel::SurfaceType::Cylinder) {
            out[Face::Hole] = id;
        } else if (near(s->centroid.z, 0)) {
            out[Face::Floor] = id;
        } else if (near(s->centroid.z, 10)) {
            out[Face::Lid] = id;
        } else if (near(s->bounds.min.y, 0) && near(s->bounds.max.y, 0)) {
            out[Face::Bottom] = id;
        } else if (near(s->bounds.min.y, 20) && near(s->bounds.max.y, 20)) {
            out[Face::Top] = id; // with a split top line, either piece
        } else if (near(s->bounds.min.x, 0) && near(s->bounds.max.x, 0)) {
            out[Face::Left] = id;
        } else {
            out[Face::Right] = id;
        }
    }
    return out;
}

} // namespace

TEST_CASE("faces from untouched sketch curves keep Exact names through dimension edits, new entities, trims and "
          "splits",
          "[doc][parametric][stability]")
{
    Model m;
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","name":"w","expression":"40 mm","kind":"length"})"));
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 2) + R"(","name":"hx","expression":"20 mm","kind":"length"})"));
    // A 40 x 20 plate whose bottom line runs on past its left corner to x = -10 (trimmed later), with a hole.
    SketchJson s;
    s.point(1, 0, 0).point(2, 40, 0).point(3, 40, 20).point(4, 0, 20).point(20, -10, 0);
    s.line(5, 20, 2).line(6, 2, 3).line(7, 3, 4).line(8, 4, 1);
    s.circleAt(10, 20, 10, 3);
    s.constraint(1, "fix", {1})
        .constraint(2, "horizontal", {5})
        .constraint(3, "horizontal", {7})
        .constraint(4, "vertical", {6})
        .constraint(5, "vertical", {8})
        .constraint(6, "distanceX", {1, 2}, "w")
        .constraint(7, "distanceY", {1, 4}, "20 mm")
        .constraint(8, "pointOnCurve", {1, 5})
        .constraint(9, "distanceX", {1, 10}, "hx")
        .constraint(10, "distanceY", {1, 10}, "10 mm")
        .constraint(11, "radius", {11}, "3 mm");
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", s)));
    // The plate region is picked by its outline (lines 5 to 8).
    m.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b', R"(,"regions":[)" + ids({5, 6, 7, 8}) + "]")));
    BuildResultPtr b = m.build();
    REQUIRE(report(*b, 2).status == FeatureStatus::Ok); // the overhang is a dangling piece of line 5
    REQUIRE(report(*b, 3).status == FeatureStatus::Ok);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(8000.0 - kPi * 9 * 10).epsilon(1e-9));
    const std::map<Face, kernel::SubId> faces0 = classify(bodyOf(*b, 'b').shape);
    REQUIRE(faces0.size() == 7);
    std::map<Face, naming::TopoRef> refs;
    for (const auto& [face, id] : faces0) {
        refs[face] = refTo(*b, 'b', id);
    }
    // Every edge and vertex too: counted, not required (they are named from faces).
    std::vector<naming::TopoRef> others;
    for (const kernel::SubKind kind : {kernel::SubKind::Edge, kernel::SubKind::Vertex}) {
        for (std::uint32_t i = 0; i < bodyOf(*b, 'b').shape.count(kind); ++i) {
            others.push_back(refTo(*b, 'b', kernel::SubId{kind, i}));
        }
    }

    auto expectExact = [&](const std::string& step, const std::set<Face>& touched) {
        b = m.build();
        INFO(step);
        REQUIRE(report(*b, 3).status == FeatureStatus::Ok);
        const std::map<Face, kernel::SubId> now = classify(bodyOf(*b, 'b').shape);
        for (const auto& [face, ref] : refs) {
            const naming::Resolved r = resolveOn(*b, 'b', ref);
            INFO(faceName(face) << ": " << r.reason);
            if (touched.count(face) != 0) {
                continue;
            }
            CHECK(r.state == naming::ResolveState::Exact);
            REQUIRE(r.elements.size() == 1);
            CHECK(r.elements[0] == now.at(face));
        }
        long exact = 0;
        for (const naming::TopoRef& ref : others) {
            exact += resolveOn(*b, 'b', ref).state == naming::ResolveState::Exact ? 1 : 0;
        }
        return exact;
    };

    // 1. A solve: rebuild of the unchanged document.
    CHECK(expectExact("no change", {}) == static_cast<long>(others.size()));

    // 2. A dimension: w 40 -> 46. The solver moves points 2 and 3; every id stays.
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","expression":"46 mm"})"));
    CHECK(expectExact("dimension w", {}) == static_cast<long>(others.size()));
    CHECK(volumeOf(*b, 'b') == Catch::Approx(46.0 * 200 - kPi * 9 * 10).epsilon(1e-9));

    // 3. The hole moves (a hole position with a stable id): hx 20 -> 26.
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 2) + R"(","expression":"26 mm"})"));
    CHECK(expectExact("hole moved", {}) == static_cast<long>(others.size()));

    // 4. An unrelated entity: an open line outside the plate, and a dimension changed by its own command.
    m.run(cmd("sketch.entity.add", R"({"id":")" + uuid('f', 2) + R"(","entities":[)" + R"({"id":")" + ent(30) +
                                       R"(","type":"point","x":60,"y":0},{"id":")" + ent(31) +
                                       R"(","type":"point","x":70,"y":30},{"id":")" + ent(32) +
                                       R"(","type":"line","a":")" + ent(30) + R"(","b":")" + ent(31) + R"("}]})"));
    m.run(cmd("sketch.constraint.set",
              R"({"id":")" + uuid('f', 2) + R"(","constraint":")" + uuid('c', 11) + R"(","expression":"3.5 mm"})"));
    CHECK(expectExact("unrelated line, hole radius", {}) == static_cast<long>(others.size()));

    // 5. Trim the overhang of line 5: the line keeps its id and now starts at point 1.
    m.run(cmd("sketch.trim", R"({"id":")" + uuid('f', 2) + R"(","entity":")" + ent(5) + R"(","at":{"x":-5,"y":0}})"));
    {
        const sketch::Sketch t = m.sketchModel(2);
        CHECK(hasEntity(t, 5));
        CHECK(!hasEntity(t, 20)); // the old end point is gone
        for (const sketch::Entity& e : t.entities) {
            if (e.id.value == Uuid::fromString(ent(5))) {
                CHECK(std::get<sketch::Line>(e.geom).a.value == Uuid::fromString(ent(1)));
            }
        }
    }
    CHECK(expectExact("trim line 5", {}) == static_cast<long>(others.size()));

    // 6. Split the top line at x = 10: line 7 keeps its id (and the part from point 3), a new line has the rest.
    m.run(cmd("sketch.split", R"({"id":")" + uuid('f', 2) + R"(","entity":")" + ent(7) + R"(","at":{"x":10,"y":20}})"));
    {
        const sketch::Sketch t = m.sketchModel(2);
        CHECK(hasEntity(t, 7));
        CHECK(t.entities.size() == 15);
    }
    const long afterSplit = expectExact("split line 7", {Face::Top});
    CHECK(afterSplit > 0);
    // The caps keep their names: the region is picked by lines 5 to 8, which all still bound it.
    // (Checked in expectExact: Floor and Lid are not in the touched set.)
}

TEST_CASE("sketch.trim and sketch.split keep ids and join at existing points", "[doc][parametric][stability]")
{
    Model m;
    // Two vertical lines at x = 10 and x = 20 crossed by a horizontal line from 0 to 30; a circle crossed by it.
    SketchJson s;
    s.point(1, 10, -5).point(2, 10, 5).line(3, 1, 2);
    s.point(4, 20, -5).point(5, 20, 5).line(6, 4, 5);
    s.point(7, 0, 0).point(8, 30, 0).line(9, 7, 8);
    s.circleAt(10, 20, 0, 4);
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", s)));
    SECTION("trimming the middle piece splits the line; it keeps its id and its start")
    {
        m.run(cmd("sketch.trim", R"({"id":")" + uuid('f', 2) + R"(","entity":")" + ent(9) +
                                     R"(","at":{"x":15,"y":0},"newEntity":")" + ent(50) + R"("})"));
        const sketch::Sketch t = m.sketchModel(2);
        const sketch::Entity* line9 = nullptr;
        const sketch::Entity* line50 = nullptr;
        for (const sketch::Entity& e : t.entities) {
            if (e.id.value == Uuid::fromString(ent(9))) {
                line9 = &e;
            }
            if (e.id.value == Uuid::fromString(ent(50))) {
                line50 = &e;
            }
        }
        REQUIRE(line9 != nullptr);
        REQUIRE(line50 != nullptr);
        CHECK(std::get<sketch::Line>(line9->geom).a.value == Uuid::fromString(ent(7)));
        CHECK(std::get<sketch::Line>(line50->geom).b.value == Uuid::fromString(ent(8)));
    }
    SECTION("trimming a circle between two crossings makes an arc with the circle's id")
    {
        m.run(cmd("sketch.trim",
                  R"({"id":")" + uuid('f', 2) + R"(","entity":")" + ent(11) + R"(","at":{"x":22.83,"y":2.83}})"));
        const sketch::Sketch t = m.sketchModel(2);
        bool arc = false;
        for (const sketch::Entity& e : t.entities) {
            if (e.id.value == Uuid::fromString(ent(11))) {
                arc = std::holds_alternative<sketch::Arc>(e.geom);
            }
        }
        CHECK(arc);
    }
    SECTION("splitting at an end is refused")
    {
        const ICommandHandler* split = builtinCommandRegistry()->find("sketch.split");
        auto r = split->apply(m.state,
                              R"({"id":")" + uuid('f', 2) + R"(","entity":")" + ent(9) + R"(","at":{"x":0,"y":0}})");
        REQUIRE(!r);
        CHECK(r.error().message == "sketch.split: 'at' is at an end of the entity; there is nothing to split");
    }
    SECTION("removing a line removes its end points and their constraints")
    {
        m.run(cmd("sketch.constraint.add", R"({"id":")" + uuid('f', 2) + R"(","constraints":[{"id":")" + uuid('c', 1) +
                                               R"(","type":"fix","refs":[")" + ent(1) + R"("]}]})"));
        m.run(cmd("sketch.entity.remove", R"({"id":")" + uuid('f', 2) + R"(","entities":[")" + ent(3) + R"("]})"));
        const sketch::Sketch t = m.sketchModel(2);
        CHECK(!hasEntity(t, 1));
        CHECK(!hasEntity(t, 2));
        CHECK(!hasEntity(t, 3));
        CHECK(t.constraints.empty());
    }
}
