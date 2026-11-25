// The references of a document for the reference review (ray_api/references.h, task T-083): every reference a feature
// or a CAM operation holds, resolved where its owner resolves it; the commands that put new picks in its place and
// confirm a likely match, each one undo step through the owner's own edit action.
#include "api_test_util.h"

#include "json.h"

#include <ray_api/references.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using ray::ErrorCode;
using ray::api::Caller;
using ray::api::OpenDocument;
using ray::test::kWait;
using ray::test::makeTestApp;
namespace json = ray::api::json;
namespace refs = ray::api::references;

namespace {

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

// A 40 x 20 rectangle with a circle of radius 4 at (30, 10): a plate with a through hole once extruded.
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

ray::lib::Tool tool()
{
    ray::lib::Tool t;
    t.id = ray::ToolId{ray::Uuid::fromString(id('d', 1))};
    t.name = "6 mm end mill";
    t.flutes = 3;
    t.diameter = 6;
    t.shankDiameter = 6;
    t.fluteLength = 18;
    t.shoulderLength = 22;
    t.overallLength = 57;
    t.bodyMaterial = "carbide";
    return t;
}

ray::lib::MachineDef machine()
{
    ray::lib::MachineDef m;
    m.id = ray::MachineId{ray::Uuid::fromString(id('9', 1))};
    m.name = "Test mill";
    m.axes = {{'X', 0, 500, 20000, 10000, 0}, {'Y', 0, 400, 20000, 10000, 0}, {'Z', -400, 0, 20000, 10000, 0}};
    m.maxRpm = 12000;
    m.toolCapacity = 20;
    m.controlDialect = "generic-iso";
    m.defaultPost = "generic-iso";
    return m;
}

struct Plate {
    std::unique_ptr<ray::api::HeadlessApp> app;
    OpenDocument* od = nullptr;
    std::string body = id('b', 1);

    Plate()
    {
        app = makeTestApp("");
        invoke(*app, "file.new", "{}");
        invoke(*app, "feature.sketch.add",
               R"({"id":")" + id('f', 1) + R"(","props":{"plane":{"principal":"xy"},"sketch":)" + plateSketch() + "}}");
        invoke(*app, "feature.extrude.add",
               R"({"id":")" + id('f', 2) + R"(","props":{"sketch":")" + id('f', 1) +
                   R"(","distance":"10 mm","operation":"new","target":")" + body + R"("}})");
        od = app->active();
        REQUIRE(od != nullptr);
        REQUIRE(app->waitForBuild(*od, kWait));
        for (const auto& r : od->document->build()->reports) {
            INFO(r.message);
            REQUIRE(r.status == ray::doc::FeatureStatus::Ok);
        }
        REQUIRE(od->document->build()->bodies.size() == 1);
    }
    const ray::kernel::Shape& shape() const { return od->document->build()->bodies.begin()->second.shape; }
    std::uint32_t face(ray::kernel::SurfaceType type, double z = -1) const
    {
        std::optional<std::uint32_t> found;
        for (std::uint32_t i = 0; i < shape().count(ray::kernel::SubKind::Face) && !found; ++i) {
            auto s = ray::kernel::surfaceInfo(shape(), ray::kernel::SubId{ray::kernel::SubKind::Face, i});
            if (s && s->type == type && (z < 0 || std::abs(s->centroid.z - z) < 1e-9)) {
                found = i;
            }
        }
        REQUIRE(found.has_value());
        return found.value_or(0);
    }
    std::string ref(std::uint32_t face) const
    {
        auto made =
            json::parse(invoke(*app, "model.reference",
                               R"({"body":")" + body + R"(","kind":"face","index":)" + std::to_string(face) + "}"));
        REQUIRE(made);
        return json::write(*made->find("ref"));
    }
    refs::Listing list(bool all = false) const
    {
        REQUIRE(app->waitForBuild(*od, kWait));
        ray::JobContext ctx;
        auto listed = refs::list(*app, od->id, od->document->state(), od->document->build(), ctx, all);
        REQUIRE(listed);
        return std::move(*listed);
    }
    void run(const ray::doc::Command& c) const { invoke(*app, c.name, c.argsJson); }
};

} // namespace

