// The direct-edit feature types (task T-082): delete_face, offset_face and resize_hole through the typed commands and
// the real rebuilder. A later feature's references survive an unrelated direct edit and break when their face is
// deleted; editing a direct edit replays the features after it; results are cached like any feature's; expressions
// drive the sizes; kernel warnings and refusals reach the feature unchanged and a refusal leaves the body as it was;
// every resolve state is handled as the fillet handles it (a resolver double gives each state); imported bodies (the
// S9 plate whose hole walls come in halves) are edited, undone, redone, saved and reloaded; previews; cancel. The CAM
// side (an operation's references through a direct edit) is in tests/unit/cam/engine_directedit_test.cpp.
#include "parametric_test_util.h"

#include "features/delete_face_feature.h"
#include "features/offset_face_feature.h"
#include "features/resize_hole_feature.h"

#include <directedit_testing.h> // src/kernel: the S9 cases of a corpus part, for the cancel test

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <thread>

using namespace ray;
using namespace ray::doc;
using namespace ray::doc::test;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
namespace fx = ray::doc::features;

namespace {

struct Model {
    explicit Model(int workers = 4) : env(workers) {}
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
    Error fails(const Command& c)
    {
        const ICommandHandler* handler = builtinCommandRegistry()->find(c.name);
        REQUIRE(handler != nullptr);
        auto next = handler->apply(state, c.argsJson);
        if (next) {
            FAIL(c.name << " should have failed");
        }
        return next.error();
    }
    BuildResultPtr build()
    {
        BuildResultPtr b = env.rebuild(state);
        for (const FeatureReport& r : b->reports) {
            if (r.status != FeatureStatus::Ok) {
                UNSCOPED_INFO("feature " << r.id.value.toString() << ": " << r.message);
            }
        }
        return b;
    }
};

const std::string kXY = R"({"principal":"xy"})";

// The plate: sketch 2, a 60 x 30 rectangle with a circle of radius 3 at (15, 15) (hole A) and one of radius 4 at
// (45, 15) (hole B); extrude 3, 10 mm, new body 'b'.
constexpr double kPlate = 60.0 * 30.0 * 10.0;
double hole(double r)
{
    return kPi * r * r * 10.0;
}
void plate(Model& m)
{
    m.run(addFeature(
        "sketch", 2,
        sketchProps(kXY, SketchJson().rect(1, 0, 0, 60, 30).circleAt(10, 15, 15, 3).circleAt(12, 45, 15, 4))));
    m.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
}

std::optional<kernel::SubId> cylinder(const kernel::Shape& shape, double r)
{
    return findFace(shape, [&](const kernel::SurfaceInfo& s) {
        return s.type == kernel::SurfaceType::Cylinder && near(s.radius, r, 1e-6);
    });
}

int cylinders(const kernel::Shape& shape, double r)
{
    int n = 0;
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
        auto s = kernel::surfaceInfo(shape, kernel::SubId{kernel::SubKind::Face, i});
        n += s && s->type == kernel::SurfaceType::Cylinder && near(s->radius, r, 1e-6) ? 1 : 0;
    }
    return n;
}

// The planar face of `shape` whose outward normal is the axis `n` and whose centroid lies at `at` along it.
std::optional<kernel::SubId> planeOf(const kernel::Shape& shape, Vec3 n, double at)
{
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId sub{kernel::SubKind::Face, i};
        auto s = kernel::faceSample(shape, sub);
        auto info = kernel::surfaceInfo(shape, sub);
        if (!s || !info || info->type != kernel::SurfaceType::Plane) {
            continue;
        }
        const double along = info->centroid.x * n.x + info->centroid.y * n.y + info->centroid.z * n.z;
        if (near(s->normal.x, n.x, 1e-9) && near(s->normal.y, n.y, 1e-9) && near(s->normal.z, n.z, 1e-9) &&
            near(along, at, 1e-6)) {
            return sub;
        }
    }
    return std::nullopt;
}

kernel::SubId planeFace(const BuildResult& build, char body, Vec3 n, double at)
{
    auto face = planeOf(bodyOf(build, body).shape, n, at);
    REQUIRE(face);
    return *face;
}

kernel::SubId cylinderFace(const BuildResult& build, char body, double r)
{
    auto face = cylinder(bodyOf(build, body).shape, r);
    REQUIRE(face);
    return *face;
}

std::string facesJson(std::initializer_list<naming::TopoRef> refs)
{
    std::string out = "[";
    for (const naming::TopoRef& r : refs) {
        out += (out.size() > 1 ? "," : "") + refJson(r);
    }
    return out + "]";
}

std::string deleteProps(std::initializer_list<naming::TopoRef> refs)
{
    return R"({"faces":)" + facesJson(refs) + "}";
}
std::string offsetProps(std::initializer_list<naming::TopoRef> refs, const std::string& distance)
{
    return R"({"faces":)" + facesJson(refs) + R"(,"distance":)" + distance + "}";
}
std::string resizeProps(const naming::TopoRef& ref, const std::string& diameter)
{
    return R"({"face":)" + refJson(ref) + R"(,"diameter":)" + diameter + "}";
}

std::string kernelMessage(const Result<kernel::OpResult>& r)
{
    REQUIRE_FALSE(r.has_value());
    return r.error().message;
}

} // namespace

