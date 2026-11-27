// A library vise placed on a setup's stock (after task T-077; ray_api/cam_fixtures.h).
//
// The arithmetic: the shipped descriptions agree with the shapes the shipped models are (fixture_models.h); the fit
// across X and across Y, the shorter side by default, with and without parallels, a stock wider than the vise opens;
// the turn and offsets of a move without a mate (rotations about Z, a part turned over, a side setup) give back the
// move through the feature's own arithmetic.
// A real document: a 100 x 60 x 20 block, the shipped vise and parallels imported as fixtures, a setup with a box stock
// grown by 2, 3, 1, 4, 0 and 1 mm; the planned commands applied and rebuilt, each body measured in the setup's WCS:
// across Y on the parallels and across X on the floor; the same stand with the setup's WCS turned 30 degrees about Z
// (a turned block with its X edge), with the part turned over (flipZ: the vise upside down in the model) and on its
// side (Z face the block's +X face); run again after the stock changed (the placements edited, not added); a stock
// wider than the vise opens (the jaw at its widest) and narrower than the parallels (refused).
// The safety checks (src/sim/verify.h) see the placed vise: a pocket near the stock's front side, the vise closed on
// the stock standing on parallels: no event at all (the jaws touching the stock, the stock on the parallels are not
// collisions; since the setup checks of T-074's follow-up test fixtures against the part and the stock too, touching
// is still none); on the vise's floor, the jaws 24.45 above the stock, the
// holder at the front wall overhangs the moving jaw: HolderHitsFixture on it. The fixture solids the simulator gets are
// where the placement put them.
// After T-077: every shipped parallels description agrees with its bars and stands at least kMinGripMm below the jaws'
// top; the fit on each pair; the taller pair suggested for thin stock (tallerParallels); each taller pair placed by the
// action under the block raises no setup event and no motion event.
#include "engine_test_util.h"
#include "fixture_models.h"
#include "parametric_test_util.h"

#include "features/transform_body_feature.h"
#include "naming/naming_internal.h"
#include "naming/naming_testing.h"
#include "sim/verify.h"

#include <ray/cam.h>
#include <ray/sim.h>
#include <ray_api/cam_fixtures.h>
#include <ray_api/cam_setup.h>
#include <ray_api/cam_sim.h>
#include <ray_api/feature_edit.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <thread>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
namespace dt = ray::doc::test;
namespace ct = ray::cam::test;
namespace cf = ray::api::cam_fixtures;
namespace fm = ray::cam::test::fixtures;
namespace fx = ray::doc::features;

namespace {

constexpr double kPi = 3.14159265358979323846;
const std::string kDir = std::string(RAY_SOURCE_DIR) + "/resources/library/fixtures/";
const std::string kXY = R"({"principal":"xy"})";

std::string readText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

cf::FixtureModel shipped(const std::string& name)
{
    auto m = cf::parseFixtureModel(readText(kDir + name));
    INFO(name << ": " << (m ? std::string() : m.error().message));
    REQUIRE(m);
    return *m;
}

Vec3 apply(const Xform& x, Vec3 p)
{
    return Vec3{x.m[0] * p.x + x.m[1] * p.y + x.m[2] * p.z + x.m[3],
                x.m[4] * p.x + x.m[5] * p.y + x.m[6] * p.z + x.m[7],
                x.m[8] * p.x + x.m[9] * p.y + x.m[10] * p.z + x.m[11]};
}

// The box of `box` moved by `x` (its eight corners).
Box3 moved(const Box3& box, const Xform& x)
{
    Box3 out{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
    for (int k = 0; k < 8; ++k) {
        const Vec3 p =
            apply(x, Vec3{k & 1 ? box.max.x : box.min.x, k & 2 ? box.max.y : box.min.y, k & 4 ? box.max.z : box.min.z});
        out.min = Vec3{std::min(out.min.x, p.x), std::min(out.min.y, p.y), std::min(out.min.z, p.z)};
        out.max = Vec3{std::max(out.max.x, p.x), std::max(out.max.y, p.y), std::max(out.max.z, p.z)};
    }
    return out;
}

Xform shiftY(double dy)
{
    Xform x = Xform::identity();
    x.m[7] = dy;
    return x;
}

Xform then(const Xform& a, const Xform& b) // a after b
{
    Xform out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            out.m[i * 4 + j] = a.m[i * 4] * b.m[j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j] +
                               (j == 3 ? a.m[i * 4 + 3] : 0.0);
        }
    }
    return out;
}

void checkBox(const Box3& got, const Box3& want, const char* what, double tol = 2e-5)
{
    INFO(what << ": got " << got.min.x << " " << got.min.y << " " << got.min.z << " .. " << got.max.x << " "
              << got.max.y << " " << got.max.z << "; want " << want.min.x << " " << want.min.y << " " << want.min.z
              << " .. " << want.max.x << " " << want.max.y << " " << want.max.z);
    CHECK(got.min.x == Catch::Approx(want.min.x).margin(tol));
    CHECK(got.min.y == Catch::Approx(want.min.y).margin(tol));
    CHECK(got.min.z == Catch::Approx(want.min.z).margin(tol));
    CHECK(got.max.x == Catch::Approx(want.max.x).margin(tol));
    CHECK(got.max.y == Catch::Approx(want.max.y).margin(tol));
    CHECK(got.max.z == Catch::Approx(want.max.z).margin(tol));
}

// The library boxes of the shipped shapes.
Box3 boxOf(const kernel::Shape& s)
{
    return kernel::bounds(s);
}

// ---------- A document with the part, the vise and the parallels ----------
struct Shop {
    dt::RebuildEnv env{4};
    doc::DocStatePtr state = std::make_shared<const doc::DocState>();
    doc::BuildResultPtr build;
    BodyId vise, jaw, parA, parB;
    std::uint64_t nextFeature = 100;

