// The sketch commands added for constraints and dimensions (task T-093): dimension label offsets stored in the sketch
// feature's props ("labels"), set by sketch.dimension.place and by sketch.constraint.add with "labels", dropped when
// the dimension goes; and the sketch feature accepting the "labels" props.
#include "parametric_test_util.h"

#include "features/feature_props.h"
#include "features/sketch_feature.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    // The "labels" props of the sketch: {"<id>": {"x", "y"}}, or nullopt.
    std::optional<json::Value> labels() const
    {
        REQUIRE(!state->features.empty());
        auto root = json::parse(state->features.back().propsJson);
        REQUIRE(root);
        const json::Value* l = root->find("labels");
        if (l == nullptr) {
            return std::nullopt;
        }
        return *l;
    }
    sketch::Sketch sketch() const
    {
        auto s = features::sketchOfFeature(state->features.back(), true);
        REQUIRE(s);
        return *s;
    }
};

std::string sk()
{
    return uuid('f', 2);
}

std::string place(std::uint64_t constraint, const std::string& offset)
{
    return R"({"id":")" + sk() + R"(","constraint":")" + uuid('c', constraint) + R"(","offset":)" + offset + "}";
}

double at(const json::Value& labels, std::uint64_t constraint, const char* axis)
{
    const json::Value* l = labels.find(uuid('c', constraint));
    REQUIRE(l != nullptr);
    REQUIRE(l->find(axis) != nullptr);
    return l->find(axis)->asNumber();
}

} // namespace