TEST_CASE("a later feature's references survive an unrelated direct edit and break when their face is deleted; "
          "editing a direct edit replays the features after it",
          "[doc][directedit]")
{
    Model m;
    plate(m);
    const BuildResultPtr b0 = m.build();
    const naming::TopoRef wallA = refTo(*b0, 'b', cylinderFace(*b0, 'b', 3));
    const naming::TopoRef wallB = refTo(*b0, 'b', cylinderFace(*b0, 'b', 4));
    const naming::TopoRef front = refTo(*b0, 'b', planeFace(*b0, 'b', {0, -1, 0}, 0));
    // Later features, picked on the plate: hole A opened up to 10 mm, the front face pushed out 1 mm.
    m.run(addFeature("resize_hole", 5, resizeProps(wallA, "\"10 mm\"")));
    CHECK(m.state->features.back().name == "Resize Hole 1");
    m.run(addFeature("offset_face", 6, offsetProps({front}, "1")));
    CHECK(m.state->features.back().name == "Offset Face 1");
    BuildResultPtr b = m.build();
    REQUIRE(report(*b, 5).status == FeatureStatus::Ok);
    REQUIRE(report(*b, 6).status == FeatureStatus::Ok);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(kPlate + 600 - hole(5) - hole(4)).epsilon(1e-9));

    // A deletion of hole B inserted before them: their references are untouched by it and they build as before.
    m.run(addFeature("delete_face", 4, deleteProps({wallB}), R"(,"index":2)"));
    REQUIRE(m.state->features[2].type == fx::kDeleteFaceType);
    CHECK(m.state->features[2].name == "Delete Face 1");
    b = m.build();
    for (const std::uint64_t n : {4, 5, 6}) {
        INFO("feature " << n << ": " << report(*b, n).message);
        CHECK(report(*b, n).status == FeatureStatus::Ok);
        CHECK(report(*b, n).message.empty());
    }
    CHECK_FALSE(report(*b, 5).cacheHit); // the body it reads changed
    CHECK(volumeOf(*b, 'b') == Catch::Approx(kPlate + 600 - hole(5)).epsilon(1e-9));
    CHECK(cylinders(bodyOf(*b, 'b').shape, 4) == 0);
    CHECK(cylinders(bodyOf(*b, 'b').shape, 5) >= 1);
    CHECK(resolveOn(*b, 'b', wallA).state == naming::ResolveState::Exact); // the opened-up wall is still wall A

    // Nothing changed: every feature comes from the cache.
    b = m.build();
    for (const std::uint64_t n : {2, 3, 4, 5, 6}) {
        CHECK(report(*b, n).cacheHit);
    }

    // Editing the resize (a smaller diameter, as an expression) replays it and the push/pull after it; the deletion
    // before it is reused.
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","name":"bore","expression":"7 mm","kind":"length"})"));
    m.run(editFeature("resize_hole", 5, R"({"diameter":"bore"})"));
    b = m.build();
    CHECK(report(*b, 4).cacheHit);
    CHECK_FALSE(report(*b, 5).cacheHit);
    CHECK_FALSE(report(*b, 6).cacheHit);
    CHECK(report(*b, 5).status == FeatureStatus::Ok);
    CHECK(report(*b, 6).status == FeatureStatus::Ok);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(kPlate + 600 - hole(3.5)).epsilon(1e-9));
    // A parameter nothing uses changes nothing.
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 2) + R"(","name":"unused","expression":"1 mm","kind":"length"})"));
    b = m.build();
    for (const std::uint64_t n : {4, 5, 6}) {
        CHECK(report(*b, n).cacheHit);
    }

    // The deletion now takes hole A: the resize's face is gone, so the resize fails naming it, the push/pull after it
    // is not built, and the body is the plate as the deletion left it (nothing is resized on a guess).
    m.run(editFeature("delete_face", 4, deleteProps({wallA})));
    b = m.build();
    CHECK(report(*b, 4).status == FeatureStatus::Ok);
    CHECK(report(*b, 5).status == FeatureStatus::Failed);
    CHECK_THAT(report(*b, 5).message, ContainsSubstring("Resize Hole 1: its face no longer exists"));
    CHECK(report(*b, 6).status == FeatureStatus::NotBuilt);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(kPlate - hole(4)).epsilon(1e-9));
    CHECK(cylinders(bodyOf(*b, 'b').shape, 3) == 0);
    CHECK(resolveOn(*b, 'b', wallA).state == naming::ResolveState::Broken);
}

TEST_CASE("a push/pull distance is an expression; its edits rebuild it; 0 fails and leaves the body",
          "[doc][directedit]")
{
    Model m;
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","name":"d","expression":"2 mm","kind":"length"})"));
    m.run(addFeature("sketch", 2, sketchProps(kXY, SketchJson().rect(1, 0, 0, 40, 20))));
    m.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
    const BuildResultPtr b0 = m.build();
    const naming::TopoRef top = refTo(*b0, 'b', planeFace(*b0, 'b', {0, 0, 1}, 10));
    m.run(addFeature("offset_face", 4, offsetProps({top}, "\"d\"")));
    BuildResultPtr b = m.build();
    REQUIRE(report(*b, 4).status == FeatureStatus::Ok);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(40.0 * 20 * 12).epsilon(1e-12));
    // The pushed face is still the top face, now at z 12.
    const naming::Resolved r = resolveOn(*b, 'b', top);
    CHECK(r.state == naming::ResolveState::Exact);
    REQUIRE(r.elements.size() == 1);
    CHECK(planeOf(bodyOf(*b, 'b').shape, {0, 0, 1}, 12) == r.elements.front());

    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","expression":"-3 mm"})"));
    b = m.build();
    CHECK_FALSE(report(*b, 4).cacheHit);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(40.0 * 20 * 7).epsilon(1e-12));

    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","expression":"0 mm"})"));
    b = m.build();
    CHECK(report(*b, 4).status == FeatureStatus::Failed);
    CHECK_THAT(report(*b, 4).message, ContainsSubstring("must not be 0"));
    CHECK(volumeOf(*b, 'b') == Catch::Approx(8000.0).epsilon(1e-12));
}

