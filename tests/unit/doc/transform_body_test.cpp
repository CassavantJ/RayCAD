// The "transform_body" feature type (task T-077): the mate arithmetic, a body mated to another body's face with a gap,
// a turn and offsets, through the typed commands and the rebuilder; the mate following an edit of the part; names
// kept through the move; the command's refusals; undo, redo, save and reload; documents from before the type.
#include "parametric_test_util.h"

#include "features/transform_body_feature.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>
#include <map>
#include <sstream>

using namespace ray;
using namespace ray::doc;
using namespace ray::doc::test;
using Catch::Matchers::ContainsSubstring;
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

// The part: sketch 2, a 40 x 20 rectangle at the origin, extrude 3 (distance `height`, new body 'b'). The fixture:
// sketch 4, a 30 x 10 rectangle at (100, 100), extrude 5 (15 mm, new body 'c'): x 100..130, y 100..110, z 0..15.
void twoBodies(Model& m, const std::string& height = "10")
{
    m.run(addFeature("sketch", 2, sketchProps(kXY, SketchJson().rect(1, 0, 0, 40, 20))));
    m.run(addFeature("extrude", 3, extrudeProps(2, height, "new", 'b')));
    m.run(addFeature("sketch", 4, sketchProps(kXY, SketchJson().rect(1, 100, 100, 130, 110))));
    m.run(addFeature("extrude", 5, extrudeProps(4, "15", "new", 'c')));
}

// The planar face of `body` whose outward normal is `n` (an axis) and whose centroid lies at `at` along it.
kernel::SubId planeFace(const BuildResult& build, char body, Vec3 n, double at)
{
    const kernel::Shape& shape = bodyOf(build, body).shape;
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
    FAIL("no such face on body " << body);
    return {};
}

std::string mateJson(const BuildResult& build, kernel::SubId face, kernel::SubId target, char targetBody = 'b')
{
    return R"({"face":)" + refJson(refTo(build, 'c', face)) + R"(,"target":)" +
           refJson(refTo(build, targetBody, target)) + "}";
}

void checkBox(const BuildResult& build, char body, Box3 want, double tol = 1e-6)
{
    const Box3 box = kernel::bounds(bodyOf(build, body).shape);
    INFO("body " << body << " box " << box.min.x << " " << box.min.y << " " << box.min.z << " .. " << box.max.x << " "
                 << box.max.y << " " << box.max.z);
    // OCCT's box may carry the shape's tolerance (1e-7) on each side.
    CHECK(box.min.x == Catch::Approx(want.min.x).margin(tol));
    CHECK(box.min.y == Catch::Approx(want.min.y).margin(tol));
    CHECK(box.min.z == Catch::Approx(want.min.z).margin(tol));
    CHECK(box.max.x == Catch::Approx(want.max.x).margin(tol));
    CHECK(box.max.y == Catch::Approx(want.max.y).margin(tol));
    CHECK(box.max.z == Catch::Approx(want.max.z).margin(tol));
}

Vec3 applyX(const Xform& x, Vec3 p)
{
    return Vec3{x.m[0] * p.x + x.m[1] * p.y + x.m[2] * p.z + x.m[3],
                x.m[4] * p.x + x.m[5] * p.y + x.m[6] * p.z + x.m[7],
                x.m[8] * p.x + x.m[9] * p.y + x.m[10] * p.z + x.m[11]};
}

std::string read(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string s = text.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

} // namespace