TEST_CASE("a reference that records a planar face's outline passes every consumer, and a save and reload",
          "[api][references]")
{
    // T-081b: model.reference records a planar face's pick-time outline (naming.h TopoRef::outline); the shared
    // TopoRef argument schema (doc/features/feature_props.cpp topoRefSchema) must take it wherever a reference goes.
    Plate p;
    const std::string topRef = p.ref(p.face(ray::kernel::SurfaceType::Plane, 10));
    REQUIRE_THAT(topRef, ContainsSubstring("\"outline\":{\"chords\":[["));
    // A feature (push/pull of the top face).
    invoke(*p.app, "feature.offset_face.add",
           R"({"id":")" + id('f', 3) + R"(","props":{"faces":[)" + topRef + R"(],"distance":"1 mm"}})");
    // A setup whose WCS Z comes from the face, and an operation on it.
    invoke(*p.app, "cam.setup.add",
           R"({"id":")" + id('a', 1) + R"(","name":"Setup 1","partBodies":[")" + p.body + R"("],"machine":")" +
               id('9', 1) + R"(","library":{"machines":[)" + ray::lib::toJson(machine()) + "]}}");
    invoke(*p.app, "cam.setup.edit", R"({"id":")" + id('a', 1) + R"(","wcs":{"zFace":)" + topRef + "}}");
    invoke(*p.app, "cam.operation.add",
           R"({"id":")" + id('c', 1) + R"(","setup":")" + id('a', 1) + R"(","type":"no.such.type","name":"Op 1",)"
               R"("tool":{"tool":")" + id('d', 1) + R"("},"cutting":{"rpm":8000,"feedCut":1200,"feedPlunge":300},)"
               R"("heights":{"clearance":25,"retract":5,"feedPlane":2,"top":0,"bottom":-5},"geometry":[)" +
               topRef + R"(],"library":{"tools":[)" + ray::lib::toJson(tool()) + "]}}");
    const auto stored = [&] {
        const refs::Listing all = p.list(true);
        std::size_t withOutline = 0;
        for (const refs::Reference& r : all.references)
            withOutline += r.refJson.find("\"outline\":{\"chords\":[[") != std::string::npos ? 1 : 0;
        return std::make_pair(all.references.size(), withOutline);
    };
    const auto before = stored();
    CHECK(before.first == 3); // the feature's face, the setup's Z face and the operation's geometry
    CHECK(before.second == before.first);
    // Saved and opened again: the references keep their outlines.
    const std::string path = (std::filesystem::temp_directory_path() / "raycad_outline_refs.raycad").string();
    invoke(*p.app, "file.saveAs", R"({"path":)" + json::write(json::Value::string(path)) + "}");
    invoke(*p.app, "file.close", "{}");
    invoke(*p.app, "file.open", R"({"path":)" + json::write(json::Value::string(path)) + "}");
    p.od = p.app->active();
    REQUIRE(p.od != nullptr);
    const auto after = stored();
    CHECK(after == before);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