TEST_CASE("kernel warnings become the feature's warnings word for word", "[doc][directedit]")
{
    // A block with one top edge rounded: its top face meets the round tangentially, so pushing the top face up leaves
    // a step there, which the kernel says.
    Model m;
    m.run(addFeature("sketch", 2, sketchProps(kXY, SketchJson().rect(1, 0, 0, 40, 20))));
    m.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
    BuildResultPtr b = m.build();
    const auto edge = findEdge(bodyOf(*b, 'b').shape, [](const kernel::CurveInfo& c) {
        return c.type == kernel::CurveType::Line && near(c.start.z, 10) && near(c.end.z, 10) && near(c.start.y, 0) &&
               near(c.end.y, 0);
    });
    REQUIRE(edge);
    m.run(addFeature("fillet", 4, R"({"edges":[)" + refJson(refTo(*b, 'b', *edge)) + R"(],"radius":2})"));
    b = m.build();
    REQUIRE(report(*b, 4).status == FeatureStatus::Ok);
    const kernel::SubId top = planeFace(*b, 'b', {0, 0, 1}, 10);
    JobContext direct;
    const std::vector<kernel::SubId> faces{top};
    auto expected = kernel::offsetFaces(bodyOf(*b, 'b').shape, faces, 1.0, direct);
    REQUIRE(expected);
    REQUIRE_FALSE(expected->warnings.empty());
    m.run(addFeature("offset_face", 5, offsetProps({refTo(*b, 'b', top)}, "1")));
    b = m.build();
    CHECK(report(*b, 5).status == FeatureStatus::Warning);
    std::string want;
    for (const std::string& w : expected->warnings) {
        want += (want.empty() ? "" : "; ") + std::string("Offset Face 1: ") + w;
    }
    CHECK(report(*b, 5).message == want);
    CHECK_THAT(report(*b, 5).message, ContainsSubstring("step"));
    CHECK(bodyOf(*b, 'b').shape.contentHash() == expected->shape.contentHash());
}

TEST_CASE("a kernel refusal is the feature's failure with the kernel's message, and the body stays as it was",
          "[doc][directedit]")
{
    Model m;
    plate(m);
    const BuildResultPtr b0 = m.build();
    const kernel::Shape& shape = bodyOf(*b0, 'b').shape;
    const kernel::SubId wallA = cylinderFace(*b0, 'b', 3);
    const kernel::SubId top = planeFace(*b0, 'b', {0, 0, 1}, 10);
    const DocStatePtr base = m.state;
    JobContext ctx;
    const std::vector<kernel::SubId> wall{wallA}, topOnly{top};
    struct Case {
        Command add;
        std::string message;
    };
    const std::vector<Case> cases{
        // A hole that already has the diameter (no success for a no-op).
        {addFeature("resize_hole", 4, resizeProps(refTo(*b0, 'b', wallA), "6")),
         "Resize Hole 1: " + kernelMessage(kernel::resizeHole(shape, wallA, 6.0, ctx))},
        // A face that is not part of a hole.
        {addFeature("resize_hole", 4, resizeProps(refTo(*b0, 'b', top), "6")),
         "Resize Hole 1: " + kernelMessage(kernel::resizeHole(shape, top, 6.0, ctx))},
        // Push/pull is for flat faces.
        {addFeature("offset_face", 4, offsetProps({refTo(*b0, 'b', wallA)}, "1")),
         "Offset Face 1: " + kernelMessage(kernel::offsetFaces(shape, wall, 1.0, ctx))},
        // The top of the plate cannot be removed: its neighbours cannot close the gap.
        {addFeature("delete_face", 4, deleteProps({refTo(*b0, 'b', top)})),
         "Delete Face 1: " + kernelMessage(kernel::deleteFaces(shape, topOnly, ctx))},
    };
    for (const Case& c : cases) {
        INFO(c.add.name << " " << c.message);
        m.state = base;
        m.run(c.add);
        const BuildResultPtr b = m.build();
        CHECK(report(*b, 4).status == FeatureStatus::Failed);
        CHECK(report(*b, 4).message == c.message);
        CHECK(bodyOf(*b, 'b').shape.contentHash() == shape.contentHash());
    }
    CHECK_THAT(cases.back().message, EndsWith("Nothing was changed."));
    // A diameter that is not above 0 is the feature's own refusal.
    m.state = base;
    m.run(addFeature("resize_hole", 4, resizeProps(refTo(*b0, 'b', wallA), "\"0 mm\"")));
    const BuildResultPtr b = m.build();
    CHECK(report(*b, 4).status == FeatureStatus::Failed);
    CHECK_THAT(report(*b, 4).message, ContainsSubstring("greater than 0"));
    CHECK(bodyOf(*b, 'b').shape.contentHash() == shape.contentHash());
}

