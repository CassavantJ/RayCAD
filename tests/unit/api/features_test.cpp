// The parametric feature actions (task T-096): feature.<type>.add/.edit and sketch.* run as document commands,
// model.reference makes the TopoRef JSON a fillet takes, and a part built through actions saves and reopens with the
// same state hash and geometry.
#include "api_test_util.h"

#include "json.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
    for (const char* type : {"datum_plane", "sketch", "extrude", "revolve", "fillet", "chamfer", "delete_face",
                             "offset_face", "resize_hole"}) {
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
    for (const char* name :
         {"sketch.set", "sketch.entity.add", "sketch.entity.remove", "sketch.constraint.add",
          "sketch.constraint.remove", "sketch.constraint.set", "sketch.split", "sketch.trim", "sketch.construction",
          "sketch.extend", "sketch.offset", "sketch.fillet", "sketch.dimension.place"}) {
        INFO(name);
        REQUIRE(byId.count(name) == 1);
        CHECK(byId[name].category == "Sketch");
    }
    REQUIRE(byId.count("model.reference") == 1);
    CHECK_FALSE(byId["model.reference"].changesDocument);
    REQUIRE(byId.count("model.blendRun") == 1); // T-082
    CHECK_FALSE(byId["model.blendRun"].changesDocument);
    CHECK(byId["feature.offset_face.add"].title == "Push/Pull");
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

        // The model before the fillet (what its edit mode picks in): the edge has the index it had, and the reference
        // made there is the fillet's own. A feature that is not there is NotFound.
        {
            const auto before = json::parse(invoke(*app, "model.reference",
                                                   R"({"body":")" + body + R"(","kind":"edge","index":)" +
                                                       std::to_string(edge) + R"(,"before":")" + id('f', 3) + R"("})"));
            REQUIRE(before);
            const json::Value* again = before->find("ref");
            REQUIRE(again != nullptr);
            CHECK(json::write(*again) == json::write(*ref));
            auto missing = app->wait(app->actions().invoke("model.reference",
                                                           R"({"body":")" + body +
                                                               R"(","kind":"edge","index":0,)"
                                                               R"("before":")" +
                                                               id('f', 9) + R"("})",
                                                           Caller::User),
                                     kWait);
            REQUIRE_FALSE(missing);
            CHECK(missing.error().code == ErrorCode::NotFound);
        }

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

