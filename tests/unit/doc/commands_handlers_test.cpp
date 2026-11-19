// The built-in command handlers: what each does, and argument validation (errors name the field).
#include "commands_test_util.h"
#include "json.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using ray::ErrorCode;
using ray::doc::Body;
using ray::doc::builtinCommandRegistry;
using ray::doc::canonicalJson;
using ray::doc::DocStatePtr;
using ray::doc::Parameter;
using ray::doc::test::cmd;
using ray::doc::test::DocFixture;
using ray::doc::test::featureAddArgs;
using ray::doc::test::hashOf;
using ray::doc::test::uuid;

namespace {

std::vector<std::string> featureIds(const DocStatePtr& state)
{
    std::vector<std::string> ids;
    for (const auto& f : state->features) {
        ids.push_back(f.id.value.toString());
    }
    return ids;
}

// The command fails with InvalidArgument, the message names `field`, and nothing changed.
void rejects(DocFixture& f, const std::string& name, const std::string& args, const std::string& field)
{
    INFO(name << " " << args);
    const DocStatePtr before = f.doc->state();
    const auto undo = f.doc->undoCount();
    const auto redo = f.doc->redoCount();
    const auto posted = f.ui.posted();
    const ray::Error error = f.fails(cmd(name, args));
    CHECK(error.code == ErrorCode::InvalidArgument);
    CHECK_THAT(error.message, ContainsSubstring(name));
    CHECK_THAT(error.message, ContainsSubstring(field));
    CHECK(f.doc->state() == before);
    CHECK(f.doc->undoCount() == undo);
    CHECK(f.doc->redoCount() == redo);
    CHECK(f.ui.posted() == posted);
}

} // namespace

TEST_CASE("the built-in registry has the seven model commands and document.import with JSON Schemas", "[doc][commands]")
{
    const auto registry = builtinCommandRegistry();
    REQUIRE(registry != nullptr);
    // The model commands (T-009), document.import (T-010), and the parametric features and sketch editing (T-096).
    CHECK(registry->names() == std::vector<std::string>{"body.set",
                                                        "document.import",
                                                        "feature.add",
                                                        "feature.chamfer.add",
                                                        "feature.chamfer.edit",
                                                        "feature.datum_plane.add",
                                                        "feature.datum_plane.edit",
                                                        "feature.edit",
                                                        "feature.extrude.add",
                                                        "feature.extrude.edit",
                                                        "feature.fillet.add",
                                                        "feature.fillet.edit",
                                                        "feature.hole.add",
                                                        "feature.hole.edit",
                                                        "feature.remove",
                                                        "feature.reorder",
                                                        "feature.revolve.add",
                                                        "feature.revolve.edit",
                                                        "feature.sketch.add",
                                                        "feature.sketch.edit",
                                                        "feature.suppress",
                                                        "param.set",
                                                        "sketch.constraint.add",
                                                        "sketch.constraint.remove",
                                                        "sketch.constraint.set",
                                                        "sketch.construction",
                                                        "sketch.dimension.place",
                                                        "sketch.entity.add",
                                                        "sketch.entity.remove",
                                                        "sketch.extend",
                                                        "sketch.fillet",
                                                        "sketch.offset",
                                                        "sketch.set",
                                                        "sketch.split",
                                                        "sketch.trim"});
    for (const std::string& name : registry->names()) {
        const ray::doc::ICommandHandler* handler = registry->find(name);
        REQUIRE(handler != nullptr);
        CHECK(handler->name() == name);
        CHECK_FALSE(handler->changesToolpathsOrGcode());
        auto schema = ray::doc::json::parse(handler->argsSchemaJson());
        REQUIRE(schema);
        CHECK(schema->isObject());
        CHECK(schema->find("properties") != nullptr);
    }
    CHECK(registry->find("feature.nope") == nullptr);
}