    void run(const doc::Command& c) { state = ct::exec(state, c); }
    void rebuild()
    {
        build = env.rebuild(state);
        for (const doc::FeatureReport& r : build->reports) {
            INFO(r.message);
            REQUIRE(r.status == doc::FeatureStatus::Ok);
        }
    }
    void import(const std::string& file, std::uint64_t feature)
    {
        doc::ImportRequest request;
        request.feature = dt::featureId(feature);
        request.path = kDir + file;
        request.role = doc::Body::Role::Fixture;
        JobContext ctx;
        auto prepared = doc::prepareImport(request, ctx);
        REQUIRE(prepared);
        for (const auto& [hash, bytes] : prepared->objects) {
            REQUIRE(env.pool->put(bytes));
        }
        run(prepared->command);
    }
    BodyId named(const std::string& name) const
    {
        for (const doc::Body& b : state->bodies) {
            if (b.name == name) {
                return b.id;
            }
        }
        FAIL_CHECK("no body " << name);
        return {};
    }
    naming::TopoRef ref(const BodyId& body, kernel::SubId sub) const
    {
        const doc::BuiltBody& b = build->bodies.at(body);
        auto resolver = naming::makeResolver({naming::ResolverBody{body, b.map, b.shape}});
        return resolver->makeRef(body, sub);
    }
    // The planar face of `body` looking along `n` whose centroid lies at `along` on it.
    kernel::SubId face(const BodyId& body, Vec3 n, double along) const
    {
        const kernel::Shape& shape = build->bodies.at(body).shape;
        for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
            const kernel::SubId sub{kernel::SubKind::Face, i};
            auto s = kernel::faceSample(shape, sub);
            auto info = kernel::surfaceInfo(shape, sub);
            if (s && info && info->type == kernel::SurfaceType::Plane && dt::near(s->normal.x, n.x, 1e-9) &&
                dt::near(s->normal.y, n.y, 1e-9) && dt::near(s->normal.z, n.z, 1e-9) &&
                dt::near(info->centroid.x * n.x + info->centroid.y * n.y + info->centroid.z * n.z, along, 1e-6)) {
                return sub;
            }
        }
        FAIL_CHECK("no such face");
        return {};
    }
    int placements() const
    {
        int n = 0;
        for (const doc::Feature& f : state->features) {
            n += f.type == "transform_body" ? 1 : 0;
        }
        return n;
    }
    // The part: a block (sketch 2, extrude 3, body 'b'), 100 x 60 x 20, turned `turnDeg` about Z at the origin; the
    // vise (feature 7) and the parallels (feature 8) imported as fixtures.
    void make(double turnDeg = 0)
    {
        const double c = std::cos(turnDeg * kPi / 180), s = std::sin(turnDeg * kPi / 180);
        const auto at = [&](double x, double y) {
            return std::pair<double, double>{c * x - s * y, s * x + c * y};
        };
        dt::SketchJson sk;
        const std::pair<double, double> p[] = {at(0, 0), at(100, 0), at(100, 60), at(0, 60)};
        for (int i = 0; i < 4; ++i) {
            sk.point(1 + i, p[i].first, p[i].second);
        }
        sk.line(5, 1, 2).line(6, 2, 3).line(7, 3, 4).line(8, 4, 1);
        run(dt::addFeature("sketch", 2, dt::sketchProps(kXY, sk)));
        run(dt::addFeature("extrude", 3, dt::extrudeProps(2, "20", "new", 'b')));
        import("generic-vise-6in.step", 7);
        import("generic-parallels-1x6in.step", 8);
        vise = named("Generic 6 in vise");
        jaw = named("Generic 6 in vise moving jaw");
        parA = named("Generic parallel 1/8 x 1 x 6 in A");
        parB = named("Generic parallel 1/8 x 1 x 6 in B");
        rebuild();
        run(ct::command("cam.library.embed", ct::library(ct::toolJson(1), ct::holderJson(1), ct::machineJson(1))));
    }
    // Setup 'a' n on the block, its stock grown as the header says, `wcs` its WCS JSON ("" the default).
    void addSetup(std::uint64_t n, const std::string& fixtures, const std::string& wcs = "",
                  const std::string& stock = R"({"type":"box","xNeg":2,"xPos":3,"yNeg":1,"yPos":4,"zNeg":0,"zPos":1})")
    {
        run(ct::setupAdd(n, "\"" + ct::body('b').value.toString() + "\"",
                         R"(,"fixtureBodies":[)" + fixtures + R"(],"stock":)" + stock +
                             (wcs.empty() ? std::string() : R"(,"wcs":)" + wcs)));
    }
    std::string ids(std::initializer_list<BodyId> list) const
    {
        std::string s;
        for (const BodyId& b : list) {
            s += (s.empty() ? "\"" : ",\"") + b.value.toString() + "\"";
        }
        return s;
    }
    cam::ResolvedSetup resolved(std::uint64_t setup) const
    {
        JobContext ctx;
        auto p = api::cam_setup::previewSetup(state, build, SetupId{Uuid::fromString(ct::id('a', setup))}, ctx);
        INFO((p ? std::string() : p.error().message));
        REQUIRE(p);
        return p->resolved;
    }
    cf::PlaceRequest request(std::uint64_t setup, cf::Grip grip, bool parallels)
    {
        const cam::ResolvedSetup r = resolved(setup);
        cf::PlaceRequest rq;
        rq.state = state;
        rq.build = build;
        rq.modelFromWcs = r.modelFromWcs;
        rq.stock = r.stockBounds;
        rq.setup = SetupId{Uuid::fromString(ct::id('a', setup))};
        rq.viseModel = shipped("generic-vise-6in.json").vise;
        rq.vise = vise;
        rq.jaw = jaw;
        if (parallels) {
            rq.parallelsModel = shipped("generic-parallels-1x6in.json").parallels;
            rq.parallels = {parA, parB};
        }
        rq.grip = grip;
        rq.buildBefore = [this](const FeatureId& f) -> Result<doc::BuildResultPtr> {
            const doc::DocStatePtr before = api::feature_edit::stateBefore(*state, f);
            REQUIRE(before);
            return env.rebuild(before);
        };
        rq.newFeatureId = [this] {
            return dt::featureId(nextFeature++);
        };
        return rq;
    }
    // Applies the batch's commands one by one (cam.batch is the app's; the document's registry has each of them).
    void apply(const cf::PlacePlan& plan)
    {
        auto batch = doc::json::parse(plan.batchArgsJson);
        REQUIRE(batch);
        const doc::json::Value* list = batch->find("commands");
        REQUIRE(list != nullptr);
        for (const doc::json::Value& c : list->asArray()) {
            run(ct::command(c.find("name")->asString(), doc::json::write(*c.find("args"))));
        }
        rebuild();
    }
    cf::PlacePlan place(std::uint64_t setup, cf::Grip grip, bool parallels)
    {
        auto plan = cf::planViseOnStock(request(setup, grip, parallels));
        INFO((plan ? std::string() : plan.error().message));
        REQUIRE(plan);
        apply(*plan);
        return *plan;
    }
    Box3 inWcs(const BodyId& body, const cam::ResolvedSetup& r) const
    {
        auto b = kernel::boundsIn(build->bodies.at(body).shape, r.wcsFromModel);
        REQUIRE(b);
        return *b;
    }
};

// The library boxes of the shipped shapes (fixture_models.h builds them).
struct Library {
    Box3 vise, jaw, parA, parB;
    Library()
    {
        const auto v = fm::vise();
        const auto p = fm::parallels();
        REQUIRE(v.size() == 2);
        REQUIRE(p.size() == 2);
        vise = boxOf(v[0].shape);
        jaw = boxOf(v[1].shape);
        parA = boxOf(p[0].shape);
        parB = boxOf(p[1].shape);
    }
};

// Every body where the fit says it goes, in the setup's WCS.
void checkStand(const Shop& shop, const cam::ResolvedSetup& r, const cf::ViseFit& fit, bool parallels)
{
    const Library lib;
    checkBox(shop.inWcs(shop.vise, r), moved(lib.vise, fit.wcsFromVise), "vise");
    checkBox(shop.inWcs(shop.jaw, r), moved(lib.jaw, then(fit.wcsFromVise, shiftY(fit.opening - fm::kOpening))),
             "moving jaw");
    if (parallels) {
        checkBox(shop.inWcs(shop.parA, r), moved(lib.parA, fit.wcsFromVise), "parallel A");
        checkBox(shop.inWcs(shop.parB, r), moved(lib.parB, then(fit.wcsFromVise, shiftY(fit.opening - fm::kOpening))),
                 "parallel B");
    }
}

