// The parametric feature actions (task T-096): feature.<type>.add/.edit and sketch.* run as document commands,
// model.reference makes the TopoRef JSON a fillet takes, and a part built through actions saves and reopens with the
// same state hash and geometry.
#include "api_test_util.h"

#include "json.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <map>
#include <string>

using Catch::Matchers::ContainsSubstring;
using ray::ErrorCode;
using ray::api::Caller;
using ray::api::OpenDocument;
using ray::test::kWait;
using ray::test::makeTestApp;
using ray::test::TempDir;

namespace {

namespace json = ray::api::json;

// Fixed ids: features f0000000-...-00000000000n, sketch entities e0000000-...
std::string id(char kind, int n)
{
    char tail[13];
    std::snprintf(tail, sizeof tail, "%012x", n);
    return std::string(1, kind) + "0000000-0000-4000-8000-" + tail;
}

std::string invoke(ray::api::HeadlessApp& app, const std::string& action, const std::string& args)
{
    auto done = app.wait(app.actions().invoke(action, args, Caller::User), kWait);
    if (!done) {
        FAIL(action << " failed: " << done.error().message);
    }
    return done->resultJson;
}

// A 40 x 20 rectangle (lines e5..e8) with a circle (e10, centre e9) of radius 4 at (30, 10).
std::string plateSketch()
{
    auto pt = [](int n, double x, double y) {
        return R"({"id":")" + id('e', n) + R"(","type":"point","x":)" + std::to_string(x) + R"(,"y":)" +
               std::to_string(y) + "}";
    };
    auto line = [](int n, int a, int b) {
        return R"({"id":")" + id('e', n) + R"(","type":"line","a":")" + id('e', a) + R"(","b":")" + id('e', b) +
               R"("})";
    };
    return R"({"entities":[)" + pt(1, 0, 0) + "," + pt(2, 40, 0) + "," + pt(3, 40, 20) + "," + pt(4, 0, 20) + "," +
           line(5, 1, 2) + "," + line(6, 2, 3) + "," + line(7, 3, 4) + "," + line(8, 4, 1) + "," + pt(9, 30, 10) +
           R"(,{"id":")" + id('e', 10) + R"(","type":"circle","center":")" + id('e', 9) +
           R"(","radius":4}],"constraints":[]})";
}

double volumeOf(const OpenDocument& od, const std::string& body)
{
    auto build = od.document->build();
    REQUIRE(build);
    const auto it = build->bodies.find(ray::BodyId{ray::Uuid::fromString(body)});
    REQUIRE(it != build->bodies.end());
    auto mp = ray::kernel::massProps(it->second.shape);
    REQUIRE(mp);
    return mp->volume;
}

} // namespace

TEST_CASE("the parametric feature commands are actions with their props schemas", "[api][features]")
{
    auto app = makeTestApp("");
    std::map<std::string, ray::api::ActionInfo> byId;
    for (const auto& info : app->actions().all()) {
        byId[info.id] = info;
    }
    for (const char* type : {"datum_plane", "sketch", "extrude", "revolve", "fillet", "chamfer"}) {
        for (const char* verb : {"add", "edit"}) {
            const std::string name = std::string("feature.") + type + "." + verb;
            INFO(name);
            REQUIRE(byId.count(name) == 1);
            CHECK(byId[name].category == "Model");
            CHECK(byId[name].changesDocument);
            CHECK_FALSE(byId[name].changesToolpathsOrGcode);
            CHECK_THAT(byId[name].argsSchemaJson, ContainsSubstring("\"document\""));
        }
    }
    CHECK_THAT(byId["feature.extrude.add"].argsSchemaJson,
               ContainsSubstring("x-unit") && ContainsSubstring("\"operation\""));
    CHECK(byId["feature.extrude.add"].title == "Extrude");
    for (const char* name : {"sketch.set", "sketch.entity.add", "sketch.entity.remove", "sketch.constraint.add",
                             "sketch.constraint.remove", "sketch.constraint.set", "sketch.split", "sketch.trim"}) {
        INFO(name);
        REQUIRE(byId.count(name) == 1);
        CHECK(byId[name].category == "Sketch");
    }
    REQUIRE(byId.count("model.reference") == 1);
    CHECK_FALSE(byId["model.reference"].changesDocument);
}