TEST_CASE("the mate puts the face on the target face, opposed and centred, keeping up up", "[doc][transform_body]")
{
    // A vertical face looking +Y onto a vertical face looking -Y: no turn, a shift only.
    fx::Placement p;
    p.face = fx::MateFace{{115, 100, 7.5}, {0, -1, 0}};
    p.target = fx::MateFace{{20, 20, 5}, {0, 1, 0}};
    Xform x = fx::placementTransform(p);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            CHECK(x.m[i * 4 + j] == (i == j ? 1.0 : 0.0));
        }
    }
    Vec3 c = applyX(x, {115, 100, 7.5});
    CHECK(c.x == 20);
    CHECK(c.y == 20);
    CHECK(c.z == 5);
    // The same face onto a face looking the same way: half a turn about Z (never upside down).
    p.target = fx::MateFace{{0, 0, 0}, {0, -1, 0}};
    x = fx::placementTransform(p);
    CHECK(x.m[0] == -1.0);
    CHECK(x.m[5] == -1.0);
    CHECK(x.m[10] == 1.0);
    // A floor onto a ceiling: Z faces, X kept.
    p.face = fx::MateFace{{0, 0, 0}, {0, 0, -1}};
    p.target = fx::MateFace{{5, 6, 7}, {0, 0, 1}};
    p.gap = 2;
    x = fx::placementTransform(p);
    CHECK(x.m[0] == 1.0);
    CHECK(x.m[5] == 1.0);
    CHECK(x.m[10] == 1.0);
    c = applyX(x, {0, 0, 0});
    CHECK(c.x == 5);
    CHECK(c.y == 6);
    CHECK(c.z == 9);
    // A side face onto a top face stands the body on that face: its normal goes to -Z.
    p.face = fx::MateFace{{0, 0, 0}, {1, 0, 0}};
    p.gap = 0;
    x = fx::placementTransform(p);
    const Vec3 n{x.m[0], x.m[4], x.m[8]}; // where +X goes
    CHECK(n.x == 0.0);
    CHECK(n.y == 0.0);
    CHECK(n.z == -1.0);
    // A quarter turn about the target normal is exact, about the target centre.
    p.face = fx::MateFace{{0, 0, 0}, {0, 0, -1}};
    p.angle = kPi / 2;
    p.offset = {1, 2, 3};
    x = fx::placementTransform(p);
    CHECK(x.m[0] == 0.0);
    CHECK(x.m[1] == -1.0);
    CHECK(x.m[4] == 1.0);
    CHECK(x.m[5] == 0.0);
    c = applyX(x, {1, 0, 0});
    CHECK(c.x == Catch::Approx(6).margin(1e-12));
    CHECK(c.y == Catch::Approx(9).margin(1e-12));
    CHECK(c.z == Catch::Approx(10).margin(1e-12));
    // Without a mate: a turn about model Z through the origin, then the offsets.
    fx::Placement free;
    free.angle = kPi;
    free.offset = {10, 0, 0};
    x = fx::placementTransform(free);
    c = applyX(x, {1, 2, 3});
    CHECK(c.x == Catch::Approx(9).margin(1e-12));
    CHECK(c.y == Catch::Approx(-2).margin(1e-12));
    CHECK(c.z == 3.0);
    // Rounding: a normal a hair off an axis is the axis; a centre is whole nanometres.
    const Vec3 r = fx::roundedNormal({1e-12, -1, 3e-13});
    CHECK(r.x == 0.0);
    CHECK(r.y == -1.0);
    CHECK(r.z == 0.0);
    CHECK(fx::roundedPoint({1.0000000004, 0, 0}).x == 1.0);
}

TEST_CASE("kernel::transformed keeps every element's index, so a pick on the moved body names the same element",
          "[doc][transform_body]")
{
    auto box = kernel::makeBox({0, 0, 0}, {30, 10, 15});
    INFO((box ? std::string() : box.error().message));
    REQUIRE(box);
    fx::Placement p;
    p.angle = 0.3;
    p.offset = {5, -7, 2};
    const Xform x = fx::placementTransform(p);
    auto moved = kernel::transformed(*box, x);
    REQUIRE(moved);
    for (std::uint32_t i = 0; i < box->count(kernel::SubKind::Face); ++i) {
        const kernel::SubId sub{kernel::SubKind::Face, i};
        auto a = kernel::surfaceInfo(*box, sub);
        auto b = kernel::surfaceInfo(moved->shape, sub);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(b->area == Catch::Approx(a->area));
        const Vec3 c = applyX(x, a->centroid);
        CHECK(b->centroid.x == Catch::Approx(c.x).margin(1e-9));
        CHECK(b->centroid.y == Catch::Approx(c.y).margin(1e-9));
        CHECK(b->centroid.z == Catch::Approx(c.z).margin(1e-9));
    }
}

