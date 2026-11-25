// Replacing an imported part with a new revision of its file (task T-084): the action file.replaceImport and the
// command feature.import.replace through the app, with what depends on the part: direct edits downstream of the import,
// CAM operations on its faces, the reference review's listing, undo and redo, save and reopen, and the refusals.
#include "api_test_util.h"

#include "json.h"

#include <ray_api/references.h>
#include <ray_api/replace_import.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using ray::api::Caller;
using ray::test::kWait;
namespace json = ray::api::json;
namespace refs = ray::api::references;

namespace {

using namespace ray;
using kernel::SubId;
using kernel::SubKind;

std::string uuid(char kind, int n)
{
    std::string tail = std::to_string(n);
    return std::string(1, kind) + "0000000-0000-4000-8000-" + std::string(12 - tail.size(), '0') + tail;
}

// "test.replace": one rapid and one feed move, so an operation can be up to date.
class TestGenerator final : public cam::IOperationType {
public:
    std::string_view typeName() const override { return "test.replace"; }
    std::string_view paramsSchemaJson() const override { return R"({"type":"object"})"; }
    Status validate(const cam::GenerateInput&) const override { return {}; }
    Result<cam::Toolpath> generate(const cam::GenerateInput& in, JobContext&) const override
    {
        cam::Toolpath tp{};
        cam::Move rapid{};
        rapid.kind = cam::MoveKind::Rapid;
        rapid.role = cam::MoveRole::Approach;
        rapid.z = toNm(in.op.heights.clearance);
        cam::Move feed{};
        feed.kind = cam::MoveKind::Linear;
        feed.role = cam::MoveRole::Cut;
        feed.z = toNm(in.op.heights.bottom);
        feed.feedMicronPerMin = static_cast<std::int32_t>(in.op.cutting.feedCut * 1000.0);
        tp.moves = {rapid, feed};
        return tp;
    }
};

lib::Tool tool()
{
    lib::Tool t;
    t.id = ToolId{Uuid::fromString(uuid('d', 1))};
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

lib::MachineDef machine()
{
    lib::MachineDef m;
    m.id = MachineId{Uuid::fromString(uuid('9', 1))};
    m.name = "Test mill";
    m.axes = {{'X', 0, 500, 20000, 10000, 0}, {'Y', 0, 400, 20000, 10000, 0}, {'Z', -400, 0, 20000, 10000, 0}};
    m.maxRpm = 12000;
    m.toolCapacity = 20;
    m.controlDialect = "generic-iso";
    m.defaultPost = "generic-iso";
    return m;
}

template <class T>
T take(Result<T> r)
{
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r.has_value());
    return std::move(*r);
}

std::string invoke(api::HeadlessApp& app, const std::string& action, const std::string& args)
{
    auto done = app.wait(app.actions().invoke(action, args, Caller::User), kWait);
    if (!done) {
        FAIL(action << " failed: " << done.error().message);
    }
    return done->resultJson;
}

Error invokeFails(api::HeadlessApp& app, const std::string& action, const std::string& args)
{
    auto done = app.wait(app.actions().invoke(action, args, Caller::User), kWait);
    if (done) {
        FAIL(action << " should have failed");
    }
    return done.error();
}

// A 60 x 40 x 20 block with a 6 mm hole at (15, 20) and an 8 mm hole at (45, 20); `added`: a 4 mm hole at (30, 32);
// `withoutBig`: no 8 mm hole.
kernel::Shape block(bool added, bool withoutBig)
{
    JobContext ctx;
    kernel::Shape s = take(kernel::makeBox({0, 0, 0}, {60, 40, 20}));
    auto cut = [&](Vec3 at, double r) {
        s = take(kernel::boolean(kernel::BoolOp::Cut, s, take(kernel::makeCylinder(at, {0, 0, 1}, r, 22)), ctx)).shape;
    };
    cut({15, 20, -1}, 3);
    if (!withoutBig)
        cut({45, 20, -1}, 4);
    if (added)
        cut({30, 32, -1}, 2);
    return s;
}

void writeStep(const std::string& path, const std::vector<kernel::Shape>& shapes)
{
    JobContext ctx;
    REQUIRE(kernel::exportStep(path, shapes, ctx).has_value());
}

std::unique_ptr<api::HeadlessApp> makeApp()
{
    auto app = ray::test::makeTestApp("");
    REQUIRE(app->operationTypes() != nullptr);
    app->operationTypes()->add(std::make_unique<TestGenerator>());
    return app;
}

struct Part {
    std::unique_ptr<api::HeadlessApp> app = makeApp();
    ray::test::TempDir dir;
    std::string feature, body;
    api::OpenDocument* od = nullptr;