// The first stand of the header (across Y, on the parallels), as numbers: the stock is x 0..105, y 0..65, z -21..0 in
// the WCS (corner 0 at the origin).
void checkFirstStand(const Shop& shop, const cam::ResolvedSetup& r)
{
    const Box3 jaw = shop.inWcs(shop.jaw, r);
    const Box3 a = shop.inWcs(shop.parA, r);
    const Box3 b = shop.inWcs(shop.parB, r);
    const Box3 v = shop.inWcs(shop.vise, r);
    // The moving jaw closed on the stock's -Y face, centred along X, its top 44.45 above the floor 25.4 below the
    // stock.
    checkBox(jaw, Box3{{52.5 - 76.2, -60, -46.4}, {52.5 + 76.2, 0, -1.95}}, "moving jaw (numbers)");
    // The parallels under the stock, one against each jaw, their tops on the stock's bottom.
    checkBox(a, Box3{{52.5 - 76.2, 65 - 3.175, -46.4}, {52.5 + 76.2, 65, -21}}, "parallel A (numbers)");
    checkBox(b, Box3{{52.5 - 76.2, 0, -46.4}, {52.5 + 76.2, 3.175, -21}}, "parallel B (numbers)");
    // The vise's back 60 behind its fixed jaw's face on the stock's +Y face; its screw out at the front.
    CHECK(v.max.y == Catch::Approx(65 + 60).margin(2e-5));
    CHECK(v.max.z == Catch::Approx(-1.95).margin(2e-5));
    CHECK((v.min.x + v.max.x) / 2 == Catch::Approx(52.5).margin(2e-5));
}

// The body a transform_body feature moves.
std::string movedBody(const doc::Feature& f)
{
    auto props = doc::json::parse(f.propsJson);
    const doc::json::Value* body = props ? props->find("body") : nullptr;
    return f.type == "transform_body" && body != nullptr ? body->asString() : std::string();
}

std::string propsOf(const doc::DocState& state, const BodyId& body)
{
    for (const doc::Feature& f : state.features) {
        if (movedBody(f) == body.value.toString()) {
            return f.propsJson;
        }
    }
    return {};
}

} // namespace

TEST_CASE("the shipped vise and parallels descriptions agree with the shipped shapes", "[cam][fixtures][place]")
{
    const cf::FixtureModel v = shipped("generic-vise-6in.json");
    REQUIRE(v.kind == cf::FixtureModel::Kind::Vise);
    CHECK(v.vise.body == "Generic 6 in vise");
    CHECK(v.vise.movingJaw == "Generic 6 in vise moving jaw");
    CHECK(v.vise.jawLength == Catch::Approx(fm::kJawWidth).margin(1e-9));
    CHECK(v.vise.jawHeight == Catch::Approx(fm::kJawHeight).margin(1e-9));
    // The widest the moving jaw opens: its back at the front end of the base.
    CHECK(v.vise.maxOpening == fm::kFrontY - fm::kJawThickness);
    const cf::FixtureModel p = shipped("generic-parallels-1x6in.json");
    REQUIRE(p.kind == cf::FixtureModel::Kind::Parallels);
    CHECK(p.parallels.bodies == std::vector<std::string>{fm::parallels()[0].name, fm::parallels()[1].name});
    CHECK(p.parallels.height == Catch::Approx(fm::kParallelHeight).margin(1e-9));
    CHECK(p.parallels.thickness == Catch::Approx(fm::kParallelThickness).margin(1e-9));
    CHECK(fm::vise()[0].name == v.vise.body);
    CHECK(fm::vise()[1].name == v.vise.movingJaw);
    // What is wrong with a description is said.
    auto bad = cf::parseFixtureModel(R"({"schema":1,"kind":"vise","body":"a","movingJaw":"b","jawLength":1})");
    REQUIRE_FALSE(bad);
    CHECK_THAT(bad.error().message, ContainsSubstring("jawHeight"));
    CHECK_FALSE(cf::parseFixtureModel(R"({"schema":2,"kind":"vise"})"));
    CHECK_FALSE(cf::parseFixtureModel(R"({"schema":1,"kind":"clamp"})"));
}

// After T-077: the taller pairs. Each description names the bodies its STEP file has and gives their height and
// thickness; the heights are the library's steps, 1/8 in apart, each below the jaws' top by at least kMinGripMm.
TEST_CASE("every shipped parallels description agrees with its shapes, and stands below the jaws' top",
          "[cam][fixtures][place][parallels]")
{
    const cf::ViseModel vise = shipped("generic-vise-6in.json").vise;
    std::vector<double> heights;
    for (const fm::ParallelsFile& file : fm::kParallelsFiles) {
        const std::string json =
            std::string(file.fileName).replace(std::string(file.fileName).rfind(".step"), 5, ".json");
        INFO(json);
        const cf::FixtureModel m = shipped(json);
        REQUIRE(m.kind == cf::FixtureModel::Kind::Parallels);
        const std::vector<fm::NamedShape> shapes = file.make();
        REQUIRE(shapes.size() == 2);
        CHECK(m.parallels.bodies == std::vector<std::string>{shapes[0].name, shapes[1].name});
        CHECK(shapes[0].name == "Generic parallel 1/8 x " + fm::inchesText(file.eighths) + " x 6 in A");
        CHECK(m.parallels.height == Catch::Approx(file.eighths * fm::kIn / 8).margin(1e-9));
        CHECK(m.parallels.thickness == Catch::Approx(fm::kParallelThickness).margin(1e-9));
        for (const fm::NamedShape& s : shapes) {
            // Plain bars, 1/8 x height x 6 in, on the floor against the jaws (fixture_models.h).
            const Box3 b = boxOf(s.shape);
            CHECK(b.max.x - b.min.x == Catch::Approx(fm::kParallelLength).margin(1e-6));
            CHECK(b.max.y - b.min.y == Catch::Approx(m.parallels.thickness).margin(1e-6));
            CHECK(b.min.z == Catch::Approx(0).margin(1e-6));
            CHECK(b.max.z == Catch::Approx(m.parallels.height).margin(1e-6));
            CHECK(s.shape.count(kernel::SubKind::Face) == 6);
        }
        CHECK(m.parallels.height <= vise.jawHeight - cf::kMinGripMm);
        heights.push_back(m.parallels.height);
    }
    CHECK(heights == std::vector<double>{25.4, 31.75, 38.1, 41.275});
    CHECK(fm::inchesText(8) == "1");
    CHECK(fm::inchesText(10) == "1-1/4");
    CHECK(fm::inchesText(12) == "1-1/2");
    CHECK(fm::inchesText(13) == "1-5/8");
    // 1-3/4 in would be level with the jaws' top: nothing for them to hold.
    CHECK(14 * fm::kIn / 8 == Catch::Approx(vise.jawHeight).margin(1e-9));
}