TEST_CASE("a body mated to a face of the part sits on it, with gap, turn and offsets, and keeps its names",
          "[doc][transform_body]")
{
    Model m;
    twoBodies(m);
    const BuildResultPtr before = m.build();
    // The fixture's -Y face onto the part's +Y face.
    const kernel::SubId jaw = planeFace(*before, 'c', {0, -1, 0}, -100);
    const kernel::SubId side = planeFace(*before, 'b', {0, 1, 0}, 20);
    const kernel::SubId top = planeFace(*before, 'c', {0, 0, 1}, 15);
    const naming::TopoRef topRef = refTo(*before, 'c', top);
    m.run(addFeature("transform_body", 6,
                     R"({"body":")" + uuid('c', 1) + R"(","mate":)" + mateJson(*before, jaw, side) + "}"));
    CHECK(m.state->features.back().name == "Move Body 1");
    BuildResultPtr b = m.build();
    INFO(report(*b, 6).message);
    REQUIRE(report(*b, 6).status == FeatureStatus::Ok);
    checkBox(*b, 'c', Box3{{5, 20, -2.5}, {35, 30, 12.5}});
    checkBox(*b, 'b', Box3{{0, 0, 0}, {40, 20, 10}}); // the part does not move
    CHECK(volumeOf(*b, 'c') == Catch::Approx(4500.0).epsilon(1e-12));
    // Names survive the move: a reference made before it resolves Exact to the moved top face.
    const naming::Resolved r = resolveOn(*b, 'c', topRef);
    CHECK(r.state == naming::ResolveState::Exact);
    REQUIRE(r.elements.size() == 1);
    auto info = kernel::surfaceInfo(bodyOf(*b, 'c').shape, r.elements[0]);
    REQUIRE(info);
    CHECK(info->centroid.z == Catch::Approx(12.5).margin(1e-9));

    // A gap of 3 and a quarter turn about the part face's normal (+Y) through its centre (20, 20, 5).
    m.run(editFeature("transform_body", 6, R"({"gap":"3 mm","angle":"90 deg"})"));
    b = m.build();
    REQUIRE(report(*b, 6).status == FeatureStatus::Ok);
    checkBox(*b, 'c', Box3{{12.5, 23, -10}, {27.5, 33, 20}});
    // Offsets along the model axes, last.
    m.run(editFeature("transform_body", 6, R"({"x":1,"y":"2 mm","z":"-0.125 in"})"));
    b = m.build();
    checkBox(*b, 'c', Box3{{13.5, 25, -13.175}, {28.5, 35, 16.825}});
    // Clearing the mate (and its gap): the turn is about model Z through the origin, then the offsets.
    m.run(editFeature("transform_body", 6, R"({"mate":null,"gap":null,"angle":"180 deg","x":0,"y":0,"z":0})"));
    b = m.build();
    REQUIRE(report(*b, 6).status == FeatureStatus::Ok);
    checkBox(*b, 'c', Box3{{-130, -110, 0}, {-100, -100, 15}});
}