TEST_CASE("model.blendRun gives the faces of a blend run, which one delete_face removes", "[api][features]")
{
    // T-082: a 30 x 20 x 10 block with its four top edges rounded (radius 2) by one fillet: the rounds are one run.
    auto app = makeTestApp("");
    invoke(*app, "file.new", "{}");
    const std::string body = id('b', 1);
    const std::string block =
        R"({"entities":[{"id":")" + id('e', 1) + R"(","type":"point","x":0,"y":0},{"id":")" + id('e', 2) +
        R"(","type":"point","x":30,"y":0},{"id":")" + id('e', 3) + R"(","type":"point","x":30,"y":20},{"id":")" +
        id('e', 4) + R"(","type":"point","x":0,"y":20},{"id":")" + id('e', 5) + R"(","type":"line","a":")" +
        id('e', 1) + R"(","b":")" + id('e', 2) + R"("},{"id":")" + id('e', 6) + R"(","type":"line","a":")" +
        id('e', 2) + R"(","b":")" + id('e', 3) + R"("},{"id":")" + id('e', 7) + R"(","type":"line","a":")" +
        id('e', 3) + R"(","b":")" + id('e', 4) + R"("},{"id":")" + id('e', 8) + R"(","type":"line","a":")" +
        id('e', 4) + R"(","b":")" + id('e', 1) + R"("}],"constraints":[]})";
    invoke(*app, "feature.sketch.add",
           R"({"id":")" + id('f', 1) + R"(","props":{"plane":{"principal":"xy"},"sketch":)" + block + "}}");
    invoke(*app, "feature.extrude.add",
           R"({"id":")" + id('f', 2) + R"(","props":{"sketch":")" + id('f', 1) +
               R"(","distance":"10 mm","operation":"new","target":")" + body + R"("}})");
    OpenDocument* od = app->active();
    REQUIRE(od != nullptr);
    REQUIRE(app->waitForBuild(*od, kWait));
    const auto refOf = [&](const char* kind, std::uint32_t index) {
        auto made = json::parse(
            invoke(*app, "model.reference",
                   R"({"body":")" + body + R"(","kind":")" + kind + R"(","index":)" + std::to_string(index) + "}"));
        REQUIRE(made);
        const json::Value* ref = made->find("ref");
        REQUIRE(ref != nullptr);
        return json::write(*ref);
    };
    {
        const auto& shape = od->document->build()->bodies.begin()->second.shape;
        std::string edges;
        for (std::uint32_t i = 0; i < shape.count(ray::kernel::SubKind::Edge); ++i) {
            auto c = ray::kernel::curveInfo(shape, ray::kernel::SubId{ray::kernel::SubKind::Edge, i});
            if (c && std::abs(c->start.z - 10) < 1e-9 && std::abs(c->end.z - 10) < 1e-9) {
                edges += (edges.empty() ? "" : ",") + refOf("edge", i);
            }
        }
        invoke(*app, "feature.fillet.add",
               R"({"id":")" + id('f', 3) + R"(","props":{"edges":[)" + edges + R"(],"radius":"2 mm"}})");
        REQUIRE(app->waitForBuild(*od, kWait));
    }
    const auto& rounded = od->document->build()->bodies.begin()->second.shape;
    std::optional<std::uint32_t> round, flat;
    for (std::uint32_t i = 0; i < rounded.count(ray::kernel::SubKind::Face); ++i) {
        auto s = ray::kernel::surfaceInfo(rounded, ray::kernel::SubId{ray::kernel::SubKind::Face, i});
        if (s && s->type == ray::kernel::SurfaceType::Cylinder && !round) {
            round = i;
        }
        if (s && s->type == ray::kernel::SurfaceType::Plane && !flat) {
            flat = i;
        }
    }
    REQUIRE(round);
    REQUIRE(flat);
    const auto run = [&](std::uint32_t index) {
        auto answer = json::parse(
            invoke(*app, "model.blendRun", R"({"body":")" + body + R"(","index":)" + std::to_string(index) + "}"));
        REQUIRE(answer);
        const json::Value* faces = answer->find("faces");
        REQUIRE(faces != nullptr);
        REQUIRE(faces->isArray());
        std::vector<std::uint32_t> out;
        for (const json::Value& f : faces->asArray()) {
            out.push_back(static_cast<std::uint32_t>(f.asNumber()));
        }
        return out;
    };
    const std::vector<std::uint32_t> faces = run(*round);
    CHECK(faces.size() >= 4);
    CHECK(std::find(faces.begin(), faces.end(), *round) != faces.end());
    CHECK(std::is_sorted(faces.begin(), faces.end()));
    CHECK(run(*flat).empty()); // not a blend
    // The model before the fillet has no blend; a face it does not have is refused.
    {
        auto before = json::parse(
            invoke(*app, "model.blendRun", R"({"body":")" + body + R"(","index":0,"before":")" + id('f', 3) + R"("})"));
        REQUIRE(before);
        CHECK(before->find("faces")->asArray().empty());
        auto bad = app->wait(
            app->actions().invoke("model.blendRun", R"({"body":")" + body + R"(","index":999})", Caller::User), kWait);
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == ErrorCode::InvalidArgument);
    }
    // The whole run, referenced face by face, comes out with one delete_face: the block is square again.
    std::string refs;
    for (const std::uint32_t f : faces) {
        refs += (refs.empty() ? "" : ",") + refOf("face", f);
    }
    invoke(*app, "feature.delete_face.add", R"({"id":")" + id('f', 4) + R"(","props":{"faces":[)" + refs + "]}}");
    REQUIRE(app->waitForBuild(*od, kWait));
    for (const auto& r : od->document->build()->reports) {
        INFO(r.message);
        CHECK(r.status == ray::doc::FeatureStatus::Ok);
    }
    CHECK(std::abs(volumeOf(*od, body) - 6000.0) < 1e-6);
}

