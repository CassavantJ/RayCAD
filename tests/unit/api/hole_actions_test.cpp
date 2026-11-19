// The hole feature's actions (task T-097): feature.hole.add / .edit with their props schema, standards.threads.list,
// and a tapped counterbored part made through actions that saves and reopens with the same state hash.
#include "api_test_util.h"

#include "json.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstdio>
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

TEST_CASE("the hole commands and the thread table are actions", "[api][hole]")
{
    auto app = makeTestApp("");
    std::map<std::string, ray::api::ActionInfo> byId;
    for (const auto& info : app->actions().all()) {
        byId[info.id] = info;
    }
    for (const char* name : {"feature.hole.add", "feature.hole.edit"}) {
        INFO(name);
        REQUIRE(byId.count(name) == 1);
        CHECK(byId[name].category == "Model");
        CHECK(byId[name].changesDocument);
    }
    CHECK(byId["feature.hole.add"].title == "Hole");
    CHECK_THAT(byId["feature.hole.add"].argsSchemaJson, ContainsSubstring("x-unit") &&
                                                            ContainsSubstring("\"countersink_tapped\"") &&
                                                            ContainsSubstring("\"tapDrill\""));
    REQUIRE(byId.count("standards.threads.list") == 1);
    CHECK_FALSE(byId["standards.threads.list"].changesDocument);

    auto all = json::parse(invoke(*app, "standards.threads.list", "{}"));
    REQUIRE(all);
    const json::Value* series = all->find("series");
    REQUIRE(series != nullptr);
    REQUIRE(series->asArray().size() == 4);
    auto unc = json::parse(invoke(*app, "standards.threads.list", R"({"series":"unc"})"));
    REQUIRE(unc);
    const json::Value& sizes = *unc->find("series")->asArray()[0].find("sizes");
    bool found = false;
    for (const json::Value& s : sizes.asArray()) {
        if (s.find("designation")->asString() == "1/4-20 UNC") {
            found = true;
            CHECK(s.find("tapDrill")->find("size")->asString() == "#7");
            CHECK(std::abs(s.find("tapDrill")->find("diameter")->asNumber() - 5.1054) < 1e-9);
        }
    }
    CHECK(found);
    auto bad = app->wait(app->actions().invoke("standards.threads.list", R"({"series":"acme"})", Caller::User), kWait);
    REQUIRE_FALSE(bad);
}

TEST_CASE("a tapped counterbored part made through actions saves and reopens identically", "[api][hole]")
{
    TempDir dir;
    const std::string saved = dir.file("holes.raycad");
    const std::string body = id('b', 1);
    std::string stateHash;
    double volume = 0;
    {
        auto app = makeTestApp(dir.file("user"));
        invoke(*app, "file.new", "{}");
        invoke(*app, "feature.sketch.add",
               R"({"id":")" + id('f', 1) + R"(","props":{"plane":{"principal":"xy"},"sketch":{"entities":[)" +
                   R"({"id":")" + id('e', 1) + R"(","type":"point","x":0,"y":0},{"id":")" + id('e', 2) +
                   R"(","type":"point","x":40,"y":0},{"id":")" + id('e', 3) + R"(","type":"point","x":40,"y":20},)" +
                   R"({"id":")" + id('e', 4) + R"(","type":"point","x":0,"y":20},{"id":")" + id('e', 5) +
                   R"(","type":"line","a":")" + id('e', 1) + R"(","b":")" + id('e', 2) + R"("},{"id":")" + id('e', 6) +
                   R"(","type":"line","a":")" + id('e', 2) + R"(","b":")" + id('e', 3) + R"("},{"id":")" + id('e', 7) +
                   R"(","type":"line","a":")" + id('e', 3) + R"(","b":")" + id('e', 4) + R"("},{"id":")" + id('e', 8) +
                   R"(","type":"line","a":")" + id('e', 4) + R"(","b":")" + id('e', 1) + R"("}],"constraints":[]}}})");
        invoke(*app, "feature.extrude.add",
               R"({"id":")" + id('f', 2) + R"(","props":{"sketch":")" + id('f', 1) +
                   R"(","distance":"12 mm","operation":"new","target":")" + body + R"("}})");
        invoke(*app, "feature.datum_plane.add",
               R"({"id":")" + id('f', 3) + R"(","props":{"base":{"principal":"xy"},"offset":"12 mm"}})");
        invoke(*app, "feature.sketch.add",
               R"({"id":")" + id('f', 4) + R"(","props":{"plane":{"datum":")" + id('f', 3) +
                   R"("},"sketch":{"entities":[{"id":")" + id('e', 11) + R"(","type":"point","x":10,"y":10},)" +
                   R"({"id":")" + id('e', 12) + R"(","type":"point","x":30,"y":10}],"constraints":[]}}})");
        invoke(*app, "feature.hole.add",
               R"({"id":")" + id('f', 5) + R"(","props":{"sketch":")" + id('f', 4) + R"(","target":")" + body +
                   R"(","kind":"counterbore_tapped","end":"blind","depth":"10 mm","counterbore":{"diameter":"11 mm",)"
                   R"("depth":"4 mm"},"thread":{"designation":"M6x1","standard":"iso_metric_coarse",)"
                   R"("nominalDiameter":"6 mm","pitch":"1 mm","class":"6H","depth":"5 mm","tapDrill":"5 mm"}}})");
        OpenDocument* od = app->active();
        REQUIRE(od != nullptr);
        REQUIRE(app->waitForBuild(*od, kWait));
        for (const auto& r : od->document->build()->reports) {
            INFO(r.message);
            CHECK(r.status == ray::doc::FeatureStatus::Ok);
        }
        const double pi = 3.14159265358979323846;
        const double one =
            pi * 5.5 * 5.5 * 4 + pi * 2.5 * 2.5 * 6 + pi * 2.5 * 2.5 * (2.5 / std::tan(59 * pi / 180)) / 3;
        volume = volumeOf(*od, body);
        CHECK(std::abs(volume - (40 * 20 * 12 - 2 * one)) < 1e-6);
        // A bad prop is refused with the field named, and changes nothing.
        auto bad =
            app->wait(app->actions().invoke("feature.hole.edit",
                                            R"({"id":")" + id('f', 5) + R"(","props":{"depth":"deep"}})", Caller::User),
                      kWait);
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == ErrorCode::InvalidArgument);
        CHECK_THAT(bad.error().message, ContainsSubstring("Hole 1: 'depth'") && ContainsSubstring("unknown name"));
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