// The fit on each pair, and the pair Place on stock suggests (cf::tallerParallels) when the stock's top ends below the
// jaws': at least kMinGripMm held, the top at or above the jaws' where a pair allows it, the lowest such pair.
TEST_CASE("the fit on each pair, and the taller pair suggested for thin stock", "[cam][fixtures][place][parallels]")
{
    const cf::ViseModel vise = shipped("generic-vise-6in.json").vise;
    std::vector<cf::ParallelsModel> pairs;
    for (const fm::ParallelsFile& file : fm::kParallelsFiles) {
        const std::string json =
            std::string(file.fileName).replace(std::string(file.fileName).rfind(".step"), 5, ".json");
        pairs.push_back(shipped(json).parallels);
    }
    REQUIRE(pairs.size() == 4);
    // 10 mm plate, 120 x 80: on each pair the floor is the pair's height below the stock; the jaws hold what is left of
    // 44.45 above the floor.
    const Box3 plate{{0, 0, -10}, {120, 80, 0}};
    const double want[][2] = {// jaws above the stock's top, held
                              {44.45 - 25.4 - 10, 10},
                              {44.45 - 31.75 - 10, 10},
                              {44.45 - 38.1 - 10, 44.45 - 38.1},
                              {44.45 - 41.275 - 10, 44.45 - 41.275}};
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        INFO("pair " << pairs[i].height);
        const cf::ViseFit f = cf::fitVise(plate, cf::Grip::Shorter, vise, &pairs[i]);
        CHECK(f.floorZ == Catch::Approx(-10 - pairs[i].height));
        CHECK(f.jawAboveStock == Catch::Approx(want[i][0]));
        CHECK(f.gripDepth == Catch::Approx(want[i][1]));
    }

    // On the 1 in pair the jaws stand 9.05 above the plate: the 1-1/2 in pair puts its top 3.65 above the jaws with
    // 6.35 held (the 1-5/8 in would too, holding less; the 1-1/4 in leaves it 2.7 below).
    cf::ViseFit f = cf::fitVise(plate, cf::Grip::Shorter, vise, &pairs[0]);
    auto s = cf::tallerParallels(f, vise, &pairs[0], pairs);
    REQUIRE(s);
    CHECK(s->index == 2);
    CHECK(s->aboveJaws);
    CHECK(s->gripDepth == Catch::Approx(6.35));
    CHECK(s->jawAboveStock == Catch::Approx(-3.65));
    // The same from the floor (no parallels), and from the 1-1/4 in pair.
    s = cf::tallerParallels(cf::fitVise(plate, cf::Grip::Shorter, vise, nullptr), vise, nullptr, pairs);
    REQUIRE(s);
    CHECK(s->index == 2);
    s = cf::tallerParallels(cf::fitVise(plate, cf::Grip::Shorter, vise, &pairs[1]), vise, &pairs[1], pairs);
    REQUIRE(s);
    CHECK(s->index == 2);
    // On the suggested pair the top is above the jaws: nothing more to suggest.
    CHECK_FALSE(cf::tallerParallels(cf::fitVise(plate, cf::Grip::Shorter, vise, &pairs[2]), vise, &pairs[2], pairs));

    // 5 mm sheet: only the 1-5/8 in pair holds 3 mm with the top above the jaws (3.175 held, top 1.825 above).
    const Box3 sheet{{0, 0, -5}, {120, 80, 0}};
    s = cf::tallerParallels(cf::fitVise(sheet, cf::Grip::Shorter, vise, &pairs[0]), vise, &pairs[0], pairs);
    REQUIRE(s);
    CHECK(s->index == 3);
    CHECK(s->aboveJaws);
    CHECK(s->gripDepth == Catch::Approx(3.175));
    // 3.1 mm sheet: no pair puts its top above the jaws with 3 mm held; the 1-5/8 in brings it nearest (0.075 below),
    // holding 3.1.
    const Box3 thin{{0, 0, -3.1}, {120, 80, 0}};
    s = cf::tallerParallels(cf::fitVise(thin, cf::Grip::Shorter, vise, &pairs[0]), vise, &pairs[0], pairs);
    REQUIRE(s);
    CHECK(s->index == 3);
    CHECK_FALSE(s->aboveJaws);
    CHECK(s->jawAboveStock == Catch::Approx(44.45 - 41.275 - 3.1));
    CHECK(s->gripDepth == Catch::Approx(3.1));
    // 2 mm sheet: no pair holds 3 mm of it.
    const Box3 foil{{0, 0, -2}, {120, 80, 0}};
    CHECK_FALSE(cf::tallerParallels(cf::fitVise(foil, cf::Grip::Shorter, vise, &pairs[0]), vise, &pairs[0], pairs));
    // A stock whose top is above the jaws already: nothing.
    const Box3 block{{0, 0, -30}, {120, 80, 0}};
    CHECK_FALSE(cf::tallerParallels(cf::fitVise(block, cf::Grip::Shorter, vise, &pairs[0]), vise, &pairs[0], pairs));
    // On the tallest pair already: none is taller.
    CHECK_FALSE(cf::tallerParallels(cf::fitVise(thin, cf::Grip::Shorter, vise, &pairs[3]), vise, &pairs[3], pairs));
    // A stock narrower than two parallels side by side gets none.
    const Box3 strip{{0, 0, -10}, {120, 6, 0}};
    CHECK_FALSE(cf::tallerParallels(cf::fitVise(strip, cf::Grip::Shorter, vise, nullptr), vise, nullptr, pairs));
}

TEST_CASE("the fit: the grip, the floor, the parallels, the opening and its limit", "[cam][fixtures][place]")
{
    const cf::ViseModel vise = shipped("generic-vise-6in.json").vise;
    const cf::ParallelsModel par = shipped("generic-parallels-1x6in.json").parallels;
    // A stock 105 x 65 x 21 in the WCS, top at z = 0.
    const Box3 stock{{0, 0, -21}, {105, 65, 0}};

    // Across Y (the shorter, by default), on the floor: the fixed jaw's face on y = 65 looking -Y, the vise's X along
    // -X, centred on x = 52.5; the floor at the stock's bottom; the jaws above the stock's top.
    cf::ViseFit f = cf::fitVise(stock, cf::Grip::Shorter, vise, nullptr);
    CHECK_FALSE(f.acrossX);
    CHECK(f.across == 65);
    CHECK(f.along == 105);
    CHECK(f.opening == 65);
    CHECK_FALSE(f.tooWide);
    CHECK(f.floorZ == -21);
    CHECK(f.height == 21);
    CHECK(f.gripDepth == 21);
    CHECK(f.jawAboveStock == Catch::Approx(44.45 - 21));
    const Vec3 origin = apply(f.wcsFromVise, {0, 0, 0});
    CHECK(origin.x == 52.5);
    CHECK(origin.y == 65);
    CHECK(origin.z == -21);
    const Vec3 opens = apply(f.wcsFromVise, {0, 1, 0});
    CHECK(opens.y == 64); // the moving jaw opens towards -Y
    // The moving jaw's face, the opening from the fixed one, lies on the stock's -Y face.
    CHECK(apply(f.wcsFromVise, {0, f.opening, 0}).y == 0);

    // On the parallels: the floor a parallel's height below the stock; the jaws hold 44.45 - 25.4 of it.
    f = cf::fitVise(stock, cf::Grip::AcrossY, vise, &par);
    CHECK(f.floorZ == Catch::Approx(-21 - 25.4));
    CHECK(f.gripDepth == Catch::Approx(44.45 - 25.4));
    CHECK(f.jawAboveStock == Catch::Approx(44.45 - 25.4 - 21));

    // Across X: the fixed jaw on x = 0 looking +X, opening towards +X, centred on y = 32.5.
    f = cf::fitVise(stock, cf::Grip::AcrossX, vise, &par);
    CHECK(f.acrossX);
    CHECK(f.across == 105);
    CHECK(f.along == 65);
    CHECK(f.opening == 105);
    const Vec3 o2 = apply(f.wcsFromVise, {0, 0, 0});
    CHECK(o2.x == 0);
    CHECK(o2.y == 32.5);
    CHECK(apply(f.wcsFromVise, {0, f.opening, 0}).x == 105);
    CHECK(apply(f.wcsFromVise, {1, 0, 0}).y == 31.5); // the jaws run along -Y

    // The shorter side is X when X is shorter; Y when they are equal.
    CHECK(cf::fitVise(Box3{{0, 0, 0}, {40, 50, 10}}, cf::Grip::Shorter, vise, nullptr).acrossX);
    CHECK_FALSE(cf::fitVise(Box3{{0, 0, 0}, {50, 50, 10}}, cf::Grip::Shorter, vise, nullptr).acrossX);

    // Wider than the vise opens: the opening is the vise's widest, and that is said.
    f = cf::fitVise(Box3{{0, 0, -10}, {300, 400, 0}}, cf::Grip::Shorter, vise, nullptr);
    CHECK(f.across == 300);
    CHECK(f.tooWide);
    CHECK(f.opening == 270);
    // Taller parallels than jaws would hold nothing.
    cf::ParallelsModel tall = par;
    tall.height = 50;
    CHECK(cf::fitVise(stock, cf::Grip::Shorter, vise, &tall).gripDepth < 0);
}