namespace {

// A tangent chain (a meander of lines and semicircles, every joint tangent, lengths and radii dimensioned, the first
// line fixed) whose solve is long: `periods` x 10 entities. Constraint c15 is the second line's length; changing it
// moves everything after it.
std::string meanderSketch(int periods)
{
    constexpr double L = 30, R = 2;
    std::vector<std::string> entities, constraints;
    int e = 10, c = 10;
    const auto pt = [&](int n, double x, double y) {
        entities.push_back(R"({"id":")" + id('e', n) + R"(","type":"point","x":)" + std::to_string(x) + R"(,"y":)" +
                           std::to_string(y) + "}");
    };
    const auto line = [&](int n, int a, int b) {
        entities.push_back(R"({"id":")" + id('e', n) + R"(","type":"line","a":")" + id('e', a) + R"(","b":")" +
                           id('e', b) + R"("})");
    };
    const auto arc = [&](int n, int center, int a, int b, bool ccw) {
        entities.push_back(R"({"id":")" + id('e', n) + R"(","type":"arc","center":")" + id('e', center) +
                           R"(","start":")" + id('e', a) + R"(","end":")" + id('e', b) + R"(","ccw":)" +
                           (ccw ? "true" : "false") + "}");
    };
    const auto con = [&](const char* type, std::vector<int> refs, double value = -1) {
        std::string r;
        for (int ref : refs) {
            r += (r.empty() ? "\"" : ",\"") + id('e', ref) + "\"";
        }
        constraints.push_back(R"({"id":")" + id('c', c++) + R"(","type":")" + type + R"(","refs":[)" + r + "]" +
                              (value >= 0 ? R"(,"value":)" + std::to_string(value) : std::string()) + "}");
    };
    int prevEnd = e++;
    pt(prevEnd, 0, 0);
    con("fix", {prevEnd});
    int prevCurve = 0;
    for (int k = 0; k < periods; ++k) {
        const double y0 = 4 * R * k;
        const int p1 = e++, cA = e++, p2 = e++, p3 = e++, cB = e++, p4 = e++;
        const int lA = e++, aA = e++, lB = e++, aB = e++;
        pt(p1, L, y0);
        pt(cA, L, y0 + R);
        pt(p2, L, y0 + 2 * R);
        pt(p3, 0, y0 + 2 * R);
        pt(cB, 0, y0 + 3 * R);
        pt(p4, 0, y0 + 4 * R);
        line(lA, prevEnd, p1);
        arc(aA, cA, p1, p2, true);
        line(lB, p2, p3);
        arc(aB, cB, p3, p4, false);
        if (k == 0) {
            con("fix", {p1});
        } else {
            con("tangent", {prevCurve, lA, prevEnd});
            con("distance", {lA}, L);
        }
        con("tangent", {lA, aA, p1});
        con("radius", {aA}, R);
        con("tangent", {aA, lB, p2});
        con("distance", {lB}, L);
        con("tangent", {lB, aB, p3});
        con("radius", {aB}, R);
        prevEnd = p4;
        prevCurve = aB;
    }
    std::string out = R"({"entities":[)";
    for (std::size_t i = 0; i < entities.size(); ++i) {
        out += (i ? "," : "") + entities[i];
    }
    out += R"(],"constraints":[)";
    for (std::size_t i = 0; i < constraints.size(); ++i) {
        out += (i ? "," : "") + constraints[i];
    }
    return out + "]}";
}

std::string widthArgs(double value)
{
    return R"({"id":")" + id('f', 1) + R"(","constraint":")" + id('c', 15) + R"(","value":)" + std::to_string(value) +
           "}";
}