TEST_CASE("the direct-edit commands refuse bad props, naming the field", "[doc][directedit]")
{
    Model m;
    plate(m);
    m.run(addFeature("sketch", 7, sketchProps(kXY, SketchJson().rect(1, 100, 0, 110, 10))));
    m.run(addFeature("extrude", 8, extrudeProps(7, "5", "new", 'c')));
    const BuildResultPtr b = m.build();
    const naming::TopoRef wall = refTo(*b, 'b', cylinderFace(*b, 'b', 3));
    const naming::TopoRef other = refTo(*b, 'c', planeFace(*b, 'c', {0, 0, 1}, 5));
    const auto edge = findEdge(bodyOf(*b, 'b').shape, [](const kernel::CurveInfo&) { return true; });
    REQUIRE(edge);
    const naming::TopoRef anEdge = refTo(*b, 'b', *edge);
    Error e = m.fails(addFeature("delete_face", 4, R"({"faces":[]})"));
    CHECK_THAT(e.message, ContainsSubstring("'faces'") && ContainsSubstring("at least one face"));
    e = m.fails(addFeature("delete_face", 4, deleteProps({wall, anEdge})));
    CHECK_THAT(e.message, ContainsSubstring("'faces[1]'") && ContainsSubstring("must be a face, not an edge"));
    e = m.fails(addFeature("delete_face", 4, deleteProps({wall, other})));
    CHECK_THAT(e.message, ContainsSubstring("'faces[1]'") && ContainsSubstring("another body"));
    e = m.fails(addFeature("delete_face", 4, R"({"faces":)" + facesJson({wall}) + R"(,"distance":1})"));
    CHECK_THAT(e.message, ContainsSubstring("'distance'"));
    e = m.fails(addFeature("offset_face", 4, deleteProps({wall})));
    CHECK_THAT(e.message, ContainsSubstring("'distance'"));
    e = m.fails(addFeature("offset_face", 4, offsetProps({wall}, "\"30 deg\"")));
    CHECK_THAT(e.message, ContainsSubstring("distance"));
    e = m.fails(addFeature("offset_face", 4, offsetProps({wall}, "\"depth\"")));
    CHECK_THAT(e.message, ContainsSubstring("unknown name 'depth'"));
    e = m.fails(addFeature("resize_hole", 4, R"({"diameter":5})"));
    CHECK_THAT(e.message, ContainsSubstring("'face'") && ContainsSubstring("required"));
    e = m.fails(addFeature("resize_hole", 4, resizeProps(anEdge, "5")));
    CHECK_THAT(e.message, ContainsSubstring("'face'") && ContainsSubstring("must be a face"));
    e = m.fails(addFeature("resize_hole", 4, R"({"faces":)" + facesJson({wall}) + R"(,"diameter":5})"));
    CHECK_THAT(e.message, ContainsSubstring("'faces'"));
    // A face of a body the document does not have.
    naming::TopoRef stray = wall;
    stray.body = BodyId{Uuid::fromString(uuid('e', 9))};
    e = m.fails(addFeature("delete_face", 4, deleteProps({stray})));
    CHECK_THAT(e.message, ContainsSubstring("not in the document"));
}

namespace {

// A resolver double: every reference resolves to `answer` (its repairConfirmed copied from the reference, as the real
// resolver does).
class FixedResolver final : public naming::IResolver {
public:
    naming::Resolved answer{naming::ResolveState::Broken, {}, {}, false};
    naming::Resolved resolve(const naming::TopoRef& ref) const override
    {
        naming::Resolved r = answer;
        r.repairConfirmed = ref.repairConfirmed;
        return r;
    }
    naming::TopoRef makeRef(BodyId, kernel::SubId) const override { return {}; }
};

// A rebuild context with one body, for running a feature type's rebuild() with any resolve answer.
class FixedContext final : public RebuildContext {
public:
    FixedContext(const DocState& s, const std::map<std::string, double>& p, const naming::IResolver& r, JobContext& j,
                 kernel::Shape shape, naming::ElementMap map)
        : RebuildContext(s, p, r, j), shape_(std::move(shape)), map_(std::move(map))
    {
    }
    Result<kernel::Shape> shapeOf(BodyId) const override { return shape_; }
    const naming::ElementMap& mapOf(BodyId) const override { return map_; }
    Result<std::vector<std::byte>> blob(const Hash256&) const override
    {
        return tl::unexpected(Error{ErrorCode::NotFound, "no objects", {}});
    }

private:
    kernel::Shape shape_;
    naming::ElementMap map_;
};

// The two-hole plate made by the kernel directly: hole A radius 3 at (15, 15), hole B radius 4 at (45, 15).
kernel::Shape kernelPlate()
{
    JobContext ctx;
    auto box = kernel::makeBox({0, 0, 0}, {60, 30, 10});
    REQUIRE(box);
    auto a = kernel::makeCylinder({15, 15, -1}, {0, 0, 1}, 3, 12);
    auto b = kernel::makeCylinder({45, 15, -1}, {0, 0, 1}, 4, 12);
    REQUIRE(a);
    REQUIRE(b);
    auto one = kernel::boolean(kernel::BoolOp::Cut, *box, *a, ctx);
    REQUIRE(one);
    auto two = kernel::boolean(kernel::BoolOp::Cut, one->shape, *b, ctx);
    REQUIRE(two);
    return two->shape;
}

struct StateRun {
    kernel::Shape plate = kernelPlate();
    naming::ElementMap map = naming::nameImported(featureId(1), plate);
    kernel::SubId wallA = *cylinder(plate, 3);
    kernel::SubId wallB = *cylinder(plate, 4);
    kernel::SubId top = *planeOf(plate, {0, 0, 1}, 10);
    naming::TopoRef ref =
        naming::makeResolver({naming::ResolverBody{bodyId('b'), map, plate}})->makeRef(bodyId('b'), wallA);

    // The feature of `type` named "<title> 1" with `props`, rebuilt with every reference resolving to `answer`.
    Result<FeatureOutput> rebuild(std::string_view type, const std::string& title, const std::string& props,
                                  naming::ResolveState state, std::vector<kernel::SubId> elements,
                                  std::string reason = {})
    {
        const IFeatureType* t = builtinFeatureRegistry()->find(type);
        REQUIRE(t != nullptr);
        Feature f;
        f.id = featureId(4);
        f.type = std::string(type);
        f.name = title + " 1";
        f.propsJson = canonicalJson(props);
        FixedResolver resolver;
        resolver.answer = naming::Resolved{state, std::move(elements), std::move(reason), false};
        const DocState doc;
        const std::map<std::string, double> params;
        JobContext job;
        FixedContext ctx(doc, params, resolver, job, plate, map);
        return t->rebuild(f, ctx);
    }
};

} // namespace