TEST_CASE("CommandRegistry rejects null, unnamed and duplicate handlers", "[doc][commands]")
{
    ray::doc::CommandRegistry registry;
    CHECK(registry.add(nullptr).error().code == ErrorCode::InvalidArgument);
    REQUIRE(ray::doc::registerBuiltinCommands(registry));
    const auto again = ray::doc::registerBuiltinCommands(registry);
    REQUIRE_FALSE(again);
    CHECK_THAT(again.error().message, ContainsSubstring("already registered"));
}

TEST_CASE("feature.add inserts at the end or at an index, with canonical props", "[doc][commands]")
{
    DocFixture f;
    f.run(cmd("feature.add", featureAddArgs(1)));
    f.run(cmd("feature.add", featureAddArgs(2)));
    auto s = f.run(cmd("feature.add", R"({ "index": 0, "suppressed": true, "props": { "z": 1.50, "a": [ 1e2 ] },
                                          "name": "Hole 1", "type": "hole", "id": ")" +
                                          uuid('f', 3) + R"(" })"));
    REQUIRE(s);
    CHECK(featureIds(s) == std::vector<std::string>{uuid('f', 3), uuid('f', 1), uuid('f', 2)});
    const auto& hole = s->features[0];
    CHECK(hole.type == "hole");
    CHECK(hole.name == "Hole 1");
    CHECK(hole.suppressed);
    CHECK(hole.propsJson == R"({"a":[100],"z":1.5})");
    CHECK(hole.extraJson.empty());
    CHECK_FALSE(s->features[1].suppressed);
    CHECK(s->features[1].propsJson == R"({"distance":"1 mm"})");
    s = f.run(cmd("feature.add", featureAddArgs(4, R"(,"index":3)")));
    CHECK(featureIds(s).back() == uuid('f', 4));
}

TEST_CASE("feature.add validates its arguments", "[doc][commands]")
{
    DocFixture f;
    f.run(cmd("feature.add", featureAddArgs(1)));
    const std::string id2 = uuid('f', 2);
    rejects(f, "feature.add", featureAddArgs(1), "'id'"); // duplicate id
    rejects(f, "feature.add", R"({"type":"extrude","name":"E","props":{}})", "'id'");
    rejects(f, "feature.add", R"({"id":"F0000000-0000-4000-8000-000000000002","type":"x","name":"E","props":{}})",
            "'id'");
    rejects(f, "feature.add", R"({"id":"00000000-0000-0000-0000-000000000000","type":"x","name":"E","props":{}})",
            "'id'");
    rejects(f, "feature.add", R"({"id":"not-a-uuid","type":"x","name":"E","props":{}})", "'id'");
    rejects(f, "feature.add", R"({"id":7,"type":"x","name":"E","props":{}})", "'id'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","name":"E","props":{}})", "'type'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","type":"","name":"E","props":{}})", "'type'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","type":"has space","name":"E","props":{}})", "'type'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","type":"x","props":{}})", "'name'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","type":"x","name":"","props":{}})", "'name'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","type":"x","name":"E"})", "'props'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","type":"x","name":"E","props":"{}"})", "'props'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","type":"x","name":"E","props":{},"index":2})", "'index'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","type":"x","name":"E","props":{},"index":-1})", "'index'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","type":"x","name":"E","props":{},"index":0.5})", "'index'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","type":"x","name":"E","props":{},"suppressed":1})",
            "'suppressed'");
    rejects(f, "feature.add", R"({"id":")" + id2 + R"(","type":"x","name":"E","props":{},"colour":1})", "'colour'");
    rejects(f, "feature.add", R"([1,2])", "JSON object");
    rejects(f, "feature.add", R"({"id":)", "JSON");
}