// Drives the UI queue until `job` is done; returns the longest time the UI thread spent in one turn of the queue.
template <class T>
double longestUiTurn(ray::api::HeadlessApp& app, ray::JobHandle<T>& job)
{
    double longest = 0;
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (!job.done() && std::chrono::steady_clock::now() < deadline) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::size_t ran = app.runPending();
        longest =
            std::max(longest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        if (ran == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return longest;
}

} // namespace

TEST_CASE("a sketch command's solve runs on a worker; commands keep their order; cancel changes nothing",
          "[api][features][sketch-threading]")
{
    auto app = makeTestApp("");
    invoke(*app, "file.new", "{}");
    OpenDocument* od = app->active();
    REQUIRE(od != nullptr);
    // Grow the sketch until one edit's solve takes 150 ms or more on this build and machine.
    int periods = 4;
    double solveMs = 0;
    for (;;) {
        invoke(*app, "feature.sketch.add",
               R"({"id":")" + id('f', 1) + R"(","props":{"plane":{"principal":"xy"},"sketch":)" +
                   meanderSketch(periods) + "}}");
        const auto t0 = std::chrono::steady_clock::now();
        auto job = app->actions().invoke("sketch.constraint.set", widthArgs(28), Caller::User);
        const double ui = longestUiTurn(*app, job);
        solveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(job.wait());
        INFO(periods << " periods: the edit took " << solveMs << " ms, the longest UI turn " << ui << " ms");
        if (solveMs >= 150 || periods >= 64) {
            // The solve did not run in a UI turn (before, IDocument::execute solved inside one).
            CHECK(ui < solveMs / 2);
            break;
        }
        periods *= 2;
        invoke(*app, "feature.remove", R"({"id":")" + id('f', 1) + R"("})");
    }

    SECTION("a command invoked after a slow one waits for it and applies on its result")
    {
        // The first adds a point 9 mm from the fixed start (a full solve); the second fixes that point: run before the
        // first committed, it would fail.
        auto first = app->actions().invoke("sketch.entity.add",
                                           R"({"id":")" + id('f', 1) + R"(","entities":[{"id":")" + id('e', 900001) +
                                               R"(","type":"point","x":5,"y":-5}],"constraints":[{"id":")" +
                                               id('c', 900001) + R"(","type":"distance","refs":[")" + id('e', 10) +
                                               R"(",")" + id('e', 900001) + R"("],"value":9}]})",
                                           Caller::User);
        auto second =
            app->actions().invoke("sketch.constraint.add",
                                  R"({"id":")" + id('f', 1) + R"(","constraints":[{"id":")" + id('c', 900002) +
                                      R"(","type":"fix","refs":[")" + id('e', 900001) + R"("]}]})",
                                  Caller::User);
        auto r1 = app->wait(first, kWait);
        auto r2 = app->wait(second, kWait);
        INFO((r1 ? std::string("first ok") : "first: " + r1.error().message));
        INFO((r2 ? std::string("second ok") : "second: " + r2.error().message));
        REQUIRE(r1);
        REQUIRE(r2);
        REQUIRE(od->document->undoCommand());
        CHECK(od->document->undoCommand()->name == "sketch.constraint.add");
        invoke(*app, "edit.undo", "{}");
        REQUIRE(od->document->undoCommand());
        CHECK(od->document->undoCommand()->name == "sketch.entity.add");
    }
    SECTION("a command cancelled while it solves changes nothing")
    {
        const std::string before = od->document->state()->stateHash().hex();
        const std::size_t undos = od->document->undoCount();
        auto job = app->actions().invoke("sketch.constraint.set", widthArgs(31), Caller::User);
        const auto t0 = std::chrono::steady_clock::now();
        (void)app->runUntil(
            [&] {
                return std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(static_cast<int>(solveMs / 4));
            },
            kWait);
        const bool took = job.cancel();
        auto r = app->wait(job, kWait);
        // Whichever way the race went, the job's answer and the document agree (cancel_commit_test.cpp places the
        // cancel exactly): Cancelled with nothing changed, or the edit committed and reported, the cancel too late.
        if (r) {
            CHECK_FALSE(took);
            CHECK(od->document->state()->stateHash().hex() != before);
            CHECK(od->document->undoCount() == undos + 1);
        } else {
            CHECK(took);
            CHECK(r.error().code == ErrorCode::Cancelled);
            CHECK(od->document->state()->stateHash().hex() == before);
            CHECK(od->document->undoCount() == undos);
        }
    }
}