TEST_CASE("a part made through actions (sketch, extrude, fillet by model.reference) saves and reopens identically",
          "[api][features]")
{
    TempDir dir;
    const std::string saved = dir.file("plate.raycad");
    const std::string body = id('b', 1);
    std::string stateHash;
    double volume = 0;
    {
        auto app = makeTestApp(dir.file("user"));
        invoke(*app, "file.new", "{}");
        invoke(*app, "feature.sketch.add",
               R"({"id":")" + id('f', 1) + R"(","props":{"plane":{"principal":"xy"},"sketch":)" + plateSketch() + "}}");
        invoke(*app, "feature.extrude.add",
               R"({"id":")" + id('f', 2) + R"(","props":{"sketch":")" + id('f', 1) +
                   R"(","distance":"10 mm","operation":"new","target":")" + body + R"("},"bodyName":"Plate"})");
        OpenDocument* od = app->active();
        REQUIRE(od != nullptr);
        REQUIRE(app->waitForBuild(*od, kWait));
        // The plate with a through hole.
        CHECK(std::abs(volumeOf(*od, body) - (8000.0 - 3.14159265358979 * 16 * 10)) < 1e-6);
        REQUIRE(od->document->state()->bodies.size() == 1);
        CHECK(od->document->state()->bodies[0].name == "Plate");
        // The vertical edge at (0, 0): its index in the latest build, then a reference to it.
        auto build = od->document->build();
        const auto& shape = build->bodies.begin()->second.shape;
        int edge = -1;
        for (std::uint32_t i = 0; i < shape.count(ray::kernel::SubKind::Edge); ++i) {
            auto c = ray::kernel::curveInfo(shape, ray::kernel::SubId{ray::kernel::SubKind::Edge, i});
            if (c && c->type == ray::kernel::CurveType::Line && std::abs(c->start.x) < 1e-9 &&
                std::abs(c->end.x) < 1e-9 && std::abs(c->start.y) < 1e-9 && std::abs(c->end.y) < 1e-9) {
                edge = static_cast<int>(i);
            }
        }
        REQUIRE(edge >= 0);
        const auto reference =
            json::parse(invoke(*app, "model.reference",
                               R"({"body":")" + body + R"(","kind":"edge","index":)" + std::to_string(edge) + "}"));
        REQUIRE(reference);
        const json::Value* ref = reference->find("ref");
        REQUIRE(ref != nullptr);
        invoke(*app, "feature.fillet.add",
               R"({"id":")" + id('f', 3) + R"(","props":{"edges":[)" + json::write(*ref) + R"(],"radius":"2 mm"}})");
        REQUIRE(app->waitForBuild(*od, kWait));
        for (const auto& r : od->document->build()->reports) {
            INFO(r.message);
            CHECK(r.status == ray::doc::FeatureStatus::Ok);
        }
        volume = volumeOf(*od, body);
        CHECK(std::abs(volume - (8000.0 - 3.14159265358979 * 16 * 10 - (1 - 3.14159265358979 / 4) * 4 * 10)) < 1e-6);

        // A dimension change by action, then undo.
        invoke(*app, "feature.extrude.edit", R"({"id":")" + id('f', 2) + R"(","props":{"distance":"12 mm"}})");
        REQUIRE(app->waitForBuild(*od, kWait));
        CHECK(volumeOf(*od, body) > volume);
        invoke(*app, "edit.undo", "{}");
        REQUIRE(app->waitForBuild(*od, kWait));
        CHECK(std::abs(volumeOf(*od, body) - volume) < 1e-9);

        // A bad prop is refused with the field named, and changes nothing.
        auto bad = app->wait(app->actions().invoke("feature.extrude.edit",
                                                   R"({"id":")" + id('f', 2) + R"(","props":{"distance":"deep"}})",
                                                   Caller::User),
                             kWait);
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == ErrorCode::InvalidArgument);
        CHECK_THAT(bad.error().message, ContainsSubstring("'distance'") && ContainsSubstring("unknown name 'deep'"));

        stateHash = od->document->state()->stateHash().hex();
        invoke(*app, "file.saveAs", R"({"path":)" + ray::test::jsonString(saved) + "}");
    }
    {
        auto app = makeTestApp(dir.file("user"));
        auto od = app->wait(app->open(saved), kWait);
        REQUIRE(od);
        REQUIRE(app->waitForBuild(**od, kWait));
        CHECK((*od)->document->state()->stateHash().hex() == stateHash);
        CHECK(std::abs(volumeOf(**od, body) - volume) < 1e-9);
        for (const auto& r : (*od)->document->build()->reports) {
            CHECK(r.status == ray::doc::FeatureStatus::Ok);
        }
    }
}

TEST_CASE("model.reference explains what it cannot reference", "[api][features]")
{
    auto app = makeTestApp("");
    invoke(*app, "file.new", "{}");
    OpenDocument* od = app->active();
    REQUIRE(app->waitForBuild(*od, kWait));
    auto none =
        app->wait(app->actions().invoke("model.reference",
                                        R"({"body":")" + id('b', 1) + R"(","kind":"face","index":0})", Caller::User),
                  kWait);
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::NotFound);
    CHECK_THAT(none.error().message, ContainsSubstring("no body"));
}