TEST_CASE("a move without a mate: the turn and tilts that give back the rotation", "[cam][fixtures][place]")
{
    const auto check = [](const Xform& want, double tiltX, double tiltY, double angle) {
        const cf::FreeMove m = cf::freeMove(want);
        CHECK(m.tiltX == Catch::Approx(tiltX * kPi / 180).margin(1e-12));
        CHECK(m.tiltY == Catch::Approx(tiltY * kPi / 180).margin(1e-12));
        CHECK(m.angle == Catch::Approx(angle * kPi / 180).margin(1e-12));
        fx::Placement p;
        p.tiltX = m.tiltX;
        p.tiltY = m.tiltY;
        p.angle = m.angle;
        p.offset = m.offset;
        const Xform got = fx::placementTransform(p);
        for (int i = 0; i < 12; ++i) {
            CHECK(got.m[i] == Catch::Approx(want.m[i]).margin(1e-12));
        }
    };
    // Square to the model: no turn; half a turn about Z; a quarter turn the other way.
    check(Xform{{1, 0, 0, 5, 0, 1, 0, 6, 0, 0, 1, 7}}, 0, 0, 0);
    check(Xform{{-1, 0, 0, 5, 0, -1, 0, 6, 0, 0, 1, 7}}, 0, 0, 180);
    check(Xform{{0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0}}, 0, 0, -90);
    // 30 degrees about Z: no tilt.
    const double c = std::cos(kPi / 6), s = std::sin(kPi / 6);
    check(Xform{{c, -s, 0, 1, s, c, 0, 2, 0, 0, 1, 3}}, 0, 0, 30);
    // Turned over (Z down, X kept): half a turn about X; exact, as the feature makes it.
    check(Xform{{1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 0}}, 180, 0, 0);
    // On its side, Z onto model +X: a quarter turn about Y.
    check(Xform{{0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0}}, 0, 90, 0);
    // X onto model Z (the turns about X and Z are one): all of it in the angle.
    check(Xform{{0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0}}, 0, -90, 0);
    // The texts the feature stores.
    CHECK(cf::angleText(kPi) == "180 deg");
    CHECK(cf::angleText(-kPi / 2) == "-90 deg");
    CHECK(cf::angleText(1e-14) == "0 deg");
    CHECK(cf::lengthText(12.7, "mm") == "12.7 mm");
    CHECK(cf::lengthText(12.7, "in") == "0.5 in");
    CHECK(cf::lengthText(-46.4000000001, "mm") == "-46.4 mm");
    CHECK(cf::lengthText(-0.0000001, "mm") == "0 mm");
}

TEST_CASE("the vise stands the stock on the parallels or its floor, the moving jaw closed on it",
          "[cam][fixtures][place]")
{
    Shop shop;
    shop.make();
    // The setup has the parallels as fixtures, not the vise: placing makes the vise and its jaw fixtures of it.
    shop.addSetup(1, shop.ids({shop.parA, shop.parB}));
    shop.rebuild();
    const cam::ResolvedSetup r = shop.resolved(1);
    CHECK(r.stockBounds.max.x == Catch::Approx(105).margin(1e-6));
    CHECK(r.stockBounds.max.y == Catch::Approx(65).margin(1e-6));
    CHECK(r.stockBounds.min.z == Catch::Approx(-21).margin(1e-6));

    // ---- Across Y (the shorter side), on the parallels ----
    const cf::PlacePlan plan = shop.place(1, cf::Grip::Shorter, true);
    CHECK_FALSE(plan.fit.acrossX);
    CHECK(plan.fit.opening == Catch::Approx(65).margin(1e-9));
    CHECK(plan.addedToSetup == std::vector<BodyId>{shop.vise, shop.jaw});
    CHECK(shop.state->cam->setups.front().fixtureBodies ==
          std::vector<BodyId>{shop.parA, shop.parB, shop.vise, shop.jaw});
    CHECK(shop.placements() == 4);
    checkFirstStand(shop, r);
    checkStand(shop, r, plan.fit, true);
    // Ordinary placements: the vise and parallels by numbers (no tilt, the WCS Z is model +Z), the jaw by its mate to
    // the fixed jaw with the opening as the gap.
    const std::string viseProps = propsOf(*shop.state, shop.vise);
    CHECK_THAT(viseProps, ContainsSubstring("\"angle\":\"180 deg\""));
    CHECK(viseProps.find("tilt") == std::string::npos);
    CHECK(viseProps.find("mate") == std::string::npos);
    const std::string jawProps = propsOf(*shop.state, shop.jaw);
    CHECK_THAT(jawProps, ContainsSubstring("\"gap\":\"65 mm\""));
    CHECK_THAT(jawProps, ContainsSubstring("\"mate\""));
    CHECK_THAT(jawProps, ContainsSubstring(shop.vise.value.toString()));
    // The new features are named after the bodies; the vise's comes before the jaw's mate to it.
    std::size_t viseAt = 0, jawAt = 0;
    for (std::size_t i = 0; i < shop.state->features.size(); ++i) {
        const doc::Feature& f = shop.state->features[i];
        if (movedBody(f) == shop.vise.value.toString()) {
            viseAt = i;
            CHECK(f.name == "Place Generic 6 in vise");
        }
        if (movedBody(f) == shop.jaw.value.toString()) {
            jawAt = i;
        }
    }
    CHECK(viseAt < jawAt);

    // ---- Run again after the stock changed (5 mm more below): the same four placements, edited ----
    shop.run(ct::command("cam.setup.edit",
                         R"({"id":")" + ct::id('a', 1) +
                             R"(","stock":{"type":"box","xNeg":2,"xPos":3,"yNeg":1,"yPos":4,"zNeg":5,"zPos":1}})"));
    shop.rebuild();
    const cam::ResolvedSetup r2 = shop.resolved(1);
    const cf::PlacePlan again = shop.place(1, cf::Grip::Shorter, true);
    CHECK(again.addedToSetup.empty());
    CHECK(shop.placements() == 4);
    checkStand(shop, r2, again.fit, true);
    CHECK(shop.inWcs(shop.parA, r2).max.z == Catch::Approx(-26).margin(2e-5)); // under the lower stock's bottom

    // ---- Across X, on the floor (the parallels left out) ----
    shop.run(ct::command("cam.setup.edit",
                         R"({"id":")" + ct::id('a', 1) +
                             R"(","stock":{"type":"box","xNeg":2,"xPos":3,"yNeg":1,"yPos":4,"zNeg":0,"zPos":1}})"));
    shop.rebuild();
    const cf::PlacePlan acrossX = shop.place(1, cf::Grip::AcrossX, false);
    CHECK(acrossX.fit.acrossX);
    CHECK(acrossX.fit.opening == Catch::Approx(105).margin(1e-9));
    checkStand(shop, r, acrossX.fit, false);
    const Box3 jaw = shop.inWcs(shop.jaw, r);
    checkBox(jaw, Box3{{105, 32.5 - 76.2, -21}, {165, 32.5 + 76.2, 23.45}}, "moving jaw across X (numbers)");
    CHECK_THAT(propsOf(*shop.state, shop.vise), ContainsSubstring("\"angle\":\"-90 deg\""));
}