TEST_CASE("sketch.dimension.place stores a dimension's label offset in the props", "[doc][sketch][dimensions]")
{
    Model m;
    SketchJson s;
    s.rect(1, 0, 0, 40, 20).constraint(1, "distance", {5}, {}, 40.0).constraint(2, "horizontal", {5});
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", s)));
    CHECK(!m.labels());

    m.run(cmd("sketch.dimension.place", place(1, R"({"x":3,"y":-7.5})")));
    REQUIRE(m.labels());
    CHECK(at(*m.labels(), 1, "x") == Approx(3));
    CHECK(at(*m.labels(), 1, "y") == Approx(-7.5));
    // The model is unchanged and the props still pass the type's strict check.
    CHECK(m.sketch().constraints.size() == 2);

    SECTION("moving it again replaces the offset; null forgets it")
    {
        m.run(cmd("sketch.dimension.place", place(1, R"({"x":1,"y":2})")));
        CHECK(at(*m.labels(), 1, "x") == Approx(1));
        m.run(cmd("sketch.dimension.place", place(1, "null")));
        CHECK(!m.labels());
    }
    SECTION("only dimensions have labels; the offset must be a point")
    {
        CHECK(m.refused(cmd("sketch.dimension.place", place(2, R"({"x":1,"y":2})"))).code ==
              ErrorCode::InvalidArgument);
        CHECK(m.refused(cmd("sketch.dimension.place", place(9, R"({"x":1,"y":2})"))).code ==
              ErrorCode::InvalidArgument);
        CHECK(m.refused(cmd("sketch.dimension.place", place(1, R"({"x":1})"))).code == ErrorCode::InvalidArgument);
    }
    SECTION("removing the dimension drops its label; other commands keep the labels of the rest")
    {
        m.run(cmd("sketch.constraint.add", R"({"id":")" + sk() + R"(","constraints":[{"id":")" + uuid('c', 3) +
                                               R"(","type":"distance","refs":[")" + ent(6) +
                                               R"("],"value":20}],"labels":{")" + uuid('c', 3) +
                                               R"(":{"x":-5,"y":0}}})"));
        REQUIRE(m.labels());
        CHECK(at(*m.labels(), 3, "x") == Approx(-5));
        CHECK(at(*m.labels(), 1, "x") == Approx(3));
        m.run(cmd("sketch.constraint.remove", R"({"id":")" + sk() + R"(","constraints":[")" + uuid('c', 1) + R"("]})"));
        REQUIRE(m.labels());
        CHECK(m.labels()->find(uuid('c', 1)) == nullptr);
        CHECK(at(*m.labels(), 3, "x") == Approx(-5));
        // An entity edit (the dimensioned line goes) drops the last one.
        m.run(cmd("sketch.entity.remove", R"({"id":")" + sk() + R"(","entities":[")" + ent(6) + R"("]})"));
        CHECK(!m.labels());
    }
    SECTION("sketch.constraint.add refuses a label for something that is not a new or existing dimension")
    {
        CHECK(m.refused(cmd("sketch.constraint.add", R"({"id":")" + sk() + R"(","constraints":[{"id":")" +
                                                         uuid('c', 4) + R"(","type":"vertical","refs":[")" + ent(6) +
                                                         R"("]}],"labels":{")" + uuid('c', 4) + R"(":{"x":0,"y":0}}})"))
                  .code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("sketch.constraint.set makes a dimension driven and back, and takes an expression",
          "[doc][sketch][dimensions]")
{
    Model m;
    SketchJson s;
    s.rect(1, 0, 0, 40, 20).constraint(1, "distance", {5}, {}, 40.0);
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", s)));
    m.run(cmd("sketch.constraint.set",
              R"({"id":")" + sk() + R"(","constraint":")" + uuid('c', 1) + R"(","driven":true})"));
    CHECK(m.sketch().constraints[0].driven);
    m.run(cmd("sketch.constraint.set",
              R"({"id":")" + sk() + R"(","constraint":")" + uuid('c', 1) + R"(","driven":false,"value":41})"));
    CHECK(!m.sketch().constraints[0].driven);
    CHECK(m.sketch().constraints[0].value == Approx(41));
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","name":"width","expression":"50 mm","kind":"length"})"));
    m.run(cmd("sketch.constraint.set",
              R"({"id":")" + sk() + R"(","constraint":")" + uuid('c', 1) + R"(","expression":"width","value":50})"));
    REQUIRE(m.sketch().constraints[0].expression.has_value());
    CHECK(*m.sketch().constraints[0].expression == "width");
    // An expression naming no parameter is refused by the sketch's own check.
    CHECK(m.refused(cmd("sketch.constraint.set",
                        R"({"id":")" + sk() + R"(","constraint":")" + uuid('c', 1) + R"(","expression":"nosuch * 2"})"))
              .code == ErrorCode::InvalidArgument);
}

TEST_CASE("the constraint commands take the shape the caller shows (\"solved\")", "[doc][sketch][dimensions]")
{
    Model m;
    SketchJson s;
    s.rect(1, 0, 0, 40, 20).constraint(1, "distance", {5}, {}, 40.0);
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", s)));
    // The caller solved the width to 60: the right side moved.
    SketchJson shown;
    shown.rect(1, 0, 0, 60, 20).constraint(1, "distance", {5}, {}, 60.0);
    std::string solved = shown.json();
    m.run(cmd("sketch.constraint.set",
              R"({"id":")" + sk() + R"(","constraint":")" + uuid('c', 1) + R"(","value":60,"solved":)" + solved + "}"));
    const sketch::Sketch t = m.sketch();
    CHECK(t.constraints[0].value == Approx(60));
    for (const sketch::Entity& e : t.entities) {
        if (e.id.value == Uuid::fromString(ent(2))) {
            CHECK(std::get<sketch::Point>(e.geom).p.x == Approx(60));
        }
    }
    SECTION("a solved form with other entities is refused")
    {
        SketchJson other;
        other.rect(1, 0, 0, 60, 20).point(30, 1, 1);
        CHECK(m.refused(cmd("sketch.constraint.remove", R"({"id":")" + sk() + R"(","constraints":[")" + uuid('c', 1) +
                                                            R"("],"solved":)" + other.json() + "}"))
                  .code == ErrorCode::InvalidArgument);
    }
}