    explicit Part(const std::string& baseFile)
    {
        invoke(*app, "file.new", "{}");
        invoke(*app, "file.import", R"({"path":)" + ray::test::jsonString(baseFile) + "}");
        od = app->active();
        REQUIRE(app->waitForBuild(*od, kWait));
        const doc::DocStatePtr state = od->document->state();
        REQUIRE(state->features.size() == 1);
        feature = state->features.front().id.value.toString();
        REQUIRE(state->bodies.size() == 1);
        body = state->bodies.front().id.value.toString();
    }
    const kernel::Shape& shape() const
    {
        REQUIRE(app->waitForBuild(*od, kWait));
        return od->document->build()->bodies.begin()->second.shape;
    }
    // A face of the build by what it is.
    std::uint32_t face(const std::function<bool(const kernel::SurfaceInfo&)>& is) const
    {
        const kernel::Shape& s = shape();
        for (std::uint32_t i = 0; i < s.count(SubKind::Face); ++i)
            if (const auto info = kernel::surfaceInfo(s, {SubKind::Face, i}); info && is(*info))
                return i;
        FAIL_CHECK("no such face");
        return 0;
    }
    std::uint32_t hole(double x, double y) const
    {
        return face([&](const kernel::SurfaceInfo& s) {
            return s.type == kernel::SurfaceType::Cylinder && std::abs(s.centroid.x - x) < 1e-3 &&
                   std::abs(s.centroid.y - y) < 1e-3;
        });
    }
    std::uint32_t plane(int axis, double at) const
    {
        return face([&](const kernel::SurfaceInfo& s) {
            const double c = axis == 0 ? s.centroid.x : axis == 1 ? s.centroid.y : s.centroid.z;
            const double n = axis == 0 ? s.axis.x : axis == 1 ? s.axis.y : s.axis.z;
            return s.type == kernel::SurfaceType::Plane && std::abs(c - at) < 1e-6 && std::abs(std::abs(n) - 1) < 1e-9;
        });
    }
    std::string ref(std::uint32_t face) const
    {
        auto made =
            json::parse(invoke(*app, "model.reference",
                               R"({"body":")" + body + R"(","kind":"face","index":)" + std::to_string(face) + "}"));
        REQUIRE(made);
        return json::write(*made->find("ref"));
    }
    std::string replace(const std::string& path) const
    {
        return invoke(*app, ray::api::replace_import::kAction,
                      R"({"feature":")" + feature + R"(","path":)" + ray::test::jsonString(path) + "}");
    }
    cam::OpStatus status(int op) const
    {
        REQUIRE(app->waitForBuild(*od, kWait));
        const auto all = app->cam()->status(od->document->state(), od->document->build());
        const auto it = all.find(OperationId{Uuid::fromString(uuid('c', op))});
        REQUIRE(it != all.end());
        return it->second.status;
    }
    void generate(int op) const
    {
        REQUIRE(app->waitForBuild(*od, kWait));
        auto job = app->cam()->generate(od->document->state(), od->document->build(),
                                        OperationId{Uuid::fromString(uuid('c', op))});
        take(app->wait(job, kWait));
        REQUIRE(app->runUntil([&] { return status(op) == cam::OpStatus::UpToDate; }, kWait));
    }
    void addOperation(int op, const std::string& geometry) const
    {
        if (!od->document->state()->cam)
            invoke(*app, "cam.setup.add",
                   R"({"id":")" + uuid('a', 1) + R"(","name":"Setup 1","partBodies":[")" + body + R"("],"machine":")" +
                       uuid('9', 1) + R"(","library":{"machines":[)" + lib::toJson(machine()) + "]}}");
        invoke(*app, "cam.operation.add",
               R"({"id":")" + uuid('c', op) + R"(","setup":")" + uuid('a', 1) +
                   R"(","type":"test.replace","name":"Op )" + std::to_string(op) + R"(","tool":{"tool":")" +
                   uuid('d', 1) +
                   R"("},"cutting":{"rpm":8000,"feedCut":1200,"feedPlunge":300},)"
                   R"("heights":{"clearance":25,"retract":5,"feedPlane":2,"top":0,"bottom":-5},"geometry":[)" +
                   geometry + R"(],"library":{"tools":[)" + lib::toJson(tool()) + "]}}");
    }
    std::map<std::string, naming::ResolveState> states() const
    {
        REQUIRE(app->waitForBuild(*od, kWait));
        JobContext ctx;
        auto listed = take(refs::list(*app, od->id, od->document->state(), od->document->build(), ctx, true));
        std::map<std::string, naming::ResolveState> out;
        for (const refs::Reference& r : listed.references)
            out[r.ownerId + "/" + r.path] = r.state;
        return out;
    }
    std::vector<doc::FeatureStatus> reports() const
    {
        REQUIRE(app->waitForBuild(*od, kWait));
        std::vector<doc::FeatureStatus> out;
        for (const auto& r : od->document->build()->reports) {
            INFO(r.message);
            out.push_back(r.status);
        }
        return out;
    }
};