TEST_CASE("feature.edit, feature.remove, feature.reorder and feature.suppress", "[doc][commands]")
{
    DocFixture f;
    for (int i = 1; i <= 4; ++i) {
        f.run(cmd("feature.add", featureAddArgs(static_cast<std::uint64_t>(i))));
    }
    const std::string id1 = uuid('f', 1);
    const std::string id3 = uuid('f', 3);

    auto s = f.run(cmd("feature.edit", R"({"id":")" + id3 + R"(","name":"Renamed"})"));
    CHECK(s->features[2].name == "Renamed");
    CHECK(s->features[2].propsJson == R"({"distance":"3 mm"})");
    s = f.run(cmd("feature.edit", R"({"id":")" + id3 + R"(","props":{"distance":"9 mm", "draft":"1 deg"}})"));
    CHECK(s->features[2].name == "Renamed");
    CHECK(s->features[2].propsJson == R"({"distance":"9 mm","draft":"1 deg"})");

    s = f.run(cmd("feature.reorder", R"({"id":")" + id1 + R"(","index":3})"));
    CHECK(featureIds(s) == std::vector<std::string>{uuid('f', 2), uuid('f', 3), uuid('f', 4), id1});
    s = f.run(cmd("feature.reorder", R"({"id":")" + id1 + R"(","index":0})"));
    CHECK(featureIds(s) == std::vector<std::string>{id1, uuid('f', 2), uuid('f', 3), uuid('f', 4)});
    s = f.run(cmd("feature.reorder", R"({"id":")" + id3 + R"(","index":1})"));
    CHECK(featureIds(s) == std::vector<std::string>{id1, id3, uuid('f', 2), uuid('f', 4)});

    s = f.run(cmd("feature.suppress", R"({"id":")" + id3 + R"(","suppressed":true})"));
    CHECK(s->features[1].suppressed);
    s = f.run(cmd("feature.suppress", R"({"id":")" + id3 + R"(","suppressed":false})"));
    CHECK_FALSE(s->features[1].suppressed);

    s = f.run(cmd("feature.remove", R"({"id":")" + id3 + R"("})"));
    CHECK(featureIds(s) == std::vector<std::string>{id1, uuid('f', 2), uuid('f', 4)});

    const std::string missing = uuid('f', 99);
    rejects(f, "feature.edit", R"({"id":")" + missing + R"(","name":"X"})", "'id'");
    rejects(f, "feature.edit", R"({"id":")" + id1 + R"("})", "'name'");
    rejects(f, "feature.edit", R"({"id":")" + id1 + R"(","name":""})", "'name'");
    rejects(f, "feature.edit", R"({"id":")" + id1 + R"(","props":[]})", "'props'");
    rejects(f, "feature.edit", R"({"id":")" + id1 + R"(","type":"hole"})", "'type'");
    rejects(f, "feature.remove", R"({"id":")" + missing + R"("})", "'id'");
    rejects(f, "feature.remove", R"({})", "'id'");
    rejects(f, "feature.reorder", R"({"id":")" + id1 + R"(","index":3})", "'index'");
    rejects(f, "feature.reorder", R"({"id":")" + id1 + R"("})", "'index'");
    rejects(f, "feature.reorder", R"({"id":")" + missing + R"(","index":0})", "'id'");
    rejects(f, "feature.suppress", R"({"id":")" + id1 + R"("})", "'suppressed'");
    rejects(f, "feature.suppress", R"({"id":")" + id1 + R"(","suppressed":"yes"})", "'suppressed'");
}

TEST_CASE("feature edits keep unknown keys read from a file", "[doc][commands]")
{
    ray::test::TestDispatcher ui;
    auto initial = std::make_shared<ray::doc::DocState>();
    ray::doc::Feature feature;
    feature.id = ray::FeatureId{ray::Uuid::fromString(uuid('f', 1))};
    feature.type = "future_type";
    feature.name = "From the future";
    feature.propsJson = "{}";
    feature.extraJson = R"({"futureKey":[1,2]})";
    initial->features.push_back(feature);
    auto doc = ray::doc::makeDocument(ui, nullptr, initial);
    REQUIRE(doc);
    auto s = doc->execute(cmd("feature.edit", R"({"id":")" + uuid('f', 1) + R"(","name":"Renamed"})"));
    REQUIRE(s);
    CHECK((*s)->features[0].extraJson == R"({"futureKey":[1,2]})");
    CHECK((*s)->features[0].type == "future_type");
}