TEST_CASE("the stand is the same in a setup turned about Z, turned over, and on its side", "[cam][fixtures][place]")
{
    SECTION("the WCS turned 30 degrees about Z: the block's X edge")
    {
        Shop shop;
        shop.make(30);
        // The block's bottom edge from (0, 0) along 30 degrees.
        const kernel::Shape& block = shop.build->bodies.at(ct::body('b')).shape;
        const auto edge = dt::findEdge(block, [](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && dt::near(c.start.z, 0) && dt::near(c.end.z, 0) &&
                   dt::near(c.length, 100) && (dt::near(c.start.x, 0) || dt::near(c.end.x, 0)) &&
                   (dt::near(c.start.y, 0) || dt::near(c.end.y, 0));
        });
        REQUIRE(edge);
        shop.addSetup(1, shop.ids({shop.parA, shop.parB}),
                      R"({"origin":{"mode":"stockCorner","corner":0},"workOffset":1,"xEdge":)" +
                          naming::toJson(shop.ref(ct::body('b'), *edge)) + "}");
        shop.rebuild();
        const cam::ResolvedSetup r = shop.resolved(1);
        // X along the edge (either way), Z up.
        CHECK(std::fabs(r.wcsFromModel.m[0]) == Catch::Approx(std::cos(kPi / 6)).margin(1e-9));
        CHECK(r.wcsFromModel.m[10] == 1.0);
        const cf::PlacePlan plan = shop.place(1, cf::Grip::Shorter, true);
        checkFirstStand(shop, r);
        checkStand(shop, r, plan.fit, true);
        const std::string props = propsOf(*shop.state, shop.vise);
        CHECK(props.find("tilt") == std::string::npos);
        CHECK((props.find("\"angle\":\"-150 deg\"") != std::string::npos ||
               props.find("\"angle\":\"30 deg\"") != std::string::npos));
    }
    SECTION("turned over: the WCS Z is model -Z, the vise upside down in the model")
    {
        Shop shop;
        shop.make();
        shop.addSetup(1, shop.ids({shop.parA, shop.parB}),
                      R"({"flipZ":true,"origin":{"mode":"stockCorner","corner":0},"workOffset":1})");
        shop.rebuild();
        const cam::ResolvedSetup r = shop.resolved(1);
        CHECK(r.wcsFromModel.m[10] == -1.0);
        const cf::PlacePlan plan = shop.place(1, cf::Grip::Shorter, true);
        checkFirstStand(shop, r);
        checkStand(shop, r, plan.fit, true);
        CHECK_THAT(propsOf(*shop.state, shop.vise), ContainsSubstring("\"tiltX\":\"180 deg\""));
        // In the model the vise hangs upside down over the block: the stock's top in this setup is model z = -1 (the
        // block's bottom grown by 1), and the jaws' tops stop 1.95 short of it, at model z = 0.95.
        const Box3 v = kernel::bounds(shop.build->bodies.at(shop.vise).shape);
        CHECK(v.min.z == Catch::Approx(0.95).margin(2e-5));
        CHECK(v.max.z == Catch::Approx(20 + 25.4 + 100).margin(2e-5)); // its base on top
    }
    SECTION("on its side: the WCS Z is the block's +X face")
    {
        Shop shop;
        shop.make();
        const kernel::SubId side = shop.face(ct::body('b'), {1, 0, 0}, 100);
        shop.addSetup(1, shop.ids({shop.parA, shop.parB}),
                      R"({"origin":{"mode":"stockCorner","corner":0},"workOffset":1,"zFace":)" +
                          naming::toJson(shop.ref(ct::body('b'), side)) + "}");
        shop.rebuild();
        const cam::ResolvedSetup r = shop.resolved(1);
        CHECK(r.wcsFromModel.m[8] == 1.0); // WCS Z = model +X
        const cf::PlacePlan plan = shop.place(1, cf::Grip::Shorter, true);
        checkStand(shop, r, plan.fit, true);
        CHECK_THAT(propsOf(*shop.state, shop.vise), ContainsSubstring("tilt"));
        // The jaws close on the stock's two faces across the grip, the stock on the parallels.
        const Box3 jaw = shop.inWcs(shop.jaw, r);
        const Box3 a = shop.inWcs(shop.parA, r);
        CHECK(a.max.z == Catch::Approx(r.stockBounds.min.z).margin(2e-5));
        if (plan.fit.acrossX) {
            CHECK(jaw.min.x == Catch::Approx(r.stockBounds.max.x).margin(2e-5));
        } else {
            CHECK(jaw.max.y == Catch::Approx(r.stockBounds.min.y).margin(2e-5));
        }
    }
}

TEST_CASE("a stock wider than the vise opens, and one narrower than the parallels", "[cam][fixtures][place]")
{
    Shop shop;
    shop.make();
    // 300 across X: the jaw at the vise's widest, 270, not touching the stock.
    shop.addSetup(1, shop.ids({shop.parA, shop.parB}), "",
                  R"({"type":"box","xNeg":0,"xPos":200,"yNeg":0,"yPos":0,"zNeg":0,"zPos":0})");
    shop.rebuild();
    const cam::ResolvedSetup r = shop.resolved(1);
    const cf::PlacePlan plan = shop.place(1, cf::Grip::AcrossX, false);
    CHECK(plan.fit.tooWide);
    CHECK(plan.fit.across == Catch::Approx(300).margin(1e-6));
    CHECK(plan.fit.opening == 270);
    CHECK(shop.inWcs(shop.jaw, r).min.x == Catch::Approx(270).margin(2e-5));
    checkStand(shop, r, plan.fit, false);

    // A 5 mm wide part: the two parallels (3.175 each) do not fit side by side under it.
    shop.run(dt::addFeature("sketch", 20, dt::sketchProps(kXY, dt::SketchJson().rect(1, 300, 0, 305, 40))));
    shop.run(dt::addFeature("extrude", 21, dt::extrudeProps(20, "10", "new", 'c')));
    shop.run(ct::command("cam.setup.add", R"({"id":")" + ct::id('a', 2) + R"(","name":"Narrow","partBodies":[")" +
                                              ct::body('c').value.toString() + R"("],"fixtureBodies":[)" +
                                              shop.ids({shop.parA, shop.parB}) + "]}"));
    shop.rebuild();
    auto refused = cf::planViseOnStock(shop.request(2, cf::Grip::Shorter, true));
    REQUIRE_FALSE(refused);
    CHECK_THAT(refused.error().message, ContainsSubstring("narrower"));
    // Without parallels it closes on it.
    auto plain = cf::planViseOnStock(shop.request(2, cf::Grip::Shorter, false));
    REQUIRE(plain);
    CHECK(plain->fit.opening == Catch::Approx(5).margin(1e-9));
}

// ---------- The safety checks see the placed vise ----------
namespace {

std::string toolItem()
{
    return R"({"schema":1,"id":")" + ct::id('d', 1) +
           R"(","name":"10 mm 3FL end mill","type":"flatEndMill","flutes":3,"diameter":10,"shankDiameter":10,)"
           R"("fluteLength":13,"shoulderLength":13,"overallLength":60,"defaultToolNumber":1})";
}