namespace {

// Puts the meander sketch (feature f1) into the active document, grown until one width edit takes `minMs` or more on
// this build and machine; returns how long that edit took.
double slowSketch(ray::api::HeadlessApp& app, double minMs)
{
    for (int periods = 4;; periods *= 2) {
        invoke(app, "feature.sketch.add",
               R"({"id":")" + id('f', 1) + R"(","props":{"plane":{"principal":"xy"},"sketch":)" +
                   meanderSketch(periods) + "}}");
        const auto t0 = std::chrono::steady_clock::now();
        invoke(app, "sketch.constraint.set", widthArgs(28));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms >= minMs || periods >= 64) {
            return ms;
        }
        invoke(app, "feature.remove", R"({"id":")" + id('f', 1) + R"("})");
    }
}

// Returns once every worker of the app's two is free: two jobs that each wait until both have started. A worker can
// still be busy with an earlier rebuild after waitForBuild (one that was superseded, and cancelled, while it built a
// feature: it ends at the feature's next cancel check); a test that counts on a free worker must not start before.
// The time it waited, in ms.
double bothWorkersFree(ray::api::HeadlessApp& app)
{
    const auto t0 = std::chrono::steady_clock::now();
    auto started = std::make_shared<std::atomic<int>>(0);
    const auto meet = [&] {
        return app.jobs().submit<int>(ray::JobInfo{"both workers", ray::JobPriority::Interactive, false},
                                      [started](ray::JobContext&) -> ray::Result<int> {
                                          ++*started;
                                          const auto deadline = std::chrono::steady_clock::now() + kWait;
                                          while (started->load() < 2 && std::chrono::steady_clock::now() < deadline) {
                                              std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                          }
                                          return started->load();
                                      });
    };
    auto a = meet();
    auto b = meet();
    auto ra = app.wait(a, kWait);
    auto rb = app.wait(b, kWait);
    REQUIRE(ra);
    REQUIRE(rb);
    REQUIRE(*ra == 2);
    REQUIRE(*rb == 2);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Waits for `job` without running any UI work on this thread (the UI thread of the headless app): what needs the UI
// thread waits meanwhile. True if it finished within `timeout`.
template <class T>
bool doneWithoutUi(const ray::JobHandle<T>& job, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!job.done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return job.done();
}

} // namespace