TEST_CASE("the references of features and CAM operations, resolved where their owners resolve them",
          "[api][references]")
{
    Plate p;
    const std::uint32_t wall = p.face(ray::kernel::SurfaceType::Cylinder);
    const std::uint32_t top = p.face(ray::kernel::SurfaceType::Plane, 10);
    const std::string wallRef = p.ref(wall);
    const std::string topRef = p.ref(top);
    // A CAM operation on the hole wall, and one on the top face (an operation type the app does not know is stored).
    invoke(*p.app, "cam.setup.add",
           R"({"id":")" + id('a', 1) + R"(","name":"Setup 1","partBodies":[")" + p.body + R"("],"machine":")" +
               id('9', 1) + R"(","library":{"machines":[)" + ray::lib::toJson(machine()) + "]}}");
    const auto addOp = [&](int n, const std::string& geometry) {
        invoke(*p.app, "cam.operation.add",
               R"({"id":")" + id('c', n) + R"(","setup":")" + id('a', 1) + R"(","type":"no.such.type","name":"Op )" +
                   std::to_string(n) + R"(","tool":{"tool":")" + id('d', 1) +
                   R"("},"cutting":{"rpm":8000,"feedCut":1200,"feedPlunge":300},)"
                   R"("heights":{"clearance":25,"retract":5,"feedPlane":2,"top":0,"bottom":-5},"geometry":[)" +
                   geometry + R"(],"library":{"tools":[)" + ray::lib::toJson(tool()) + "]}}");
    };
    addOp(1, wallRef);
    addOp(2, topRef);
    // Nothing to review yet; listed all, every reference is Exact, each where its owner resolves it.
    CHECK(p.list().references.empty());
    {
        const refs::Listing all = p.list(true);
        REQUIRE(all.references.size() == 2);
        for (const refs::Reference& r : all.references) {
            CHECK(r.state == ray::naming::ResolveState::Exact);
            CHECK(r.owner == refs::Owner::Operation);
            CHECK(r.inList);
            CHECK_FALSE(r.before.has_value());
            CHECK(r.pickedBox.has_value()); // model.reference records the pick-time evidence
        }
        CHECK(all.references[0].path == "geometry/0");
        CHECK(all.references[0].ownerName == "Op 1");
        CHECK(all.references[0].setupName == "Setup 1");
    }

    // The hole deleted by its wall; then a resize of the same hole, which now has nothing to resize.
    invoke(*p.app, "feature.delete_face.add", R"({"id":")" + id('f', 3) + R"(","props":{"faces":[)" + wallRef + "]}}");
    invoke(*p.app, "feature.resize_hole.add",
           R"({"id":")" + id('f', 4) + R"(","props":{"face":)" + wallRef + R"(,"diameter":"10 mm"}})");
    const refs::Listing listed = p.list();
    // The resize's face (where the resize stands: after the deletion) and operation 1's face are gone; the deletion's
    // own face is Exact where it stands (before it), and the top face is Exact.
    REQUIRE(listed.references.size() == 2);
    const refs::Reference& feature = listed.references[0];
    CHECK(feature.owner == refs::Owner::Feature);
    CHECK(feature.ownerId == id('f', 4));
    CHECK(feature.ownerType == "resize_hole");
    CHECK(feature.path == "face");
    CHECK_FALSE(feature.inList);
    CHECK(feature.state == ray::naming::ResolveState::Broken);
    CHECK(feature.elements.empty());
    REQUIRE(feature.before.has_value());
    CHECK(*feature.before == ray::FeatureId{ray::Uuid::fromString(id('f', 4))});
    CHECK(listed.before.count(*feature.before) == 1);
    CHECK(feature.needsReview());
    CHECK_FALSE(feature.confirmable());
    const refs::Reference& operation = listed.references[1];
    CHECK(operation.owner == refs::Owner::Operation);
    CHECK(operation.ownerId == id('c', 1));
    CHECK(operation.state == ray::naming::ResolveState::Broken);
    CHECK(operation.ownerOrder > feature.ownerOrder); // CAM after the features
    {
        const refs::Listing all = p.list(true);
        CHECK(all.references.size() == 4);
        const auto own = std::find_if(all.references.begin(), all.references.end(),
                                      [&](const refs::Reference& r) { return r.ownerId == id('f', 3); });
        REQUIRE(own != all.references.end());
        CHECK(own->state == ray::naming::ResolveState::Exact);
        CHECK(own->path == "faces/0");
        CHECK(own->inList);
        CHECK(own->elements.size() == 1);
    }

    // Confirming is for a likely match only.
    auto refused = refs::confirmCommand(*p.od->document->state(), feature);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::InvalidArgument);

    // Re-picked: operation 1 gets the top face in place of the wall, through cam.operation.edit (one undo step).
    const std::size_t undo0 = p.od->document->undoCount();
    {
        const std::string topNow = p.ref(p.face(ray::kernel::SurfaceType::Plane, 10));
        auto two = refs::replaceCommand(*p.od->document->state(), operation, std::vector<std::string>{topNow, topNow});
        REQUIRE(two); // the same reference twice goes in once
        CHECK(two->name == "cam.operation.edit");
        auto cmd = refs::replaceCommand(*p.od->document->state(), operation, std::vector<std::string>{topNow});
        REQUIRE(cmd);
        CHECK(cmd->argsJson == two->argsJson);
        CHECK_THAT(cmd->argsJson, ContainsSubstring(R"("geometry":[)"));
        p.run(*cmd);
        CHECK(p.od->document->undoCount() == undo0 + 1);
        const auto& geometry =
            p.od->document->state()->cam->operations.at(ray::OperationId{ray::Uuid::fromString(id('c', 1))}).geometry;
        CHECK(geometry.size() == 1);
    }
    // The resize's face re-picked: a reference that is not in a list takes exactly one new one, through the feature's
    // typed edit naming only that member.
    {
        const std::string topNow = p.ref(p.face(ray::kernel::SurfaceType::Plane, 10));
        auto bad = refs::replaceCommand(*p.od->document->state(), feature, std::vector<std::string>{topNow, topNow});
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == ErrorCode::InvalidArgument);
        auto edge = json::parse(topNow);
        REQUIRE(edge);
        edge->set("kind", json::Value::string("edge"));
        auto wrongKind =
            refs::replaceCommand(*p.od->document->state(), feature, std::vector<std::string>{json::write(*edge)});
        REQUIRE_FALSE(wrongKind);
        auto cmd = refs::replaceCommand(*p.od->document->state(), feature, std::vector<std::string>{topNow});
        REQUIRE(cmd);
        CHECK(cmd->name == "feature.resize_hole.edit");
        CHECK_THAT(cmd->argsJson, ContainsSubstring(R"("props":{"face":)"));
        CHECK_THAT(cmd->argsJson, !ContainsSubstring("diameter"));
        p.run(*cmd);
        CHECK(p.od->document->undoCount() == undo0 + 2);
    }
    // Nothing left to review (the resize of a flat face fails in the kernel, but its reference resolves).
    CHECK(p.list().references.empty());
    // A stale listing's reference no longer matches what the owner holds: refused, nothing changes.
    {
        auto stale = refs::replaceCommand(*p.od->document->state(), operation, std::vector<std::string>{wallRef});
        REQUIRE_FALSE(stale);
        CHECK(stale.error().code == ErrorCode::NotFound);
    }
    // Undo: the operation's wall is back, Broken again.
    invoke(*p.app, "edit.undo", "{}");
    invoke(*p.app, "edit.undo", "{}");
    CHECK(p.list().references.size() == 2);
}