TEST_CASE("the mate follows an edit of the part", "[doc][transform_body]")
{
    Model m;
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","name":"h","expression":"10 mm","kind":"length"})"));
    twoBodies(m, "\"h\"");
    const BuildResultPtr before = m.build();
    // The fixture's floor onto the part's top: it stands on the part.
    const kernel::SubId floor = planeFace(*before, 'c', {0, 0, -1}, 0);
    const kernel::SubId top = planeFace(*before, 'b', {0, 0, 1}, 10);
    m.run(addFeature("transform_body", 6,
                     R"({"body":")" + uuid('c', 1) + R"(","mate":)" + mateJson(*before, floor, top) + "}"));
    BuildResultPtr b = m.build();
    REQUIRE(report(*b, 6).status == FeatureStatus::Ok);
    checkBox(*b, 'c', Box3{{5, 5, 10}, {35, 15, 25}});
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","expression":"12 mm"})"));
    b = m.build();
    REQUIRE(report(*b, 6).status == FeatureStatus::Ok);
    CHECK_FALSE(report(*b, 6).cacheHit);
    checkBox(*b, 'c', Box3{{5, 5, 12}, {35, 15, 27}});
    // An edit that leaves the part alone reuses the move.
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 2) + R"(","name":"unused","expression":"1 mm","kind":"length"})"));
    b = m.build();
    CHECK(report(*b, 6).cacheHit);
}

TEST_CASE("the move refuses what it cannot do, naming the field", "[doc][transform_body]")
{
    Model m;
    twoBodies(m);
    // A third body: a cylinder (body 'd') for a face that is not flat.
    m.run(addFeature("sketch", 7, sketchProps(kXY, SketchJson().circleAt(1, -50, -50, 5))));
    m.run(addFeature("extrude", 8, extrudeProps(7, "10", "new", 'd')));
    const BuildResultPtr b = m.build();
    const kernel::SubId jaw = planeFace(*b, 'c', {0, -1, 0}, -100);
    const kernel::SubId side = planeFace(*b, 'b', {0, 1, 0}, 20);
    const std::string body = R"({"body":")" + uuid('c', 1) + "\"";
    // The mate's face must be on the body that moves, the target on another body.
    Error e = m.fails(addFeature("transform_body", 6,
                                 body + R"(,"mate":{"face":)" + refJson(refTo(*b, 'b', side)) + R"(,"target":)" +
                                     refJson(refTo(*b, 'b', side)) + "}}"));
    CHECK_THAT(e.message, ContainsSubstring("'mate.face'"));
    e = m.fails(addFeature("transform_body", 6,
                           body + R"(,"mate":{"face":)" + refJson(refTo(*b, 'c', jaw)) + R"(,"target":)" +
                               refJson(refTo(*b, 'c', jaw)) + "}}"));
    CHECK_THAT(e.message, ContainsSubstring("'mate.target'"));
    // A gap without a mate, a body that does not exist, a length that is an angle, an unknown key.
    e = m.fails(addFeature("transform_body", 6, body + R"(,"gap":2})"));
    CHECK_THAT(e.message, ContainsSubstring("'gap'"));
    e = m.fails(addFeature("transform_body", 6, R"({"body":")" + uuid('e', 1) + R"("})"));
    CHECK_THAT(e.message, ContainsSubstring("not in the document"));
    e = m.fails(addFeature("transform_body", 6, body + R"(,"x":"30 deg"})"));
    CHECK_THAT(e.message, ContainsSubstring("x"));
    e = m.fails(addFeature("transform_body", 6, body + R"(,"xform":[1,0,0]})"));
    CHECK_THAT(e.message, ContainsSubstring("'xform'"));
    // A face that is not flat fails the rebuild with a message saying so; the body stays where it was.
    const kernel::Shape& cyl = bodyOf(*b, 'd').shape;
    std::optional<kernel::SubId> round;
    for (std::uint32_t i = 0; i < cyl.count(kernel::SubKind::Face); ++i) {
        auto info = kernel::surfaceInfo(cyl, kernel::SubId{kernel::SubKind::Face, i});
        if (info && info->type == kernel::SurfaceType::Cylinder) {
            round = kernel::SubId{kernel::SubKind::Face, i};
        }
    }
    REQUIRE(round);
    m.run(addFeature("transform_body", 6,
                     body + R"(,"mate":{"face":)" + refJson(refTo(*b, 'c', jaw)) + R"(,"target":)" +
                         refJson(refTo(*b, 'd', *round)) + "}}"));
    const BuildResultPtr failed = m.build();
    CHECK(report(*failed, 6).status == FeatureStatus::Failed);
    CHECK_THAT(report(*failed, 6).message, ContainsSubstring("not flat"));
    checkBox(*failed, 'c', Box3{{100, 100, 0}, {130, 110, 15}});
}