TEST_CASE("each resolve state is used, flagged or refused as the other features do; nothing is edited on a guess",
          "[doc][directedit]")
{
    StateRun s;
    const std::string del = deleteProps({s.ref});
    using RS = naming::ResolveState;

    SECTION("Exact: the face, no warning")
    {
        auto out = s.rebuild(fx::kDeleteFaceType, "Delete Face", del, RS::Exact, {s.wallA});
        REQUIRE(out);
        CHECK(out->warnings.empty());
        REQUIRE(out->bodies.size() == 1);
        CHECK(cylinders(out->bodies[0].shape, 3) == 0);
        CHECK(cylinders(out->bodies[0].shape, 4) == 1);
    }
    SECTION("Split: every piece, with a warning")
    {
        auto out = s.rebuild(fx::kDeleteFaceType, "Delete Face", del, RS::Split, {s.wallA, s.wallB});
        REQUIRE(out);
        REQUIRE(out->warnings.size() == 1);
        CHECK(out->warnings[0] == "Delete Face 1: its face was split by an earlier change; all 2 pieces are deleted.");
        CHECK(cylinders(out->bodies[0].shape, 3) + cylinders(out->bodies[0].shape, 4) == 0);
    }
    SECTION("Merged: the whole face, with a warning")
    {
        auto out = s.rebuild(fx::kOffsetFaceType, "Offset Face", offsetProps({s.ref}, "1"), RS::Merged, {s.top});
        REQUIRE(out);
        REQUIRE(out->warnings.size() == 1);
        CHECK(out->warnings[0] == "Offset Face 1: its face is now part of a larger face; the whole face is moved.");
    }
    SECTION("Repaired: refused until the user confirms it, then used with a warning")
    {
        auto out = s.rebuild(fx::kDeleteFaceType, "Delete Face", del, RS::Repaired, {s.wallA}, "one candidate");
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::ReferenceBroken);
        CHECK(out.error().message == "Delete Face 1: its face could not be found exactly; a likely match was found "
                                     "(one candidate): confirm it in the reference review, or pick the face again.");
        naming::TopoRef confirmed = s.ref;
        confirmed.repairConfirmed = true;
        out = s.rebuild(fx::kResizeHoleType, "Resize Hole", resizeProps(confirmed, "8"), RS::Repaired, {s.wallA});
        REQUIRE(out);
        REQUIRE(out->warnings.size() == 1);
        CHECK(out->warnings[0] == "Resize Hole 1: its face uses a repair the user confirmed.");
        CHECK(cylinders(out->bodies[0].shape, 3) == 0);
        CHECK(cylinders(out->bodies[0].shape, 4) >= 2); // hole A now as wide as hole B
    }
    SECTION("Broken: refused, naming the face and why")
    {
        for (const std::string_view type : {fx::kDeleteFaceType, fx::kOffsetFaceType, fx::kResizeHoleType}) {
            const std::string title = type == fx::kDeleteFaceType   ? "Delete Face"
                                      : type == fx::kOffsetFaceType ? "Offset Face"
                                                                    : "Resize Hole";
            const std::string props = type == fx::kDeleteFaceType   ? del
                                      : type == fx::kOffsetFaceType ? offsetProps({s.ref}, "1")
                                                                    : resizeProps(s.ref, "8");
            auto out = s.rebuild(type, title, props, RS::Broken, {}, "its feature was removed");
            REQUIRE_FALSE(out);
            CHECK(out.error().code == ErrorCode::ReferenceBroken);
            CHECK(out.error().message ==
                  title + " 1: its face no longer exists (its feature was removed): pick it again or remove it.");
        }
        // Of several faces, the message says which.
        auto out = s.rebuild(fx::kDeleteFaceType, "Delete Face", deleteProps({s.ref, s.ref}), RS::Broken, {});
        REQUIRE_FALSE(out);
        CHECK_THAT(out.error().message, ContainsSubstring("face 1 of 2 no longer exists") &&
                                            ContainsSubstring("face 2 of 2 no longer exists"));
    }
    SECTION("an answer that is not a face, or no answer at all, is refused")
    {
        auto out =
            s.rebuild(fx::kDeleteFaceType, "Delete Face", del, RS::Exact, {kernel::SubId{kernel::SubKind::Edge, 0}});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::ReferenceBroken);
        CHECK_THAT(out.error().message, ContainsSubstring("not a face"));
        out = s.rebuild(fx::kDeleteFaceType, "Delete Face", del, RS::Exact, {});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::ReferenceBroken);
    }
    SECTION("a hole face split into pieces of two holes is not resized in part")
    {
        auto out =
            s.rebuild(fx::kResizeHoleType, "Resize Hole", resizeProps(s.ref, "10"), RS::Split, {s.wallA, s.wallB});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::ReferenceBroken);
        CHECK_THAT(out.error().message, ContainsSubstring("not one hole") && EndsWith("Nothing was changed."));
    }
}

TEST_CASE("a whole blend run comes out with one deletion", "[doc][directedit]")
{
    // A block with its four top edges rounded by one fillet feature: the rounds form one blend run (kernel blendRun,
    // which the model.blendRun action gives the UI).
    Model m;
    m.run(addFeature("sketch", 2, sketchProps(kXY, SketchJson().rect(1, 0, 0, 30, 20))));
    m.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
    BuildResultPtr b = m.build();
    std::string edges = "[";
    const kernel::Shape& block = bodyOf(*b, 'b').shape;
    for (std::uint32_t i = 0; i < block.count(kernel::SubKind::Edge); ++i) {
        auto c = kernel::curveInfo(block, kernel::SubId{kernel::SubKind::Edge, i});
        if (c && near(c->start.z, 10) && near(c->end.z, 10)) {
            edges += (edges.size() > 1 ? "," : "") + refJson(refTo(*b, 'b', kernel::SubId{kernel::SubKind::Edge, i}));
        }
    }
    m.run(addFeature("fillet", 4, R"({"edges":)" + edges + R"(],"radius":2})"));
    b = m.build();
    REQUIRE(report(*b, 4).status == FeatureStatus::Ok);
    const kernel::Shape& rounded = bodyOf(*b, 'b').shape;
    const std::vector<kernel::SubId> run = kernel::blendRun(rounded, cylinderFace(*b, 'b', 2));
    REQUIRE(run.size() >= 4);
    std::string faces = "[";
    for (const kernel::SubId& f : run) {
        faces += (faces.size() > 1 ? "," : "") + refJson(refTo(*b, 'b', f));
    }
    m.run(addFeature("delete_face", 5, R"({"faces":)" + faces + "]}"));
    b = m.build();
    INFO(report(*b, 5).message);
    REQUIRE(report(*b, 5).status == FeatureStatus::Ok);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(6000.0).epsilon(1e-9));
    CHECK(cylinders(bodyOf(*b, 'b').shape, 2) == 0);
}