std::string machineItem()
{
    return R"({"schema":1,"id":")" + ct::id('9', 1) +
           R"(","name":"Mill","kind":"mill3","axes":[{"letter":"X","min":0,"max":762,"rapid":25400,"maxFeed":10000,)"
           R"("accel":3000},{"letter":"Y","min":0,"max":406,"rapid":25400,"maxFeed":10000,"accel":3000},)"
           R"({"letter":"Z","min":-508,"max":0,"rapid":25400,"maxFeed":10000,"accel":3000}],)"
           R"("spindle":{"maxRpm":12000},"toolChanger":{"capacity":20},)"
           R"("control":{"dialect":"generic-iso","defaultPost":"generic-iso","rigidTapping":true}})";
}

struct Checked {
    std::vector<sim::SimEvent> events;
    // The setup checks on their own (after T-074: the fixtures against the part and the stock), their work and time.
    std::vector<sim::SimEvent> setup;
    sim::detail::OverlapStats setupStats;
    double setupMs = 0;
    std::vector<Box3> fixtureBoxes; // the fixture solids the simulator gets, in the WCS
    std::vector<BodyId> fixtures;
    cf::ViseFit fit;
};

std::string describe(const std::vector<sim::SimEvent>& events)
{
    std::string s;
    for (const sim::SimEvent& e : events) {
        s +=
            std::to_string(static_cast<int>(e.kind)) + " move " + std::to_string(e.moveIndex) + ": " + e.message + "\n";
    }
    return s;
}