TEST_CASE("a move is undone, redone, saved and reloaded like any other feature", "[doc][transform_body]")
{
    DocEnv env;
    env.run(addFeature("sketch", 2, sketchProps(kXY, SketchJson().rect(1, 0, 0, 40, 20))));
    env.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
    env.run(addFeature("sketch", 4, sketchProps(kXY, SketchJson().rect(1, 100, 100, 130, 110))));
    env.run(addFeature("extrude", 5, extrudeProps(4, "15", "new", 'c')));
    REQUIRE(env.settle());
    const BuildResultPtr first = env.doc->build();
    const DocStatePtr unmoved = env.doc->state();
    const kernel::SubId jaw = planeFace(*first, 'c', {0, -1, 0}, -100);
    const kernel::SubId side = planeFace(*first, 'b', {0, 1, 0}, 20);
    env.run(addFeature("transform_body", 6,
                       R"({"body":")" + uuid('c', 1) + R"(","mate":)" + mateJson(*first, jaw, side) +
                           R"(,"gap":"0.5 in"})"));
    REQUIRE(env.settle());
    checkBox(*env.doc->build(), 'c', Box3{{5, 32.7, -2.5}, {35, 42.7, 12.5}});
    const std::string moved = fingerprint(*env.doc->state(), *env.doc->build());
    REQUIRE(env.doc->undo());
    REQUIRE(env.settle());
    CHECK(env.doc->state() == unmoved);
    checkBox(*env.doc->build(), 'c', Box3{{100, 100, 0}, {130, 110, 15}});
    REQUIRE(env.doc->redo());
    REQUIRE(env.settle());
    CHECK(fingerprint(*env.doc->state(), *env.doc->build()) == moved);

    // Saved and loaded: the same snapshot hash, and a fresh rebuilder makes the same bodies and names.
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
    RebuildEnv fresh(2);
    CHECK(fingerprint(*reloaded, *fresh.rebuild(reloaded)) == moved);
}

TEST_CASE("documents from before the move type open, save and build unchanged", "[doc][transform_body]")
{
    // golden/old_toporef is a document saved by older code (parametric_test.cpp reads its geometry); with the move
    // type registered it reads, writes back the same bytes and builds every feature, none of which is a move.
    const std::string dir = std::string(RAY_DOC_GOLDEN_DIR) + "/old_toporef";
    const std::string manifest = read(dir + "/manifest.json");
    REQUIRE(!manifest.empty());
    auto state = deserializeState(manifest, [&](const Hash256& h) -> Result<std::string> {
        const std::string bytes = read(dir + "/" + h.hex() + ".json");
        if (bytes.empty()) {
            return tl::unexpected(Error{ErrorCode::NotFound, "missing object " + h.hex(), {}});
        }
        return bytes;
    });
    REQUIRE(state);
    const auto loaded = std::make_shared<const DocState>(std::move(*state));
    CHECK(serializeState(*loaded).manifest == manifest);
    for (const Feature& f : loaded->features) {
        CHECK(f.type != fx::kTransformBodyType);
    }
    REQUIRE(builtinFeatureRegistry()->find(fx::kTransformBodyType) != nullptr);
    RebuildEnv env(2);
    const BuildResultPtr b = env.rebuild(loaded);
    for (const Feature& f : loaded->features) {
        CHECK(reportOf(*b, f.id).status == FeatureStatus::Ok);
    }
}