namespace {

std::string corpusFile(const char* name)
{
    return std::string(RAY_CORPUS_ROOT_DIR) + "/" + name;
}

// Faces of `shape` on the cylinder about the vertical axis through (x, y) (any radius).
std::vector<kernel::SubId> wallsAt(const kernel::Shape& shape, double x, double y)
{
    std::vector<kernel::SubId> out;
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        auto s = kernel::surfaceInfo(shape, f);
        if (s && s->type == kernel::SurfaceType::Cylinder && near(std::abs(s->axis.z), 1, 1e-9) &&
            near(s->origin.x, x, 1e-6) && near(s->origin.y, y, 1e-6)) {
            out.push_back(f);
        }
    }
    return out;
}

} // namespace

TEST_CASE("direct edits on an imported body build, undo, redo, save and reload like any feature", "[doc][directedit]")
{
    // The S9 plate whose hole walls come as two half faces each, as many exporters write them.
    DocEnv env;
    env.import(corpusFile("directedit/synthetic/syn_02_plate_holes_1.stp"), 1);
    REQUIRE(env.settle());
    REQUIRE(env.doc->state()->bodies.size() == 1);
    const BodyId body = env.doc->state()->bodies.front().id;
    const BuildResultPtr b0 = env.doc->build();
    const kernel::Shape& imported = b0->bodies.at(body).shape;
    // The holes, by the axes of their walls (vertical), lowest x first.
    std::vector<std::pair<Vec3, double>> holes; // axis point, radius
    const Box3 box = kernel::bounds(imported);
    for (std::uint32_t i = 0; i < imported.count(kernel::SubKind::Face); ++i) {
        auto s = kernel::surfaceInfo(imported, kernel::SubId{kernel::SubKind::Face, i});
        // Vertical cylinders whose whole circle lies inside the plate's outline: holes, not its rounded corners.
        if (!s || s->type != kernel::SurfaceType::Cylinder || !near(std::abs(s->axis.z), 1, 1e-9) ||
            s->origin.x - s->radius < box.min.x + 1e-3 || s->origin.x + s->radius > box.max.x - 1e-3 ||
            s->origin.y - s->radius < box.min.y + 1e-3 || s->origin.y + s->radius > box.max.y - 1e-3) {
            continue;
        }
        if (std::none_of(holes.begin(), holes.end(), [&](const auto& h) {
                return near(h.first.x, s->origin.x, 1e-6) && near(h.first.y, s->origin.y, 1e-6);
            })) {
            holes.emplace_back(s->origin, s->radius);
        }
    }
    std::sort(holes.begin(), holes.end(), [](const auto& a, const auto& b) { return a.first.x < b.first.x; });
    REQUIRE(holes.size() >= 2);
    const auto [axisA, radiusA] = holes[0];
    const auto [axisB, radiusB] = holes[1];
    const std::vector<kernel::SubId> wallA = wallsAt(imported, axisA.x, axisA.y);
    const std::vector<kernel::SubId> wallB = wallsAt(imported, axisB.x, axisB.y);
    INFO("hole A has " << wallA.size() << " wall faces, hole B " << wallB.size());
    CHECK(wallA.size() == 2); // the halves: one of them stands for the whole hole
    const auto refOn = [&](kernel::SubId sub) {
        auto resolver = naming::makeResolver({naming::ResolverBody{body, b0->bodies.at(body).map, imported}});
        return resolver->makeRef(body, sub);
    };
    double zTop = -1e300;
    std::optional<kernel::SubId> top;
    for (std::uint32_t i = 0; i < imported.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        auto s = kernel::surfaceInfo(imported, f);
        auto n = kernel::faceSample(imported, f);
        if (s && n && s->type == kernel::SurfaceType::Plane && near(n->normal.z, 1, 1e-9) && s->centroid.z > zTop) {
            zTop = s->centroid.z;
            top = f;
        }
    }
    REQUIRE(top);
    const auto volume = [&](const BuildResultPtr& b) {
        auto mp = kernel::massProps(b->bodies.at(body).shape);
        REQUIRE(mp);
        return mp->volume;
    };
    const double v0 = volume(b0);
    const DocStatePtr importedState = env.doc->state();

    // Hole A 25% wider by one of its half faces; hole B filled by one of its half faces; the top pushed up 1 mm.
    env.run(addFeature("resize_hole", 2, resizeProps(refOn(wallA.front()), num(2 * radiusA * 1.25))));
    env.run(addFeature("delete_face", 3, deleteProps({refOn(wallB.back())})));
    env.run(addFeature("offset_face", 4, offsetProps({refOn(*top)}, "\"1 mm\"")));
    REQUIRE(env.settle());
    const BuildResultPtr b1 = env.doc->build();
    // The resize is a boolean, whose same-domain unification also joins the halves of the other holes: the half face
    // of hole B is then part of a larger face (Merged), which the deletion uses and says. The push/pull may carry the
    // kernel's note that some coplanar faces were left apart. Warnings, not failures, each under the feature's name.
    for (const auto& [n, title] : {std::pair<std::uint64_t, std::string>{2, "Resize Hole 1"},
                                   std::pair<std::uint64_t, std::string>{3, "Delete Face 1"},
                                   std::pair<std::uint64_t, std::string>{4, "Offset Face 1"}}) {
        INFO("feature " << n << ": " << report(*b1, n).message);
        CHECK((report(*b1, n).status == FeatureStatus::Ok || report(*b1, n).status == FeatureStatus::Warning));
        if (report(*b1, n).status == FeatureStatus::Warning) {
            CHECK_THAT(report(*b1, n).message, Catch::Matchers::StartsWith(title + ": "));
        }
    }
    CHECK(report(*b1, 2).status == FeatureStatus::Ok);
    const kernel::Shape& edited = b1->bodies.at(body).shape;
    const std::vector<kernel::SubId> nowA = wallsAt(edited, axisA.x, axisA.y);
    REQUIRE_FALSE(nowA.empty());
    for (const kernel::SubId& f : nowA) {
        auto s = kernel::surfaceInfo(edited, f);
        REQUIRE(s);
        CHECK(near(s->radius, radiusA * 1.25, 1e-6)); // every half: the whole hole
    }
    CHECK(wallsAt(edited, axisB.x, axisB.y).empty());
    const std::string after = fingerprint(*env.doc->state(), *b1);

    // Undo the three, redo them: the same states and the same build.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(env.doc->undo());
    }
    REQUIRE(env.settle());
    CHECK(env.doc->state() == importedState);
    CHECK(volume(env.doc->build()) == Catch::Approx(v0).epsilon(1e-12));
    for (int i = 0; i < 3; ++i) {
        REQUIRE(env.doc->redo());
    }
    REQUIRE(env.settle());
    CHECK(fingerprint(*env.doc->state(), *env.doc->build()) == after);

    // Saved and loaded: the same snapshot hash, and a fresh rebuilder (on the same stored objects) makes the same
    // bodies and names.
    const StateObjects objects = serializeState(*env.doc->state());
    std::map<Hash256, std::string> store;
    for (const auto& [hash, bytes] : objects.features) {
        store[hash] = bytes;
    }
    auto loaded = deserializeState(objects.manifest, [&](const Hash256& h) -> Result<std::string> {
        const auto it = store.find(h);
        if (it == store.end()) {
            return tl::unexpected(Error{ErrorCode::NotFound, "missing", {}});
        }
        return it->second;
    });
    REQUIRE(loaded);
    const auto reloaded = std::make_shared<const DocState>(std::move(*loaded));
    CHECK(reloaded->stateHash() == env.doc->state()->stateHash());
    RebuildEnv fresh(2, env.pool);
    CHECK(fingerprint(*reloaded, *fresh.rebuild(reloaded)) == after);
}