TEST_CASE("param.set adds and changes parameters and keeps them evaluable", "[doc][commands]")
{
    DocFixture f;
    const std::string w = uuid('a', 1);
    const std::string h = uuid('a', 2);
    auto s = f.run(cmd("param.set", R"({"id":")" + w + R"(","name":"width","expression":"4 in","kind":"length"})"));
    REQUIRE(s->parameters.size() == 1);
    CHECK(s->parameters[0].name == "width");
    CHECK(s->parameters[0].kind == Parameter::Kind::Length);
    s = f.run(cmd("param.set", R"({"id":")" + h + R"(","name":"height","expression":"width / 2","kind":"length"})"));
    s = f.run(cmd("param.set", R"({"id":")" + w + R"(","expression":"100 mm"})"));
    CHECK(s->parameters[0].expression == "100 mm");
    CHECK(s->parameters[0].name == "width");
    auto values = ray::doc::evaluateParameters(s->parameters);
    REQUIRE(values);
    CHECK(values->at("height") == 50.0);

    rejects(f, "param.set", R"({"id":")" + h + R"(","expression":"width * 1 deg"})", "'expression'"); // unit
    rejects(f, "param.set", R"({"id":")" + w + R"(","expression":"height"})", "'expression'");        // cycle
    rejects(f, "param.set", R"({"id":")" + w + R"(","name":"breadth"})", "'name'"); // height refers to width
    rejects(f, "param.set", R"({"id":")" + w + R"(","kind":"angle"})", "'kind'");
    rejects(f, "param.set", R"({"id":")" + h + R"(","name":"width"})", "'name'"); // taken
    rejects(f, "param.set", R"({"id":")" + h + R"(","name":"mm"})", "'name'");    // reserved unit
    rejects(f, "param.set", R"({"id":")" + h + R"(","name":"sqrt"})", "'name'");  // reserved function
    rejects(f, "param.set", R"({"id":")" + h + R"(","name":"2wide"})", "'name'");
    rejects(f, "param.set", R"({"id":")" + h + R"(","kind":"volume"})", "'kind'");
    rejects(f, "param.set", R"({"id":")" + h + R"(","expression":""})", "'expression'");
    rejects(f, "param.set", R"({"id":")" + h + R"("})", "'name'");
    const std::string n = uuid('a', 3);
    rejects(f, "param.set", R"({"id":")" + n + R"(","expression":"1","kind":"count"})", "'name'");
    rejects(f, "param.set", R"({"id":")" + n + R"(","name":"n","kind":"count"})", "'expression'");
    rejects(f, "param.set", R"({"id":")" + n + R"(","name":"n","expression":"1"})", "'kind'");
    rejects(f, "param.set", R"({"id":")" + n + R"(","name":"n","expression":"nope","kind":"count"})", "'expression'");
    rejects(f, "param.set", R"({"name":"n","expression":"1","kind":"count"})", "'id'");
    rejects(f, "param.set", R"({"id":")" + n + R"(","name":"n","expression":"1","kind":"count","unit":"mm"})",
            "'unit'");

    // A rename that nothing refers to is fine.
    s = f.run(cmd("param.set", R"({"id":")" + h + R"(","name":"tall"})"));
    CHECK(s->parameters[1].name == "tall");
}

TEST_CASE("param.set can repair parameters that were already broken", "[doc][commands]")
{
    ray::test::TestDispatcher ui;
    auto initial = std::make_shared<ray::doc::DocState>();
    initial->parameters.push_back(
        {ray::ParamId{ray::Uuid::fromString(uuid('a', 1))}, "a", "b + 1 mm", Parameter::Kind::Length, ""});
    initial->parameters.push_back(
        {ray::ParamId{ray::Uuid::fromString(uuid('a', 2))}, "b", "a", Parameter::Kind::Length, ""});
    auto doc = ray::doc::makeDocument(ui, nullptr, initial);
    REQUIRE(doc);
    REQUIRE_FALSE(ray::doc::evaluateParameters(initial->parameters));
    auto s = doc->execute(cmd("param.set", R"({"id":")" + uuid('a', 2) + R"(","expression":"3 mm"})"));
    REQUIRE(s);
    auto values = ray::doc::evaluateParameters((*s)->parameters);
    REQUIRE(values);
    CHECK(values->at("a") == 4.0);
}