TEST_CASE("document commands queue per document: order, cancel of a queued one, one worker per queue",
          "[api][features][command-queue]")
{
    auto app = makeTestApp("", 2); // two workers: a queue that held more than one would starve everything else
    invoke(*app, "file.new", "{}");
    OpenDocument* od = app->active();
    REQUIRE(od != nullptr);
    const double editMs = slowSketch(*app, 300);
    INFO("one width edit takes " << editMs << " ms");
    REQUIRE(app->waitForBuild(*od, kWait));
    const std::size_t undos = od->document->undoCount();
    INFO("both workers were free after " << bothWorkersFree(*app) << " ms");

    SECTION("commands run in invoke order; a queued one cancelled ends at once and never runs")
    {
        auto a = app->actions().invoke("sketch.constraint.set", widthArgs(29), Caller::User);
        auto b = app->actions().invoke("sketch.constraint.set", widthArgs(30), Caller::User);
        auto c = app->actions().invoke("sketch.constraint.set", widthArgs(31), Caller::User);
        auto d =
            app->actions().invoke("feature.edit", R"({"id":")" + id('f', 1) + R"(","name":"Meander"})", Caller::User);
        // b waits behind a (a command ends on the UI thread, which has not run since): its cancel completes it now,
        // without a worker or a UI turn.
        CHECK_FALSE(a.done());
        CHECK(b.cancel());
        CHECK(b.done());
        auto rb = b.wait();
        REQUIRE_FALSE(rb);
        CHECK(rb.error().code == ErrorCode::Cancelled);
        auto ra = app->wait(a, kWait);
        auto rc = app->wait(c, kWait);
        auto rd = app->wait(d, kWait);
        REQUIRE(ra);
        REQUIRE(rc);
        REQUIRE(rd);
        // Three undo steps (b never ran), the last one d's, applied on c's result.
        CHECK(od->document->undoCount() == undos + 3);
        REQUIRE(od->document->undoCommand());
        CHECK(od->document->undoCommand()->name == "feature.edit");
        invoke(*app, "edit.undo", "{}");
        REQUIRE(od->document->undoCommand());
        CHECK(od->document->undoCommand()->name == "sketch.constraint.set");
        CHECK_THAT(od->document->undoCommand()->argsJson, ContainsSubstring("31"));
    }
    SECTION("a long queue holds one worker: other jobs run while it drains")
    {
        constexpr int kQueued = 6;
        std::vector<ray::JobHandle<ray::api::ActionResult>> queued;
        for (int i = 0; i < kQueued; ++i) {
            queued.push_back(app->actions().invoke("sketch.constraint.set", widthArgs(29 + i), Caller::User));
        }
        // A job submitted after the whole queue: one worker is the queue's, the other is free for it. Waited for
        // without UI turns, so the queue cannot drain meanwhile (each command ends on the UI thread) and the order is
        // forced: the job either gets the second worker while all six wait, or never runs.
        std::atomic<int> doneWhenItRan{-1};
        auto other = app->jobs().submit<int>(ray::JobInfo{"other", ray::JobPriority::Interactive, false},
                                             [&](ray::JobContext&) -> ray::Result<int> {
                                                 int done = 0;
                                                 for (auto& q : queued) {
                                                     done += q.done() ? 1 : 0;
                                                 }
                                                 doneWhenItRan = done;
                                                 return done;
                                             });
        // Before, every queued command took a worker to wait for the one before it: the other job never ran here.
        CHECK(doneWithoutUi(other, kWait));
        REQUIRE(app->wait(other, kWait));
        for (auto& q : queued) {
            REQUIRE(app->wait(q, kWait));
        }
        INFO("queued commands done when the other job ran: " << doneWhenItRan.load() << " of " << kQueued);
        CHECK(doneWhenItRan.load() == 0);
        CHECK(od->document->undoCount() == undos + kQueued);
    }
    SECTION("the queues of two documents run side by side")
    {
        invoke(*app, "file.new", "{}");
        OpenDocument* second = app->active();
        REQUIRE(second != nullptr);
        REQUIRE(second != od);
        REQUIRE(bothWorkersFree(*app) >= 0); // the new document's first rebuild has its worker back
        // The slow edit on the first document, by id; a quick one on the active (second) one, which is not queued
        // behind it.
        auto slow = app->actions().invoke(
            "sketch.constraint.set", R"({"document":")" + od->id + R"(",)" + widthArgs(33).substr(1), Caller::User);
        auto quick = app->actions().invoke(
            "param.set", R"({"id":")" + id('a', 1) + R"(","name":"w","expression":"5 mm","kind":"length"})",
            Caller::User);
        auto rq = app->wait(quick, kWait);
        REQUIRE(rq);
        CHECK_FALSE(slow.done());
        CHECK(second->document->undoCount() == 1);
        REQUIRE(app->wait(slow, kWait));
        CHECK(od->document->undoCount() == undos + 1);
    }
}