TEST_CASE("a direct edit previews through beginPreview without entering the document", "[doc][directedit][preview]")
{
    DocEnv env;
    env.run(addFeature(
        "sketch", 2,
        sketchProps(kXY, SketchJson().rect(1, 0, 0, 60, 30).circleAt(10, 15, 15, 3).circleAt(12, 45, 15, 4))));
    env.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
    REQUIRE(env.settle());
    const BuildResultPtr b0 = env.doc->build();
    const DocStatePtr s0 = env.doc->state();
    const std::size_t undos = env.doc->undoCount();
    const bool dirty = env.doc->dirty();
    const naming::TopoRef wallA = refTo(*b0, 'b', cylinderFace(*b0, 'b', 3));
    const naming::TopoRef wallB = refTo(*b0, 'b', cylinderFace(*b0, 'b', 4));
    const auto unchanged = [&] {
        CHECK(env.doc->state() == s0);
        CHECK(env.doc->undoCount() == undos);
        CHECK(env.doc->build() == b0);
        CHECK(env.doc->dirty() == dirty);
    };
    const auto previewed = [&]() -> BuildResultPtr {
        REQUIRE(env.ui.runUntil([&] { return env.doc->previewBuild() != nullptr; }, std::chrono::seconds(60)));
        return env.doc->previewBuild();
    };

    // Deleting hole B, previewed.
    env.doc->beginPreview(addFeature("delete_face", 4, deleteProps({wallB})));
    BuildResultPtr p = previewed();
    unchanged();
    REQUIRE(env.doc->previewState());
    CHECK(env.doc->previewState()->features.size() == 3);
    CHECK(report(*p, 4).status == FeatureStatus::Ok);
    CHECK(volumeOf(*p, 'b') == Catch::Approx(kPlate - hole(3)).epsilon(1e-9));
    // Replaced by a resize of hole A to 8 mm, as the numeric field changes.
    env.doc->beginPreview(addFeature("resize_hole", 4, resizeProps(wallA, "\"8 mm\"")));
    p = previewed();
    unchanged();
    CHECK(report(*p, 4).status == FeatureStatus::Ok);
    CHECK(volumeOf(*p, 'b') == Catch::Approx(kPlate - 2 * hole(4)).epsilon(1e-9));
    // A size the kernel refuses previews as the failure with the kernel's message; the document still has nothing.
    env.doc->beginPreview(addFeature("resize_hole", 4, resizeProps(wallA, "6")));
    p = previewed();
    unchanged();
    CHECK(report(*p, 4).status == FeatureStatus::Failed);
    CHECK_THAT(report(*p, 4).message, ContainsSubstring("Resize Hole 1: ") && ContainsSubstring("already 6 mm across"));
    env.doc->cancelPreview();
    CHECK(env.doc->previewState() == nullptr);
    unchanged();

    // Once added, an edit of its diameter previews the same way.
    env.run(addFeature("resize_hole", 4, resizeProps(wallA, "8")));
    REQUIRE(env.settle());
    const DocStatePtr s1 = env.doc->state();
    const BuildResultPtr b1 = env.doc->build();
    env.doc->beginPreview(editFeature("resize_hole", 4, R"({"diameter":"10 mm"})"));
    p = previewed();
    CHECK(env.doc->state() == s1);
    CHECK(env.doc->build() == b1);
    CHECK(report(*p, 4).status == FeatureStatus::Ok);
    CHECK(volumeOf(*p, 'b') == Catch::Approx(kPlate - hole(5) - hole(4)).epsilon(1e-9));
    CHECK(volumeOf(*b1, 'b') == Catch::Approx(kPlate - 2 * hole(4)).epsilon(1e-9));
    env.doc->cancelPreview();
    CHECK(env.doc->state() == s1);
}