// The block of fixture_verify_test.cpp (100 x 60 x 20, a 60 x 20 pocket 12 deep at y 6..26), its stock the block; the
// vise placed on the stock (on the parallels or not: the 1 in pair, or after T-077 `pair`); the pocket generated and
// checked.
Checked checkedJob(bool parallels, const fm::ParallelsFile& pair = fm::kParallelsFiles[0])
{
    Shop shop;
    std::shared_ptr<cam::OperationRegistry> types = std::make_shared<cam::OperationRegistry>();
    cam::registerBuiltinOperationTypes(*types);
    std::unique_ptr<cam::ICamEngine> engine = cam::makeCamEngine(*shop.env.jobs, types, &doc::camSnapshot);
    REQUIRE(engine);
    shop.run(dt::addFeature("sketch", 2, dt::sketchProps(kXY, dt::SketchJson().rect(1, 0, 0, 100, 60))));
    shop.run(dt::addFeature("extrude", 3, dt::extrudeProps(2, "20", "new", 'b')));
    shop.run(dt::addFeature("datum_plane", 4, R"({"base":{"principal":"xy"},"offset":20})"));
    shop.run(dt::addFeature(
        "sketch", 5,
        dt::sketchProps(R"({"datum":")" + dt::uuid('f', 4) + R"("})", dt::SketchJson().rect(1, 20, 6, 80, 26))));
    shop.run(dt::addFeature("extrude", 6, dt::extrudeProps(5, "12", "cut", 'b', R"(,"direction":"reverse")")));
    shop.import("generic-vise-6in.step", 7);
    shop.import(pair.fileName, 8);
    shop.vise = shop.named("Generic 6 in vise");
    shop.jaw = shop.named("Generic 6 in vise moving jaw");
    shop.parA = shop.named(pair.make()[0].name);
    shop.parB = shop.named(pair.make()[1].name);
    shop.rebuild();
    shop.run(ct::command("cam.library.embed", ct::library(toolItem(), ct::holderJson(1), machineItem())));
    shop.run(
        ct::setupAdd(1, "\"" + ct::body('b').value.toString() + "\"",
                     parallels ? R"(,"fixtureBodies":[)" + shop.ids({shop.parA, shop.parB}) + "]" : std::string()));
    shop.rebuild();
    const kernel::SubId floor = shop.face(ct::body('b'), {0, 0, 1}, 8);
    shop.run(
        ct::command("cam.operation.add",
                    R"({"id":")" + ct::id('c', 1) + R"(","setup":")" + ct::id('a', 1) +
                        R"(","type":"pocket2d","name":"Pocket","tool":{"tool":")" + ct::id('d', 1) + R"(","holder":")" +
                        ct::id('e', 1) +
                        R"(","stickOut":14},"cutting":{"rpm":8000,"feedCut":1200,"feedPlunge":400,"feedRamp":900},)"
                        R"("heights":{"clearance":15,"retract":5,"feedPlane":2,"top":0,"bottom":0},"geometry":[)" +
                        naming::toJson(shop.ref(ct::body('b'), floor)) + R"(],"params":{"stepdown":4}})"));
    cf::PlaceRequest rq = shop.request(1, cf::Grip::Shorter, parallels);
    if (parallels) {
        std::string json = pair.fileName;
        rq.parallelsModel = shipped(json.replace(json.rfind(".step"), 5, ".json")).parallels;
    }
    auto planned = cf::planViseOnStock(rq);
    INFO((planned ? std::string() : planned.error().message));
    REQUIRE(planned);
    shop.apply(*planned);
    const cf::PlacePlan plan = *planned;

    Checked out;
    out.fit = plan.fit;
    out.fixtures = shop.state->cam->setups.front().fixtureBodies;
    auto handle = engine->generate(shop.state, shop.build, ct::opId(1));
    std::optional<Result<std::shared_ptr<const cam::Toolpath>>> result;
    handle.then([&](Result<std::shared_ptr<const cam::Toolpath>> r) { result = std::move(r); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
    while (!result && std::chrono::steady_clock::now() < deadline) {
        shop.env.ui.runPending();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(result.has_value());
    shop.env.ui.runPending();
    if (!*result) {
        FAIL("generation failed: " << result->error().message);
    }
    const std::shared_ptr<const cam::Toolpath> paths[] = {**result};
    JobContext ctx;
    auto built =
        api::cam_sim::buildSimProgram(shop.state, shop.build, shop.state->cam->setups.at(0).id, paths, {}, ctx);
    if (!built) {
        FAIL("buildSimProgram failed: " << built.error().message);
    }
    const sim::SimProgram& program = built->program;
    for (const kernel::TriSoup& soup : program.fixtureSolids) {
        Box3 b{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
        for (std::size_t i = 0; i + 2 < soup.positions.size(); i += 3) {
            b.min = Vec3{std::min<double>(b.min.x, soup.positions[i]), std::min<double>(b.min.y, soup.positions[i + 1]),
                         std::min<double>(b.min.z, soup.positions[i + 2])};
            b.max = Vec3{std::max<double>(b.max.x, soup.positions[i]), std::max<double>(b.max.y, soup.positions[i + 1]),
                         std::max<double>(b.max.z, soup.positions[i + 2])};
        }
        out.fixtureBoxes.push_back(b);
    }
    JobContext verifyCtx;
    auto events = sim::detail::verifyProgram(sim::detail::verifyInputFrom(program), *shop.env.jobs, verifyCtx);
    REQUIRE(events);
    out.events = std::move(*events);
    const auto t0 = std::chrono::steady_clock::now();
    auto setup =
        sim::detail::setupEvents(sim::detail::verifyInputFrom(program), *shop.env.jobs, verifyCtx, &out.setupStats);
    out.setupMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(setup);
    out.setup = std::move(*setup);
    engine.reset();
    return out;
}

} // namespace

TEST_CASE("the safety checks see the vise placed on the stock; jaws on the stock are no collision",
          "[cam][fixtures][place][acceptance]")
{
    const Library lib;
    // Stock = the block: x 0..100, y 0..60, z -20..0 in the WCS (corner 0 at the origin).
    {
        const Checked on = checkedJob(true);
        INFO(describe(on.events));
        // The vise, its jaw and the parallels are fixtures of the setup, where the placement put them: the moving jaw
        // on the stock's -Y face, the parallels' tops on its bottom, the jaws' tops 0.95 below its top.
        REQUIRE(on.fixtureBoxes.size() == 4);
        CHECK(on.fixtures.size() == 4);
        checkBox(on.fixtureBoxes[3], moved(lib.jaw, then(on.fit.wcsFromVise, shiftY(on.fit.opening - fm::kOpening))),
                 "the jaw the simulator gets", 1e-3);
        CHECK(on.fixtureBoxes[3].max.y == Catch::Approx(0).margin(1e-4));
        CHECK(on.fixtureBoxes[3].max.z == Catch::Approx(-0.95).margin(1e-4));
        CHECK(on.fixtureBoxes[0].max.z == Catch::Approx(-20).margin(1e-4));
        // Nothing at all: the jaws touching the stock's sides and the stock on the parallels raise no event, and the
        // holder passes over the jaws.
        CHECK(on.events.empty());
        // The setup checks (after T-074) see the jaws on the stock's (and the part's) sides and the parallels under
        // them: touching, not overlapping.
        CHECK(on.setup.empty());
        CHECK(on.setupStats.capped == 0);
        WARN("the vise on the parallels, setup checks: " << on.setupStats.triangles << " triangles, "
                                                         << on.setupStats.patches << " patches, " << on.setupMs
                                                         << " ms");
    }
    {
        const Checked low = checkedJob(false);
        INFO(describe(low.events));
        REQUIRE(low.fixtureBoxes.size() == 2);
        CHECK(low.fixtureBoxes[1].max.z == Catch::Approx(24.45).margin(1e-4)); // the jaws 24.45 above the stock
        // On the vise's floor: touching it and the jaws, no setup event.
        CHECK(low.setup.empty());
        CHECK(low.setupStats.capped == 0);
        WARN("the vise on its floor, setup checks: " << low.setupStats.triangles << " triangles, "
                                                     << low.setupStats.patches << " patches, " << low.setupMs << " ms");
        // The holder over the pocket's front wall overhangs the moving jaw (fixture 2).
        std::size_t holderHits = 0;
        for (const sim::SimEvent& e : low.events) {
            CHECK((e.kind == sim::EventKind::HolderHitsFixture || e.kind == sim::EventKind::ToolTooShort));
            if (e.kind == sim::EventKind::HolderHitsFixture) {
                ++holderHits;
                CHECK_THAT(e.message, ContainsSubstring("fixture 2"));
            }
        }
        CHECK(holderHits >= 1);
    }
}

// After T-077: each taller pair placed by the action under the same block (20 tall). The parallels' tops on the
// stock's bottom, the jaws' tops the pair's height less 44.45 above it; the setup checks (src/sim/verify_setup) find
// nothing (the jaws and the parallels touch the stock, nothing overlaps), and neither do the motion checks (the jaws
// are lower than on the 1 in pair, which already raised nothing).
TEST_CASE("each taller pair placed on the stock: no setup event, no motion event", "[cam][fixtures][place][parallels]")
{
    for (std::size_t k = 1; k < std::size(fm::kParallelsFiles); ++k) {
        const fm::ParallelsFile& pair = fm::kParallelsFiles[k];
        const double height = pair.eighths * fm::kIn / 8;
        INFO(pair.fileName);
        const Checked on = checkedJob(true, pair);
        INFO(describe(on.events) << describe(on.setup));
        REQUIRE(on.fixtureBoxes.size() == 4);
        CHECK(on.fit.floorZ == Catch::Approx(-20 - height).margin(1e-9));
        CHECK(on.fit.gripDepth == Catch::Approx(44.45 - height).margin(1e-9));
        CHECK(on.fixtureBoxes[0].max.z == Catch::Approx(-20).margin(1e-4)); // parallel A under the stock
        CHECK(on.fixtureBoxes[1].max.z == Catch::Approx(-20).margin(1e-4)); // parallel B
        CHECK(on.fixtureBoxes[0].min.z == Catch::Approx(-20 - height).margin(1e-4));
        CHECK(on.fixtureBoxes[3].max.z == Catch::Approx(-20 - height + 44.45).margin(1e-4)); // the moving jaw's top
        CHECK(on.setup.empty());
        CHECK(on.setupStats.capped == 0);
        CHECK(on.events.empty());
    }
}

// After T-081b round 9: the transform_body feature that places a fixture passes its move as a body placement
// (naming::NamingInput::bodyPlacement), so the vise's map carries it and a face picked on the vise as its library file
// has it (the mate faces are) follows the vise wherever the placement puts it, decided by its samples, not by a guess.
TEST_CASE("the vise's placement is a body placement: a face picked before it follows the vise",
          "[cam][fixtures][place]")
{
    Shop shop;
    shop.make();
    shop.addSetup(1, shop.ids({shop.parA, shop.parB}));
    shop.rebuild();
    // The vise's largest planar face, picked as the library file has it.
    const kernel::Shape& library = shop.build->bodies.at(shop.vise).shape;
    std::optional<kernel::SubId> face;
    double largest = 0;
    Vec3 centroid{};
    for (std::uint32_t i = 0; i < library.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId sub{kernel::SubKind::Face, i};
        auto info = kernel::surfaceInfo(library, sub);
        if (info && info->type == kernel::SurfaceType::Plane && info->area > largest + 1e-9) {
            largest = info->area;
            face = sub;
            centroid = info->centroid;
        }
    }
    REQUIRE(face);
    const naming::TopoRef ref = shop.ref(shop.vise, *face);
    const Library lib;

    shop.place(1, cf::Grip::Shorter, true);
    const doc::BuiltBody& placed = shop.build->bodies.at(shop.vise);
    // The vise's map: one body move, carrying the import's elements, that takes the library box to where the vise is.
    const std::vector<const naming::FeaturePlacement*> moves =
        naming::detail::bodyMovesOf(placed.map, dt::featureId(7));
    REQUIRE(moves.size() == 1);
    Xform move{};
    for (int i = 0; i < 12; ++i) {
        move.m[i] = static_cast<double>(moves[0]->q[i]) / (i % 4 == 3 ? 1e4 : 1e9);
    }
    checkBox(kernel::bounds(placed.shape), moved(lib.vise, move), "the vise moved by its body placement", 1e-3);
    // The face picked before the placement: Exact on the moved face (its centroid moved by the placement), by its
    // samples (an imported face is no whole generator element).
    const auto resolver = naming::makeResolver({naming::ResolverBody{shop.vise, placed.map, placed.shape}});
    const naming::Resolved r = resolver->resolve(ref);
    INFO(r.reason);
    REQUIRE(r.state == naming::ResolveState::Exact);
    CHECK_FALSE(r.identity);
    REQUIRE(r.elements.size() == 1);
    auto info = kernel::surfaceInfo(placed.shape, r.elements.front());
    REQUIRE(info);
    const Vec3 want = apply(move, centroid);
    CHECK(info->centroid.x == Catch::Approx(want.x).margin(1e-3));
    CHECK(info->centroid.y == Catch::Approx(want.y).margin(1e-3));
    CHECK(info->centroid.z == Catch::Approx(want.z).margin(1e-3));
    // The mutation check: the body moves ignored, the move is not known and the answer is no longer Exact.
    naming::testing::setBodyMovesEnabled(false);
    const naming::Resolved off = resolver->resolve(ref);
    naming::testing::setBodyMovesEnabled(true);
    CHECK(off.state != naming::ResolveState::Exact);
}