TEST_CASE("body.set adds and changes bodies", "[doc][commands]")
{
    DocFixture f;
    const std::string b = uuid('b', 1);
    auto s = f.run(cmd("body.set", R"({"id":")" + b + R"(","name":"Bracket","role":"part"})"));
    REQUIRE(s->bodies.size() == 1);
    CHECK(s->bodies[0].name == "Bracket");
    CHECK(s->bodies[0].role == Body::Role::Part);
    CHECK(s->bodies[0].visible);
    CHECK(s->bodies[0].color == std::array<float, 4>{0.72f, 0.74f, 0.78f, 1.0f});
    s = f.run(cmd("body.set", R"({"id":")" + b + R"(","color":[0.3,0.30000000000000004,0,1],"visible":false})"));
    CHECK(s->bodies[0].color == std::array<float, 4>{0.3f, 0.3f, 0.0f, 1.0f});
    CHECK_FALSE(s->bodies[0].visible);
    CHECK(s->bodies[0].name == "Bracket");
    s = f.run(cmd("body.set", R"({"id":")" + uuid('b', 2) + R"(","name":"Vise","role":"fixture","visible":true})"));
    CHECK(s->bodies[1].role == Body::Role::Fixture);
    s = f.run(cmd("body.set", R"({"id":")" + b + R"(","role":"stock_model","name":"Stock"})"));
    CHECK(s->bodies[0].role == Body::Role::StockModel);
    CHECK(s->bodies[0].name == "Stock");

    const std::string n = uuid('b', 3);
    rejects(f, "body.set", R"({"id":")" + n + R"(","role":"part"})", "'name'");
    rejects(f, "body.set", R"({"id":")" + n + R"(","name":"X"})", "'role'");
    rejects(f, "body.set", R"({"id":")" + n + R"(","name":"X","role":"tool"})", "'role'");
    rejects(f, "body.set", R"({"id":")" + b + R"("})", "'name'");
    rejects(f, "body.set", R"({"id":")" + b + R"(","color":[1,1,1]})", "'color'");
    rejects(f, "body.set", R"({"id":")" + b + R"(","color":[1,1,1,1.5]})", "'color'");
    rejects(f, "body.set", R"({"id":")" + b + R"(","color":[1,1,"1",1]})", "'color'");
    rejects(f, "body.set", R"({"id":")" + b + R"(","visible":0})", "'visible'");
    rejects(f, "body.set", R"({"id":")" + b + R"(","name":""})", "'name'");
}

TEST_CASE("an unknown command is NotFound and changes nothing", "[doc][commands]")
{
    DocFixture f;
    const auto before = f.doc->state();
    const ray::Error error = f.fails(cmd("feature.explode", "{}"));
    CHECK(error.code == ErrorCode::NotFound);
    CHECK_THAT(error.message, ContainsSubstring("feature.explode"));
    CHECK(f.doc->state() == before);
    CHECK_FALSE(f.doc->canUndo());
}

TEST_CASE("commands canonicalise their arguments before applying and recording them", "[doc][commands]")
{
    DocFixture f;
    const std::string spaced = R"({ "props" : { "b" : 2.50, "a" : "x" }, "name" : "E", "type" : "extrude", "id" : ")" +
                               uuid('f', 1) + R"(" })";
    const auto s = f.run(cmd("feature.add", spaced, "Add E"));
    REQUIRE(s);
    const auto recorded = f.doc->undoCommand();
    REQUIRE(recorded);
    CHECK(recorded->name == "feature.add");
    CHECK(recorded->label == "Add E");
    CHECK(recorded->argsJson == canonicalJson(spaced));

    // The same command spelled canonically gives the same snapshot.
    DocFixture g;
    const auto t = g.run(cmd("feature.add", canonicalJson(spaced)));
    CHECK(hashOf(s) == hashOf(t));
}