// Hidden: run by the ctest test ray_doc_directedit_cancel, alone (its pass criterion is a wall-clock budget).
TEST_CASE("a rebuild cancelled in a slow face deletion is released at once and leaves the document as it was",
          "[.directedit-cancel]")
{
    // Fillets and blend runs of NIST CTC 05 whose removal runs for minutes (the kernel's cancel test uses them).
    using Clock = std::chrono::steady_clock;
    DocEnv env;
    env.import(std::string(RAY_CORPUS_DIR) + "/nist-pmi/nist_ctc_05_asme1_rd_geometry.stp", 1);
    REQUIRE(env.settle(std::chrono::minutes(5)));
    const DocStatePtr s0 = env.doc->state();
    const BuildResultPtr b0 = env.doc->build();
    // The largest solid of the file.
    BodyId body;
    double largest = -1.0;
    for (const auto& [id, built] : b0->bodies) {
        auto mp = kernel::massProps(built.shape);
        if (built.shape.count(kernel::SubKind::Solid) == 1 && mp && mp->volume > largest) {
            largest = mp->volume;
            body = id;
        }
    }
    REQUIRE(largest > 0);
    const BuiltBody& part = b0->bodies.at(body);
    auto cases = kernel::testing::directEditCases(part.shape);
    REQUIRE(cases);
    kernel::testing::waitForAbandonedEdits();
    auto resolver = naming::makeResolver({naming::ResolverBody{body, part.map, part.shape}});
    const ICommandHandler* add = builtinCommandRegistry()->find("feature.delete_face.add");
    REQUIRE(add != nullptr);
    bool measured = false;
    for (const kernel::testing::DirectEditCase& c : *cases) {
        if (c.edit != kernel::testing::DirectEdit::DeleteBlendRun &&
            c.edit != kernel::testing::DirectEdit::DeleteFillet) {
            continue;
        }
        std::string faces = "[";
        for (const kernel::SubId& f : c.faces) {
            faces += (faces.size() > 1 ? "," : "") + refJson(resolver->makeRef(body, f));
        }
        const Command command = addFeature("delete_face", 2, R"({"faces":)" + faces + "]}");
        auto s1 = add->apply(s0, command.argsJson);
        REQUIRE(s1);

        // The rebuilder, cancelled after 2 s of the deletion.
        CancelSource source;
        JobContext ctx{source.token(), {}};
        std::optional<Result<BuildResultPtr>> result;
        Clock::time_point returned;
        std::atomic<bool> done{false};
        std::thread caller([&] {
            result.emplace(env.rebuilder->rebuild(*s1, ctx));
            returned = Clock::now();
            done.store(true);
        });
        const Clock::time_point start = Clock::now();
        while (!done.load() && Clock::now() - start < std::chrono::seconds(2)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const bool slow = !done.load();
        const Clock::time_point cancelled = Clock::now();
        source.cancel();
        caller.join();
        if (!slow) {
            continue; // done (or refused) within 2 s: says nothing about cancelling
        }
        const double releaseMs = std::chrono::duration<double, std::milli>(returned - cancelled).count();
        INFO(kernel::testing::directEditName(c.edit)
             << " k" << c.k << ": the rebuild returned " << releaseMs << " ms after the cancel");
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->has_value());
        CHECK(result->error().code == ErrorCode::Cancelled);
        CHECK(releaseMs < 100.0);
        std::printf("rebuild cancelled in %s k%d after 2 s: released after %.1f ms\n",
                    kernel::testing::directEditName(c.edit), c.k, releaseMs);

        // The document: the command is executed, its rebuild cancelled during the deletion. build() stays the build
        // of the imported part (no partial result enters), the command stays undoable, and undo brings back the
        // imported state.
        env.run(command);
        (void)env.ui.runUntil([] { return false; }, std::chrono::seconds(2));
        REQUIRE(env.doc->rebuildPending());
        const Clock::time_point docCancel = Clock::now();
        env.doc->cancelRebuild();
        REQUIRE(env.settle(std::chrono::seconds(5)));
        const double docMs = std::chrono::duration<double, std::milli>(Clock::now() - docCancel).count();
        std::printf("document rebuild cancelled: settled after %.1f ms\n", docMs);
        CHECK(docMs < 250.0);
        REQUIRE(env.doc->buildError());
        CHECK(env.doc->buildError()->code == ErrorCode::Cancelled);
        CHECK(env.doc->build() == b0);
        CHECK(env.doc->builtState() == s0);
        REQUIRE(env.doc->undo());
        CHECK(env.doc->state() == s0);
        REQUIRE(env.settle(std::chrono::seconds(60)));
        CHECK(fingerprint(*env.doc->state(), *env.doc->build()) == fingerprint(*s0, *b0));
        measured = true;
        break;
    }
    CHECK(measured);
    kernel::testing::waitForAbandonedEdits();
}