int count(const json::Value& summary, const char* kind, const char* fate)
{
    return static_cast<int>(summary.find("counts")->find(kind)->find(fate)->asNumber());
}

} // namespace

TEST_CASE(
    "replacing an imported part: unchanged faces keep their references, CAM and downstream edits follow, one undo",
    "[api][replace-import]")
{
    ray::test::TempDir files;
    const std::string base = files.file("bracket rev A.stp"), revB = files.file("bracket rev B.stp"),
                      revC = files.file("bracket rev C.stp");
    writeStep(base, {block(false, false)});
    writeStep(revB, {block(true, false)}); // a hole added
    writeStep(revC, {block(false, true)}); // the 8 mm hole removed
    Part p(base);

    // Downstream of the import: the 6 mm hole resized, the end face pulled out 2 mm.
    invoke(*p.app, "feature.resize_hole.add",
           R"({"id":")" + uuid('f', 2) + R"(","props":{"face":)" + p.ref(p.hole(15, 20)) + R"(,"diameter":"7 mm"}})");
    invoke(*p.app, "feature.offset_face.add",
           R"({"id":")" + uuid('f', 3) + R"(","props":{"faces":[)" + p.ref(p.plane(0, 60)) +
               R"(],"distance":"2 mm"}})");
    for (const doc::FeatureStatus s : p.reports())
        REQUIRE(s == doc::FeatureStatus::Ok);
    // CAM on the result: the front face (unchanged by every revision), the top face (rev B drills into it) and the
    // 8 mm hole (rev C removes it).
    p.addOperation(1, p.ref(p.plane(1, 0)));
    p.addOperation(2, p.ref(p.plane(2, 20)));
    p.addOperation(3, p.ref(p.hole(45, 20)));
    for (int op = 1; op <= 3; ++op)
        p.generate(op);
    const auto before = p.states();
    for (const auto& [where, state] : before) {
        INFO(where);
        CHECK(state == naming::ResolveState::Exact);
    }
    const Hash256 stateBefore = p.od->document->state()->stateHash();
    const Hash256 shapeBefore = p.shape().contentHash();

    // Rev B: a hole added.
    const auto summary = take(json::parse(p.replace(revB)));
    CHECK(count(summary, "faces", "changed") == 2); // the top and bottom faces, which the new hole runs through
    CHECK(count(summary, "faces", "new") == 1);     // its wall
    CHECK(count(summary, "faces", "removed") == 0);
    CHECK(count(summary, "faces", "unchanged") == 6); // four sides and the two hole walls
    CHECK(summary.find("toolpaths")->asNumber() == 3);
    CHECK(summary.find("newFile")->asString() == "bracket rev B.stp");
    CHECK(summary.find("label")->asString() == "Replace bracket rev A.stp with bracket rev B.stp");
    {
        const auto& bodies = summary.find("bodies")->asArray();
        REQUIRE(bodies.size() == 1);
        int changedRows = 0, newRows = 0;
        for (const json::Value& f : bodies.front().find("faces")->asArray()) {
            changedRows += f.find("fate")->asString() == "changed";
            newRows += f.find("fate")->asString() == "new";
            if (f.find("fate")->asString() == "new") {
                CHECK(f.find("surface")->asString() == "cylinder");
                CHECK(f.find("hole")->asBool());
                CHECK(std::abs(f.find("radius")->asNumber() - 2) < 1e-6);
            }
        }
        CHECK(changedRows == 2);
        CHECK(newRows == 1);
    }
    // One step: the import keeps its id and bodies; the downstream edits build through their references.
    CHECK(p.od->document->undoCommand()->name == "feature.import.replace");
    CHECK(p.od->document->state()->features.size() == 3);
    for (const doc::FeatureStatus s : p.reports())
        CHECK(s == doc::FeatureStatus::Ok);
    CHECK(p.shape().contentHash() != shapeBefore);
    {
        const auto after = p.states();
        CHECK(after.at(uuid('c', 1) + "/geometry/0") == naming::ResolveState::Exact);
        CHECK(after.at(uuid('c', 2) + "/geometry/0") == naming::ResolveState::Merged); // the top face changed: review
        CHECK(after.at(uuid('c', 3) + "/geometry/0") == naming::ResolveState::Exact);
        CHECK(after.at(uuid('f', 2) + "/face") == naming::ResolveState::Exact);
        CHECK(after.at(uuid('f', 3) + "/faces/0") == naming::ResolveState::Exact);
    }
    // The part changed: every operation of its setup is out of date; the one on the changed face lost its reference.
    CHECK(p.status(1) == cam::OpStatus::Stale);
    CHECK(p.status(2) == cam::OpStatus::ReferenceBroken);
    CHECK(p.status(3) == cam::OpStatus::Stale);
    // The new names were stored with the document.
    const std::string props = p.od->document->state()->features.front().propsJson;
    CHECK_THAT(props, ContainsSubstring("\"names\":\""));
    CHECK_THAT(props, ContainsSubstring("bracket rev B.stp"));
    // The rows can be found in the build (the new hole's wall, through its names).
    {
        const auto& f = summary.find("bodies")->asArray().front().find("faces")->asArray();
        for (const json::Value& row : f) {
            std::vector<Hash256> names;
            for (const json::Value& n : row.find("names")->asArray()) {
                Hash256 h;
                const std::string hex = n.asString();
                for (std::size_t i = 0; i < 32; ++i)
                    h.bytes[i] = static_cast<std::uint8_t>(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
                names.push_back(h);
            }
            CHECK(ray::api::replace_import::locate(*p.od->document->build(), BodyId{Uuid::fromString(p.body)},
                                                   SubKind::Face, names)
                      .has_value());
        }
    }

    // Undo: the old geometry and every reference as it was; the old toolpaths are up to date again.
    invoke(*p.app, "edit.undo", "{}");
    CHECK(p.od->document->state()->stateHash() == stateBefore);
    CHECK(p.shape().contentHash() == shapeBefore);
    CHECK(p.states() == before);
    for (int op = 1; op <= 3; ++op)
        CHECK(p.status(op) == cam::OpStatus::UpToDate);
    // Redo, then save and reopen: the carried names come back from the file, so the references resolve the same.
    invoke(*p.app, "edit.redo", "{}");
    const auto redone = p.states();
    CHECK(redone.at(uuid('c', 2) + "/geometry/0") == naming::ResolveState::Merged);
    const std::string saved = files.file("bracket.raycad");
    invoke(*p.app, "file.saveAs", R"({"path":)" + ray::test::jsonString(saved) + "}");
    invoke(*p.app, "file.close", "{}");
    {
        auto again = makeApp();
        invoke(*again, "file.open", R"({"path":)" + ray::test::jsonString(saved) + "}");
        api::OpenDocument* od = again->active();
        REQUIRE(again->waitForBuild(*od, kWait));
        for (const auto& r : od->document->build()->reports) {
            INFO(r.message);
            CHECK(r.status == doc::FeatureStatus::Ok);
            CHECK(r.message.find("named afresh") == std::string::npos);
        }
        JobContext ctx;
        auto listed = take(refs::list(*again, od->id, od->document->state(), od->document->build(), ctx, true));
        std::map<std::string, naming::ResolveState> reopened;
        for (const refs::Reference& r : listed.references)
            reopened[r.ownerId + "/" + r.path] = r.state;
        CHECK(reopened == redone);
    }

    // Rev C from rev B: the 8 mm hole is gone, and the operation on it lost its face.
    // Here the 6 mm hole is deleted downstream of the import (a direct edit through its reference).
    Part q(base);
    q.addOperation(3, q.ref(q.hole(45, 20)));
    q.addOperation(1, q.ref(q.plane(1, 0)));
    q.generate(3);
    q.generate(1);
    invoke(*q.app, "feature.delete_face.add",
           R"({"id":")" + uuid('f', 4) + R"(","props":{"faces":[)" + q.ref(q.hole(15, 20)) + "]}}");
    q.generate(1);
    q.generate(3);
    const auto c = take(json::parse(q.replace(revC)));
    CHECK(count(c, "faces", "removed") == 1);
    CHECK(count(c, "faces", "new") == 0);
    for (const doc::FeatureStatus s : q.reports())
        CHECK(s == doc::FeatureStatus::Ok); // the deletion still finds its hole
    CHECK(q.states().at(uuid('f', 4) + "/faces/0") == naming::ResolveState::Exact);
    CHECK(q.states().at(uuid('c', 3) + "/geometry/0") == naming::ResolveState::Broken);
    CHECK(q.status(3) == cam::OpStatus::ReferenceBroken);
    CHECK(q.status(1) == cam::OpStatus::Stale);
}

TEST_CASE("a replacement is refused, and nothing changes, when it cannot stand for the part", "[api][replace-import]")
{
    ray::test::TempDir files;
    const std::string base = files.file("part.step");
    writeStep(base, {block(false, false)});
    Part p(base);
    const Hash256 stateBefore = p.od->document->state()->stateHash();
    const std::size_t undoBefore = p.od->document->undoCount();
    auto unchanged = [&] {
        CHECK(p.od->document->state()->stateHash() == stateBefore);
        CHECK(p.od->document->undoCount() == undoBefore);
    };
    auto refused = [&](const std::string& path) {
        return invokeFails(*p.app, ray::api::replace_import::kAction,
                           R"({"feature":")" + p.feature + R"(","path":)" + ray::test::jsonString(path) + "}");
    };

    // Two bodies where the part has one.
    const std::string two = files.file("two bodies.step");
    writeStep(two, {block(true, false), take(kernel::makeBox({100, 0, 0}, {110, 10, 10}))});
    const Error twoBodies = refused(two);
    CHECK_THAT(twoBodies.message, ContainsSubstring("holds 2 bodies and the part has 1"));
    unchanged();
    // The very file it came from.
    CHECK_THAT(refused(base).message, ContainsSubstring("very file the part was imported from"));
    unchanged();
    // A file that is not STEP at all, and one that does not exist.
    CHECK(refused(ray::test::corpusFile("raycad-synthetic/broken_html_page.stp")).code != ErrorCode::Cancelled);
    unchanged();
    CHECK(refused(files.file("missing.step")).code == ErrorCode::NotFound);
    unchanged();
    CHECK_THAT(refused(files.file("notes.txt")).message, ContainsSubstring("is not a STEP"));
    unchanged();
    // Surfaces only: the block written as an open shell with its first face left out (a surface model, as some
    // systems send), which no healing can close.
    const std::string open = files.file("open shell.step");
    {
        const std::string solid = files.file("solid.step");
        writeStep(solid, {block(true, false)});
        std::ifstream in(ray::test::fsPath(solid), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto swap = [&](const std::string& from, const std::string& to) {
            const std::size_t at = text.find(from);
            REQUIRE(at != std::string::npos);
            text.replace(at, from.size(), to);
        };
        const std::size_t shell = text.find("CLOSED_SHELL('',(");
        REQUIRE(shell != std::string::npos);
        const std::size_t first = shell + std::string("CLOSED_SHELL('',(").size();
        text.erase(first, text.find(',', first) + 1 - first); // the first face
        swap("CLOSED_SHELL", "OPEN_SHELL");
        const std::size_t brep = text.find("MANIFOLD_SOLID_BREP('',");
        REQUIRE(brep != std::string::npos);
        const std::size_t ref = brep + std::string("MANIFOLD_SOLID_BREP('',").size();
        const std::size_t end = text.find(')', ref);
        text.replace(brep, end + 1 - brep, "SHELL_BASED_SURFACE_MODEL('',(" + text.substr(ref, end - ref) + "))");
        swap("ADVANCED_BREP_SHAPE_REPRESENTATION", "MANIFOLD_SURFACE_SHAPE_REPRESENTATION");
        ray::test::writeBytes(open, text);
    }
    const Error surfaces = refused(open);
    INFO(surfaces.message);
    CHECK((surfaces.message.find("not a solid") != std::string::npos ||
           surfaces.message.find("holds no geometry") != std::string::npos));
    unchanged();

    // The command itself checks what it is given.
    const std::string props = p.od->document->state()->features.front().propsJson;
    CHECK_THAT(
        invokeFails(*p.app, "feature.import.replace", R"({"id":")" + uuid('f', 9) + R"(","props":)" + props + "}")
            .message,
        ContainsSubstring("names no feature"));
    std::string otherBody = props;
    const std::size_t at = otherBody.find(p.body);
    REQUIRE(at != std::string::npos);
    otherBody.replace(at, p.body.size(), uuid('b', 7));
    CHECK_THAT(
        invokeFails(*p.app, "feature.import.replace", R"({"id":")" + p.feature + R"(","props":)" + otherBody + "}")
            .message,
        ContainsSubstring("must keep the import's bodies"));
    unchanged();
}

TEST_CASE("several bodies: paired by where they lie, whatever their order in the file; refused when unclear",
          "[api][replace-import]")
{
    ray::test::TempDir files;
    const kernel::Shape lug = take(kernel::makeBox({100, 0, 0}, {120, 10, 10}));
    const std::string base = files.file("two parts.step");
    writeStep(base, {block(false, false), lug});
    auto app = makeApp();
    invoke(*app, "file.new", "{}");
    invoke(*app, "file.import", R"({"path":)" + ray::test::jsonString(base) + "}");
    api::OpenDocument* od = app->active();
    REQUIRE(app->waitForBuild(*od, kWait));
    const doc::DocStatePtr state = od->document->state();
    REQUIRE(state->bodies.size() == 2);
    const std::string feature = state->features.front().id.value.toString();
    auto boxOf = [&](const BodyId& b) {
        REQUIRE(app->waitForBuild(*od, kWait));
        return kernel::bounds(od->document->build()->bodies.at(b).shape);
    };
    const BodyId first = state->bodies[0].id, second = state->bodies[1].id;
    const bool blockFirst = boxOf(first).max.x < 90;
    const BodyId blockBody = blockFirst ? first : second, lugBody = blockFirst ? second : first;

    // The revision lists the lug first, then the block with a hole added: each new body goes to the old one it lies on.
    const std::string rev = files.file("two parts rev B.step");
    writeStep(rev, {lug, block(true, false)});
    const auto summary =
        take(json::parse(invoke(*app, ray::api::replace_import::kAction,
                                R"({"feature":")" + feature + R"(","path":)" + ray::test::jsonString(rev) + "}")));
    CHECK(count(summary, "faces", "new") == 1);
    CHECK(count(summary, "faces", "changed") == 2);
    CHECK(boxOf(blockBody).max.x < 90);
    CHECK(boxOf(lugBody).min.x > 90);
    for (const json::Value& b : summary.find("bodies")->asArray()) {
        const bool isLug = b.find("body")->asString() == lugBody.value.toString();
        CHECK(b.find("faces")->asArray().size() == (isLug ? 0u : 3u)); // the lug is unchanged
    }

    // Two new bodies that overlap both old ones alike: which replaces which cannot be told.
    const std::string unclear = files.file("two parts unclear.step");
    writeStep(unclear,
              {take(kernel::makeBox({0, 0, 0}, {120, 40, 20})), take(kernel::makeBox({0, 0, 0}, {120, 40, 20.5}))});
    const Hash256 before = od->document->state()->stateHash();
    const Error e = invokeFails(*app, ray::api::replace_import::kAction,
                                R"({"feature":")" + feature + R"(","path":)" + ray::test::jsonString(unclear) + "}");
    CHECK_THAT(e.message, ContainsSubstring("cannot be told from where they lie"));
    CHECK(od->document->state()->stateHash() == before);
}

TEST_CASE("a revision in other units is compared in millimetres; the summary says the units changed",
          "[api][replace-import]")
{
    // A NIST part written in inches (exported by a commercial CAD system), replaced by the same part written by
    // RayCAD's exporter in millimetres: nothing changed, and every face keeps its references.
    ray::test::TempDir files;
    const std::string base = ray::test::corpusFile("nist-pmi/nist_ctc_03_asme1_rc_geometry.stp");
    Part p(base);
    const std::string metric = files.file("ctc_03 metric.stp");
    writeStep(metric, {p.shape()});
    const auto top = p.ref(p.face([](const kernel::SurfaceInfo& s) { return s.type == kernel::SurfaceType::Plane; }));
    p.addOperation(1, top);
    const auto summary = take(json::parse(p.replace(metric)));
    CHECK(summary.find("oldUnits")->asString() == "inch");
    CHECK(summary.find("newUnits")->asString() == "metric");
    CHECK(count(summary, "faces", "changed") == 0);
    CHECK(count(summary, "faces", "new") == 0);
    CHECK(count(summary, "faces", "removed") == 0);
    CHECK(p.states().at(uuid('c', 1) + "/geometry/0") == naming::ResolveState::Exact);
}