TEST_CASE("a likely match is confirmed through its owner's edit, and a setup's WCS face is re-picked",
          "[api][references]")
{
    Plate p;
    const std::uint32_t top = p.face(ray::kernel::SurfaceType::Plane, 10);
    std::string topRef = p.ref(top);
    // A Repaired reference is one the resolver finds by its evidence alone: the stored names are of no element any
    // more, the fingerprint and the neighbours match one. Stored with names that were never issued, as a reference
    // from before a rename would be.
    auto changed = json::parse(topRef);
    REQUIRE(changed);
    json::Value names = json::Value::array();
    for (std::size_t i = 0; i < changed->find("names")->asArray().size(); ++i) {
        names.push(json::Value::string("0102030405060708091011121314151617181920212223242526272829303132"));
    }
    changed->set("names", names);
    changed->set("families", names); // no family to find it by either: only the fingerprint and the neighbours
    // A datum plane on that face (a feature with a single, nested reference: base/face).
    invoke(*p.app, "feature.datum_plane.add",
           R"({"id":")" + id('f', 3) + R"(","props":{"base":{"face":)" + json::write(*changed) +
               R"(},"offset":"5 mm"}})");
    refs::Listing listed = p.list();
    REQUIRE(listed.references.size() == 1);
    const refs::Reference r = listed.references.front();
    CHECK(r.path == "base/face");
    CHECK_FALSE(r.inList);
    INFO("state " << static_cast<int>(r.state) << ": " << r.reason);
    REQUIRE(r.state == ray::naming::ResolveState::Repaired);
    CHECK(r.confirmable());
    CHECK(r.elements.size() == 1);
    // The datum plane does not build on an unconfirmed match.
    CHECK(p.od->document->build()->reports.back().status == ray::doc::FeatureStatus::Failed);
    {
        auto cmd = refs::confirmCommand(*p.od->document->state(), r);
        REQUIRE(cmd);
        CHECK(cmd->name == "feature.datum_plane.edit");
        CHECK_THAT(cmd->argsJson, ContainsSubstring(R"("repairConfirmed":true)"));
        CHECK_THAT(cmd->argsJson, !ContainsSubstring("offset"));
        const std::size_t undo0 = p.od->document->undoCount();
        p.run(*cmd);
        CHECK(p.od->document->undoCount() == undo0 + 1);
        CHECK(p.list().references.empty()); // the confirmation is the review
        CHECK(p.od->document->build()->reports.back().status != ray::doc::FeatureStatus::Failed);
        // Listed with everything: still Repaired, now confirmed.
        const refs::Listing all = p.list(true);
        const auto datum = std::find_if(all.references.begin(), all.references.end(),
                                        [&](const refs::Reference& x) { return x.ownerId == id('f', 3); });
        REQUIRE(datum != all.references.end());
        CHECK(datum->state == ray::naming::ResolveState::Repaired);
        CHECK(datum->repairConfirmed);
        CHECK_FALSE(datum->needsReview());
    }

    // A setup whose Z face is that broken reference: re-picked through cam.setup.edit, the rest of the WCS kept.
    invoke(*p.app, "cam.setup.add",
           R"({"id":")" + id('a', 1) + R"(","name":"Setup 1","partBodies":[")" + p.body + R"("],"machine":")" +
               id('9', 1) + R"(","wcs":{"zFace":)" + json::write(*changed) +
               R"(,"origin":{"mode":"stockTopCenter"},"workOffset":2},"library":{"machines":[)" +
               ray::lib::toJson(machine()) + "]}}");
    listed = p.list();
    const auto setupRef = std::find_if(listed.references.begin(), listed.references.end(),
                                       [](const refs::Reference& x) { return x.owner == refs::Owner::Setup; });
    REQUIRE(setupRef != listed.references.end());
    CHECK(setupRef->path == "wcs/zFace");
    CHECK(setupRef->setupName == "Setup 1");
    auto cmd = refs::replaceCommand(*p.od->document->state(), *setupRef, std::vector<std::string>{topRef});
    REQUIRE(cmd);
    CHECK(cmd->name == "cam.setup.edit");
    p.run(*cmd);
    const auto& setup = p.od->document->state()->cam->setups.front();
    REQUIRE(setup.wcs.zFace.has_value());
    CHECK(setup.wcs.workOffset == 2);
    CHECK(setup.wcs.originMode == ray::cam::Wcs::OriginMode::StockTopCenter);
    CHECK(p.list().references.empty());
}
