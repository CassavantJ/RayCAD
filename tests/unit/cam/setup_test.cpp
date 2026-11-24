// Setup resolution (task T-033): the WCS frame rules, the stock types and their bounds, the corner numbering on an
// imported reference part, the errors, and bit-identical results. Builds are made by hand (boxes and cylinders named
// as imported bodies) or by importing a corpus part through the document; the edit-following test is in
// setup_document_test.cpp. T-044: flip X and flip Z (frame, codec, commands, input hash).
#include "engine_test_util.h"
#include "setup_test_util.h"

#include "rebuild_test_util.h" // the doc tests' rebuild environment (tests/unit/doc), for the corpus import

#include "commands/cam_json.h" // the WCS codec (src/doc), for the flips (T-044)

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

namespace dt = ray::doc::test;

constexpr double kPi = 3.14159265358979323846;

kernel::Shape boxShape(Vec3 min, Vec3 max)
{
    auto shape = kernel::makeBox(min, max);
    REQUIRE(shape);
    return *shape;
}

// `shape` turned by `angle` about model axis `axis` (0 X, 2 Z) through the origin; dmath, so the test's model is the
// same on every platform.
kernel::Shape turned(const kernel::Shape& shape, int axis, double angle)
{
    const double c = dmath::cos(angle);
    const double s = dmath::sin(angle);
    Xform xf = Xform::identity();
    if (axis == 0) {
        xf = Xform{{1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0}};
    } else {
        xf = Xform{{c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0}};
    }
    auto moved = kernel::transformed(shape, xf);
    REQUIRE(moved);
    return moved->shape;
}

// Bodies b1.. built by hand and named as imported ones, a document that names them, and setup 1 on b1.
struct Scene {
    doc::DocStatePtr state = std::make_shared<const doc::DocState>();
    std::shared_ptr<doc::BuildResult> build = std::make_shared<doc::BuildResult>();

    void body(std::uint64_t n, const kernel::Shape& shape)
    {
        state = exec(state, command("body.set", R"({"id":")" + id('b', n) + R"(","name":"Body )" + std::to_string(n) +
                                                    R"(","role":"part"})"));
        build->bodies[ray::cam::test::body('b', n)] =
            doc::BuiltBody{shape, naming::nameImported(FeatureId{Uuid::fromString(id('f', n))}, shape)};
    }
    void setup(const std::string& extra = {})
    {
        state = exec(state, command("cam.library.embed", library(toolJson(1), {}, machineJson(1))));
        state = exec(state, setupAdd(1, "\"" + id('b', 1) + "\"", extra));
    }
    CamSnapshot snap() const { return doc::camSnapshot(state, build); }
    const kernel::Shape& shape(std::uint64_t n) const { return build->bodies.at(ray::cam::test::body('b', n)).shape; }
};

// The 40 x 20 x 10 block b1 at the origin and setup 1 on it with default stock and WCS.
struct Block : Scene {
    Block()
    {
        body(1, boxShape(Vec3{0, 0, 0}, Vec3{40, 20, 10}));
        setup();
    }
};

kernel::SubId planeFace(const kernel::Shape& shape, Vec3 centroid)
{
    return findFace(shape, [&](const kernel::SurfaceInfo& s) {
        return s.type == kernel::SurfaceType::Plane && near3(s.centroid, centroid, 1e-6);
    });
}

// The top face of the 40 x 20 x 10 block turned 30 degrees about X: its centroid (20, 10, 10) turned.
kernel::SubId tiltedTop(const kernel::Shape& shape)
{
    const double c = dmath::cos(kPi / 6);
    const double s = dmath::sin(kPi / 6);
    return planeFace(shape, Vec3{20, c * 10 - s * 10, s * 10 + c * 10});
}

// The axes of the frame as rows.
struct Axes {
    Vec3 x, y, z;
};
Axes axesOf(const ResolvedSetup& rs)
{
    return Axes{axisOf(rs.wcsFromModel, 0), axisOf(rs.wcsFromModel, 1), axisOf(rs.wcsFromModel, 2)};
}

void checkAxes(const ResolvedSetup& rs, Vec3 x, Vec3 y, Vec3 z)
{
    const Axes a = axesOf(rs);
    INFO("X " << text(a.x) << " Y " << text(a.y) << " Z " << text(a.z));
    CHECK(same3(a.x, x));
    CHECK(same3(a.y, y));
    CHECK(same3(a.z, z));
    checkFrame(rs);
}

Vec3 extent(const Box3& b)
{
    return Vec3{b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z};
}

} // namespace

TEST_CASE("the WCS frame follows the rules of cam.h on a block", "[cam][setup]")
{
    Block f;
    const CamSnapshot snap = f.snap();
    const auto resolver = naming::makeResolver(snap.bodies);
    const BodyId b1 = body('b', 1);
    const kernel::Shape& shape = f.shape(1);
    Setup s = snap.cam->setups.at(0);
    s.wcs.originMode = Wcs::OriginMode::StockCorner;
    s.wcs.stockCornerIndex = 0;

    SECTION("no references: the model axes")
    {
        checkAxes(resolveOk(snap, s, *resolver), Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1});
    }
    SECTION("the top face: Z up, X along model X")
    {
        s.wcs.zFace = resolver->makeRef(b1, planeFace(shape, Vec3{20, 10, 10}));
        checkAxes(resolveOk(snap, s, *resolver), Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1});
    }
    SECTION("the bottom face: Z out of the material, so down; the part turned over about X")
    {
        s.wcs.zFace = resolver->makeRef(b1, planeFace(shape, Vec3{20, 10, 0}));
        checkAxes(resolveOk(snap, s, *resolver), Vec3{1, 0, 0}, Vec3{0, -1, 0}, Vec3{0, 0, -1});
    }
    SECTION("a side face: model up is WCS Y")
    {
        s.wcs.zFace = resolver->makeRef(b1, planeFace(shape, Vec3{20, 0, 5})); // the face at y = 0
        checkAxes(resolveOk(snap, s, *resolver), Vec3{1, 0, 0}, Vec3{0, 0, 1}, Vec3{0, -1, 0});
        s.wcs.zFace = resolver->makeRef(b1, planeFace(shape, Vec3{40, 10, 5})); // the face at x = 40
        checkAxes(resolveOk(snap, s, *resolver), Vec3{0, 1, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0});
    }
    SECTION("the top face and an edge: X along the edge in its curve direction")
    {
        s.wcs.zFace = resolver->makeRef(b1, planeFace(shape, Vec3{20, 10, 10}));
        // An edge along model Y on the top face, at x = 40.
        const auto edge = findEdge(shape, [](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && std::abs(c.start.x - 40) < 1e-9 &&
                   std::abs(c.end.x - 40) < 1e-9 && std::abs(c.start.z - 10) < 1e-9 && std::abs(c.end.z - 10) < 1e-9;
        });
        s.wcs.xEdge = resolver->makeRef(b1, edge);
        const double sign = kernel::curveInfo(shape, edge)->end.y > kernel::curveInfo(shape, edge)->start.y ? 1 : -1;
        checkAxes(resolveOk(snap, s, *resolver), Vec3{0, sign, 0}, Vec3{-sign, 0, 0}, Vec3{0, 0, 1});
        // An edge that does not lie in the face is projected into it: a vertical edge would not do (see the errors),
        // but an edge of the bottom face along X gives X along model X.
        const auto bottom = findEdge(shape, [](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && std::abs(c.start.y) < 1e-9 && std::abs(c.end.y) < 1e-9 &&
                   std::abs(c.start.z) < 1e-9 && std::abs(c.end.z) < 1e-9;
        });
        s.wcs.xEdge = resolver->makeRef(b1, bottom);
        const double sx = kernel::curveInfo(shape, bottom)->end.x > kernel::curveInfo(shape, bottom)->start.x ? 1 : -1;
        checkAxes(resolveOk(snap, s, *resolver), Vec3{sx, 0, 0}, Vec3{0, sx, 0}, Vec3{0, 0, 1});
    }
    SECTION("an edge without a face: Z stays model Z")
    {
        const auto edge = findEdge(shape, [](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && std::abs(c.start.x) < 1e-9 && std::abs(c.end.x) < 1e-9 &&
                   std::abs(c.start.z) < 1e-9 && std::abs(c.end.z) < 1e-9;
        });
        s.wcs.xEdge = resolver->makeRef(b1, edge);
        const double sign = kernel::curveInfo(shape, edge)->end.y > kernel::curveInfo(shape, edge)->start.y ? 1 : -1;
        checkAxes(resolveOk(snap, s, *resolver), Vec3{0, sign, 0}, Vec3{-sign, 0, 0}, Vec3{0, 0, 1});
    }
    SECTION("a model point as origin")
    {
        s.wcs.originMode = Wcs::OriginMode::ModelPoint;
        s.wcs.originVertex =
            resolver->makeRef(b1, findVertex(shape, [](Vec3 p) { return near3(p, Vec3{40, 20, 10}, 1e-9); }));
        const ResolvedSetup rs = resolveOk(snap, s, *resolver);
        CHECK(same3(apply(rs.wcsFromModel, Vec3{40, 20, 10}), Vec3{0, 0, 0}));
        CHECK(nearBox(rs.partBounds, Box3{Vec3{-40, -20, -10}, Vec3{0, 0, 0}}, 0));
    }
}

TEST_CASE("a tilted frame measures the part in the WCS orientation, not in the model axes", "[cam][setup]")
{
    SECTION("Z from a face turned 30 degrees about model X")
    {
        Scene f;
        f.body(1, turned(boxShape(Vec3{0, 0, 0}, Vec3{40, 20, 10}), 0, kPi / 6));
        f.setup(R"(,"stock":{"type":"box","xNeg":1,"xPos":2,"yNeg":3,"yPos":4,"zNeg":5,"zPos":6})");
        const CamSnapshot snap = f.snap();
        const auto resolver = naming::makeResolver(snap.bodies);
        Setup s = snap.cam->setups.at(0);
        // The block's top face, now with normal (0, -sin 30, cos 30).
        s.wcs.zFace = resolver->makeRef(body('b', 1), tiltedTop(f.shape(1)));
        const ResolvedSetup rs = resolveOk(snap, s, *resolver);
        checkFrame(rs);
        const Axes a = axesOf(rs);
        CHECK(same3(a.x, Vec3{1, 0, 0}));
        CHECK(near3(a.z, Vec3{0, -0.5, std::sqrt(3.0) / 2}, 1e-12));
        // In the WCS orientation the block is square again: 40 x 20 x 10, plus the offsets in WCS directions.
        CHECK(near3(extent(rs.partBounds), Vec3{40, 20, 10}, 2e-6));
        CHECK(near3(extent(rs.stockBounds), Vec3{43, 27, 21}, 2e-6));
        // Corner 0: the stock's top front left corner is the origin.
        CHECK(rs.stockBounds.min.x == 0.0);
        CHECK(rs.stockBounds.min.y == 0.0);
        CHECK(rs.stockBounds.max.z == 0.0);
        // The model box (axis-aligned in the model) would be much taller.
        CHECK(kernel::bounds(f.shape(1)).max.z - kernel::bounds(f.shape(1)).min.z > 18);
        // A solid stock solid is made in the WCS: its box is the stock box.
        const ResolvedSetup withSolid = resolveOk(snap, s, *resolver, SetupResolveOptions{true});
        REQUIRE_FALSE(withSolid.stockSolid.isNull());
        CHECK(nearBox(kernel::bounds(withSolid.stockSolid), withSolid.stockBounds, 1e-9));
        CHECK(hashOf(withSolid) == hashOf(rs));
    }
    SECTION("X from an edge turned 30 degrees about model Z")
    {
        Scene f;
        f.body(1, turned(boxShape(Vec3{0, 0, 0}, Vec3{40, 20, 10}), 2, kPi / 6));
        f.setup();
        const CamSnapshot snap = f.snap();
        const auto resolver = naming::makeResolver(snap.bodies);
        Setup s = snap.cam->setups.at(0);
        // The long bottom edge that started on model X at the origin, now along (cos 30, sin 30, 0).
        const auto edge = findEdge(f.shape(1), [](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && std::abs(c.length - 40) < 1e-9 && std::abs(c.start.z) < 1e-9 &&
                   std::abs(c.end.z) < 1e-9 && (near3(c.start, Vec3{}, 1e-9) || near3(c.end, Vec3{}, 1e-9));
        });
        s.wcs.xEdge = resolver->makeRef(body('b', 1), edge);
        const ResolvedSetup rs = resolveOk(snap, s, *resolver);
        checkFrame(rs);
        const Axes a = axesOf(rs);
        const double c = std::sqrt(3.0) / 2;
        CHECK((near3(a.x, Vec3{c, 0.5, 0}, 1e-12) || near3(a.x, Vec3{-c, -0.5, 0}, 1e-12)));
        CHECK(same3(a.z, Vec3{0, 0, 1}));
        CHECK(near3(extent(rs.partBounds), Vec3{40, 20, 10}, 2e-6));
    }
}

TEST_CASE("an axis within 1e-9 of a model axis is that axis exactly", "[cam][setup]")
{
    // A block turned by 1e-10 rad about X: its top face normal is off model Z by that much, and the frame snaps.
    Scene f;
    f.body(1, turned(boxShape(Vec3{0, 0, 0}, Vec3{40, 20, 10}), 0, 1e-10));
    f.setup();
    const CamSnapshot snap = f.snap();
    const auto resolver = naming::makeResolver(snap.bodies);
    Setup s = snap.cam->setups.at(0);
    const auto face = findFace(f.shape(1), [](const kernel::SurfaceInfo& i) {
        return i.type == kernel::SurfaceType::Plane && i.centroid.z > 9.9;
    });
    s.wcs.zFace = resolver->makeRef(body('b', 1), face);
    const auto info = kernel::surfaceInfo(f.shape(1), face);
    REQUIRE(info);
    CHECK(std::abs(info->axis.y) > 5e-11); // the face really is tilted
    const ResolvedSetup rs = resolveOk(snap, s, *resolver);
    checkAxes(rs, Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1});
}

TEST_CASE("WCS references that cannot give a frame are refused with the reason", "[cam][setup]")
{
    Block f;
    f.body(4, [] {
        auto c = kernel::makeCylinder(Vec3{100, 0, 0}, Vec3{0, 0, 1}, 5, 10);
        REQUIRE(c);
        return *c;
    }());
    const CamSnapshot snap = f.snap();
    const auto resolver = naming::makeResolver(snap.bodies);
    const BodyId b1 = body('b', 1);
    const kernel::Shape& shape = f.shape(1);
    Setup s = snap.cam->setups.at(0);
    const auto top = resolver->makeRef(b1, planeFace(shape, Vec3{20, 10, 10}));
    const auto fails = [&](ErrorCode code, const char* textPart) {
        const Error e = resolveFails(snap, s, *resolver);
        INFO(e.message);
        CHECK(e.code == code);
        CHECK_THAT(e.message, ContainsSubstring(textPart));
    };

    SECTION("an X edge along Z")
    {
        s.wcs.zFace = top;
        s.wcs.xEdge = resolver->makeRef(b1, findEdge(shape, [](const kernel::CurveInfo& c) {
                                            return c.type == kernel::CurveType::Line && std::abs(c.axis.z) > 0.99;
                                        }));
        fails(ErrorCode::InvalidArgument, "runs along the Z axis");
    }
    SECTION("an X edge that is not straight")
    {
        s.wcs.zFace = top;
        s.wcs.xEdge = resolver->makeRef(body('b', 4), findEdge(f.shape(4), [](const kernel::CurveInfo& c) {
                                            return c.type == kernel::CurveType::Circle;
                                        }));
        fails(ErrorCode::InvalidArgument, "is not straight (it is an arc)");
    }
    SECTION("a Z face that is not flat")
    {
        s.wcs.zFace = resolver->makeRef(body('b', 4), findFace(f.shape(4), [](const kernel::SurfaceInfo& i) {
                                            return i.type == kernel::SurfaceType::Cylinder;
                                        }));
        fails(ErrorCode::InvalidArgument, "is not flat (it is a cylinder)");
    }
    SECTION("references of the wrong kind")
    {
        s.wcs.zFace = s.wcs.xEdge = resolver->makeRef(b1, kernel::SubId{kernel::SubKind::Edge, 0});
        fails(ErrorCode::InvalidArgument, "The Z face of setup 'Setup 1' must be a face, not an edge");
        s.wcs.zFace.reset();
        s.wcs.xEdge = top;
        fails(ErrorCode::InvalidArgument, "must be an edge");
    }
    SECTION("a reference that does not resolve")
    {
        naming::TopoRef broken = top;
        broken.names = {Hash256{{1, 2, 3}}};
        broken.families = {Hash256{}};
        broken.ambiguous = {false};
        s.wcs.zFace = broken;
        fails(ErrorCode::ReferenceBroken, "The Z face of setup 'Setup 1'");
    }
    SECTION("origin choices")
    {
        s.wcs.originMode = Wcs::OriginMode::ModelPoint;
        fails(ErrorCode::InvalidArgument, "no vertex is picked");
        s.wcs.originMode = Wcs::OriginMode::StockCorner;
        s.wcs.stockCornerIndex = 8;
        fails(ErrorCode::InvalidArgument, "must be 0 to 7");
    }
    SECTION("bodies that are not built, and box stock with nothing to size it from")
    {
        auto partial = std::make_shared<doc::BuildResult>(*f.build);
        partial->bodies.erase(b1);
        const CamSnapshot without = doc::camSnapshot(f.state, partial);
        const auto r2 = naming::makeResolver(without.bodies);
        const Error e = resolveFails(without, s, *r2);
        CHECK(e.code == ErrorCode::ReferenceBroken);
        CHECK_THAT(e.message, ContainsSubstring("Part body 1 of setup 'Setup 1'"));
        s.partBodies.clear();
        fails(ErrorCode::InvalidArgument, "has no part bodies");
        s.stock = CylinderStock{10, 10, 2, 0};
        fails(ErrorCode::InvalidArgument, "has no part bodies");
        s.stock = SolidStock{body('b', 7)};
        fails(ErrorCode::ReferenceBroken, "The stock body of setup 'Setup 1'");
    }
    SECTION("stock definitions that make no stock")
    {
        s.stock = BoxStock{-50, 0, 0, 0, 0, 0};
        fails(ErrorCode::InvalidArgument, "leave no stock along X");
        s.stock = CylinderStock{0, 10, 2, 0};
        fails(ErrorCode::InvalidArgument, "diameter and a length above 0");
        s.stock = CylinderStock{10, 10, 3, 0};
        fails(ErrorCode::InvalidArgument, "axis of X, Y or Z");
    }
}

TEST_CASE("box, cylinder and solid stock produce the expected bounds", "[cam][setup]")
{
    Block f;
    f.body(2, boxShape(Vec3{-5, -5, -5}, Vec3{50, 30, 20})); // a stock body
    const CamSnapshot snap = f.snap();
    const auto resolver = naming::makeResolver(snap.bodies);
    const BodyId b1 = body('b', 1);
    const Box3 part{Vec3{0, 0, 0}, Vec3{40, 20, 10}};
    Setup s = snap.cam->setups.at(0);
    s.wcs.originMode = Wcs::OriginMode::StockCorner;
    s.wcs.stockCornerIndex = 0;

    SECTION("box stock: the part box in the WCS orientation, grown by the offsets along the WCS axes")
    {
        const BoxStock offsets{1, 2, 3, 4, 5, 6};
        s.stock = offsets;
        // Model axes first, then a side face as Z (Z = -Y, Y = +Z, X = +X): the offsets follow the WCS axes.
        for (const bool side : {false, true}) {
            INFO((side ? "side WCS" : "model axes"));
            if (side) {
                s.wcs.zFace = resolver->makeRef(b1, planeFace(f.shape(1), Vec3{20, 0, 5}));
            }
            const ResolvedSetup rs = resolveOk(snap, s, *resolver, SetupResolveOptions{true});
            const Axes a = axesOf(rs);
            const Box3 inWcsOrientation = boxIn(cornersOfModelBox(part), a.x, a.y, a.z);
            const Box3 stock = grown(inWcsOrientation, offsets);
            const Vec3 origin = cornerOf(stock, 0);
            INFO("stock " << text(rs.stockBounds) << " part " << text(rs.partBounds));
            CHECK(nearBox(rs.stockBounds, shifted(stock, origin), 0));
            CHECK(nearBox(rs.partBounds, shifted(inWcsOrientation, origin), 0));
            CHECK(nearBox(kernel::bounds(rs.stockSolid), rs.stockBounds, 1e-9));
            if (side) {
                CHECK(same3(extent(rs.stockBounds), Vec3{43, 17, 31}));
            } else {
                CHECK(same3(extent(rs.stockBounds), Vec3{43, 27, 21}));
            }
        }
    }
    SECTION("cylinder stock along each WCS axis")
    {
        for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
            INFO("axis " << axisIndex);
            s.stock = CylinderStock{50, 60, axisIndex, 2};
            const ResolvedSetup rs = resolveOk(snap, s, *resolver, SetupResolveOptions{true});
            // Centred on the part across the axis; the + end 2 beyond the part, 60 long.
            const double centre[] = {20, 10, 5};
            const double top[] = {42, 22, 12};
            Box3 stock{};
            double* lo[] = {&stock.min.x, &stock.min.y, &stock.min.z};
            double* hi[] = {&stock.max.x, &stock.max.y, &stock.max.z};
            for (int i = 0; i < 3; ++i) {
                *lo[i] = i == axisIndex ? top[i] - 60 : centre[i] - 25;
                *hi[i] = i == axisIndex ? top[i] : centre[i] + 25;
            }
            const Vec3 origin = cornerOf(stock, 0);
            CHECK(nearBox(rs.stockBounds, shifted(stock, origin), 0));
            REQUIRE_FALSE(rs.stockSolid.isNull());
            CHECK(nearBox(kernel::bounds(rs.stockSolid), rs.stockBounds, 1e-9));
            auto mass = kernel::massProps(rs.stockSolid);
            REQUIRE(mass);
            CHECK(std::abs(mass->volume - kPi * 25 * 25 * 60) < 1e-6 * mass->volume);
        }
        // Along WCS Z of a side setup (Z = model -Y): the bar runs along model Y.
        s.wcs.zFace = resolver->makeRef(b1, planeFace(f.shape(1), Vec3{20, 0, 5}));
        s.stock = CylinderStock{50, 60, 2, 2};
        const ResolvedSetup rs = resolveOk(snap, s, *resolver, SetupResolveOptions{true});
        CHECK(same3(extent(rs.stockBounds), Vec3{50, 50, 60}));
        // Back in the model: across model x and z centred on the part, along model y from 2 before the face at y = 0
        // to 58 behind it.
        auto back = kernel::transformed(rs.stockSolid, rs.modelFromWcs);
        REQUIRE(back);
        CHECK(nearBox(kernel::bounds(back->shape), Box3{Vec3{-5, -2, -20}, Vec3{45, 58, 30}}, 1e-6));
    }
    SECTION("solid stock: the body as it lies in the WCS")
    {
        s.stock = SolidStock{body('b', 2)};
        const ResolvedSetup modelAxes = resolveOk(snap, s, *resolver, SetupResolveOptions{true});
        const Box3 stockModel{Vec3{-5, -5, -5}, Vec3{50, 30, 20}};
        CHECK(nearBox(modelAxes.stockBounds, shifted(stockModel, cornerOf(stockModel, 0)), 0));
        CHECK(nearBox(modelAxes.partBounds, shifted(part, cornerOf(stockModel, 0)), 0));
        s.wcs.zFace = resolver->makeRef(b1, planeFace(f.shape(1), Vec3{20, 0, 5}));
        const ResolvedSetup side = resolveOk(snap, s, *resolver, SetupResolveOptions{true});
        const Axes a = axesOf(side);
        const Box3 oriented = boxIn(cornersOfModelBox(stockModel), a.x, a.y, a.z);
        CHECK(nearBox(side.stockBounds, shifted(oriented, cornerOf(oriented, 0)), 0));
        CHECK(nearBox(kernel::bounds(side.stockSolid), side.stockBounds, 1e-9));
        CHECK(std::abs(kernel::massProps(side.stockSolid)->volume - 55.0 * 35 * 25) < 1e-6);
        // With the model frame itself (origin at the model origin) the stock solid is the body, unchanged.
        s.wcs.zFace.reset();
        s.wcs.originMode = Wcs::OriginMode::ModelPoint;
        s.wcs.originVertex =
            resolver->makeRef(b1, findVertex(f.shape(1), [](Vec3 p) { return same3(p, Vec3{0, 0, 0}); }));
        const ResolvedSetup at0 = resolveOk(snap, s, *resolver, SetupResolveOptions{true});
        CHECK(same3(apply(at0.wcsFromModel, Vec3{3, 4, 5}), Vec3{3, 4, 5}));
        CHECK(at0.stockSolid.contentHash() == f.shape(2).contentHash());
    }
    SECTION("stock from an earlier setup needs that setup simulated first (T-075: the engine carries it)")
    {
        s.stock = FromSetupStock{s.id};
        const Error e = resolveFails(snap, s, *resolver);
        CHECK(e.code == ErrorCode::InvalidArgument);
        CHECK_THAT(e.message, ContainsSubstring("must be simulated first"));
    }
}

TEST_CASE("fixture bodies are carried into ResolvedGeometry and change neither stock nor frame", "[cam][setup]")
{
    Block f;
    f.body(3, boxShape(Vec3{-200, -200, -50}, Vec3{200, 200, -1})); // a vice base under and around the part
    f.state = exec(f.state, opAdd(1, 1, 1));
    const auto withFixture =
        exec(f.state, command("cam.setup.edit", R"({"id":")" + id('a', 1) + R"(","fixtureBodies":[")" + id('b', 3) +
                                                    R"("],"stock":{"type":"box","zPos":1}})"));
    const auto plain =
        exec(f.state, command("cam.setup.edit", R"({"id":")" + id('a', 1) + R"(","stock":{"type":"box","zPos":1}})"));
    const auto resolveIn = [&](const doc::DocStatePtr& state, const doc::BuildResultPtr& build) {
        const CamSnapshot snap = doc::camSnapshot(state, build);
        const auto resolver = naming::makeResolver(snap.bodies);
        JobContext ctx;
        const Setup& setup = snap.cam->setups.at(0);
        auto setupResult = resolveSetup(snap, setup, *resolver, ctx);
        auto geom =
            resolveSetupGeometry(GeometryRequest{snap, setup, snap.cam->operations.at(opId(1)), *resolver}, ctx);
        return std::make_pair(std::move(setupResult), std::move(geom));
    };
    auto [fixtureSetup, fixtureGeom] = resolveIn(withFixture, f.build);
    auto [plainSetup, plainGeom] = resolveIn(plain, f.build);
    REQUIRE(fixtureSetup);
    REQUIRE(plainSetup);
    CHECK(hashOf(*fixtureSetup) == hashOf(*plainSetup));
    REQUIRE(fixtureGeom);
    REQUIRE(plainGeom);
    CHECK(fixtureGeom->fixtureShapes.size() == 1);
    CHECK(fixtureGeom->fixtureShapes.at(0).contentHash() == f.shape(3).contentHash());
    CHECK(plainGeom->fixtureShapes.empty());
    CHECK(fixtureGeom->partShapes.size() == 1);
    CHECK(nearBox(fixtureGeom->stockBounds, fixtureSetup->stockBounds, 0));
    for (int i = 0; i < 12; ++i) {
        CHECK(fixtureGeom->wcsFromModel.m[i] == fixtureSetup->wcsFromModel.m[i]);
    }
    CHECK_FALSE(fixtureGeom->stockSlices.has_value());
    // A fixture body that is not built breaks the operation's geometry, not the setup's frame.
    auto noFixture = std::make_shared<doc::BuildResult>(*f.build);
    noFixture->bodies.erase(body('b', 3));
    auto [setupWithout, geomWithout] = resolveIn(withFixture, noFixture);
    CHECK(setupWithout.has_value());
    REQUIRE_FALSE(geomWithout);
    CHECK(geomWithout.error().code == ErrorCode::ReferenceBroken);
    CHECK_THAT(geomWithout.error().message, ContainsSubstring("Fixture body 1"));
}

TEST_CASE("all eight stock corners and the top centre are correct on an imported reference part", "[cam][setup]")
{
    // tests/corpus raycad-synthetic/l_bracket_1: an 80 x 50 base 8 thick and a 60 high upright at x = 0..8, with
    // holes, slots and an inner fillet (tools/corpus_gen lBracket), imported through the document like a user's file.
    dt::RebuildEnv env;
    doc::ImportRequest request;
    request.feature = dt::featureId(1);
    request.path = std::string(RAY_CORPUS_DIR) + "/raycad-synthetic/l_bracket_1.stp";
    JobContext importCtx;
    auto prepared = doc::prepareImport(request, importCtx);
    if (!prepared) {
        FAIL("prepareImport failed: " << prepared.error().message);
    }
    for (const auto& [hash, bytes] : prepared->objects) {
        REQUIRE(env.pool->put(bytes));
    }
    auto state = exec(std::make_shared<const doc::DocState>(), prepared->command);
    REQUIRE(state->bodies.size() == 1);
    const BodyId part = state->bodies.at(0).id;
    const BoxStock offsets{1, 2, 3, 4, 5, 6};
    state = exec(state, command("cam.library.embed", library(toolJson(1), {}, machineJson(1))));
    state = exec(state, setupAdd(1, "\"" + part.value.toString() + "\"",
                                 R"(,"stock":{"type":"box","xNeg":1,"xPos":2,"yNeg":3,"yPos":4,"zNeg":5,"zPos":6})"));
    const auto build = env.rebuild(state);
    const CamSnapshot snap = doc::camSnapshot(state, build);
    const auto resolver = naming::makeResolver(snap.bodies);
    const kernel::Shape& shape = build->bodies.at(part).shape;
    const Box3 model{Vec3{0, 0, 0}, Vec3{80, 50, 60}};
    REQUIRE(nearBox(kernel::bounds(shape), model, 1e-6));
    Setup s = snap.cam->setups.at(0);

    // Checks corners 0..7 and the top centre of the stock in the frame with axes x, y, z: each origin lands where the
    // numbering of cam.h puts it, on the stock box made from the part box in that orientation.
    const auto checkOrigins = [&](Vec3 x, Vec3 y, Vec3 z) {
        const Box3 stock = grown(boxIn(cornersOfModelBox(model), x, y, z), offsets);
        for (int k = 0; k <= 8; ++k) {
            INFO((k < 8 ? "corner " + std::to_string(k) : std::string("top centre")));
            s.wcs.originMode = k < 8 ? Wcs::OriginMode::StockCorner : Wcs::OriginMode::StockTopCenter;
            s.wcs.stockCornerIndex = k < 8 ? k : 0;
            const ResolvedSetup rs = resolveOk(snap, s, *resolver);
            checkAxes(rs, x, y, z);
            const Vec3 o = k < 8 ? cornerOf(stock, k) : topCentreOf(stock);
            // The origin in model coordinates, and where the WCS puts it.
            const Vec3 originModel{x.x * o.x + y.x * o.y + z.x * o.z, x.y * o.x + y.y * o.y + z.y * o.z,
                                   x.z * o.x + y.z * o.y + z.z * o.z};
            INFO("origin " << text(originModel) << " stock " << text(rs.stockBounds));
            CHECK(near3(apply(rs.wcsFromModel, originModel), Vec3{0, 0, 0}, 1e-9));
            CHECK(near3(apply(rs.modelFromWcs, Vec3{0, 0, 0}), originModel, 1e-9));
            CHECK(nearBox(rs.stockBounds, shifted(stock, o), 1e-9));
        }
    };

    SECTION("in the model axes")
    {
        checkOrigins(Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1});
        // The default origin, corner 0, is the top front left corner of the stock: (-1, -3, 66) in the model.
        s.wcs.originMode = Wcs::OriginMode::StockCorner;
        s.wcs.stockCornerIndex = 0;
        CHECK(same3(apply(resolveOk(snap, s, *resolver).wcsFromModel, Vec3{-1, -3, 66}), Vec3{0, 0, 0}));
    }
    SECTION("Z from the upright's outer face, X from its bottom edge")
    {
        const auto face = findFace(shape, [](const kernel::SurfaceInfo& i) {
            return i.type == kernel::SurfaceType::Plane && std::abs(i.bounds.min.x) < 1e-6 &&
                   std::abs(i.bounds.max.x) < 1e-6;
        });
        const auto edge = findEdge(shape, [](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && std::abs(c.start.x) < 1e-6 && std::abs(c.end.x) < 1e-6 &&
                   std::abs(c.start.z) < 1e-6 && std::abs(c.end.z) < 1e-6;
        });
        s.wcs.zFace = resolver->makeRef(part, face);
        s.wcs.xEdge = resolver->makeRef(part, edge);
        const auto c = kernel::curveInfo(shape, edge);
        REQUIRE(c);
        const double sign = c->end.y > c->start.y ? 1 : -1;
        // Z out of the material at x = 0 is model -X; X along the edge is model +-Y; Y = Z x X.
        const Vec3 z{-1, 0, 0};
        const Vec3 x{0, sign, 0};
        checkOrigins(x, crossOf(z, x), z);
        // Pinned: the frame and bounds of corner 0 here, bit for bit, on every platform (cam.h rounding rule).
        s.wcs.originMode = Wcs::OriginMode::StockCorner;
        s.wcs.stockCornerIndex = 0;
        const ResolvedSetup rs = resolveOk(snap, s, *resolver);
        CHECK(hashOf(rs).hex() == "f7c16ace5918da68a91c595514bf8e6e8173aca793c921cdd9a173895279afa4");
    }
    SECTION("a vertex of the part as origin")
    {
        s.wcs.originMode = Wcs::OriginMode::ModelPoint;
        s.wcs.originVertex =
            resolver->makeRef(part, findVertex(shape, [](Vec3 p) { return near3(p, Vec3{80, 50, 0}, 1e-6); }));
        const ResolvedSetup rs = resolveOk(snap, s, *resolver);
        CHECK(near3(apply(rs.wcsFromModel, Vec3{80, 50, 0}), Vec3{0, 0, 0}, 1e-9));
        CHECK(nearBox(rs.partBounds, Box3{Vec3{-80, -50, 0}, Vec3{0, 0, 60}}, 1e-9));
    }
}

TEST_CASE("setup resolution is bit-identical run to run, and pinned across platforms", "[cam][setup]")
{
    // The tilted block of the WCS-orientation test: directions that are not model axes, bounds measured on the turned
    // body. Its result is pinned, so a platform that rounds differently fails here.
    Scene f;
    f.body(1, turned(boxShape(Vec3{0, 0, 0}, Vec3{40, 20, 10}), 0, kPi / 6));
    f.setup(R"(,"stock":{"type":"box","xNeg":1.25,"xPos":2,"yNeg":3,"yPos":4,"zNeg":5,"zPos":0.1})");
    const CamSnapshot snap = f.snap();
    const auto resolver = naming::makeResolver(snap.bodies);
    Setup s = snap.cam->setups.at(0);
    s.wcs.zFace = resolver->makeRef(body('b', 1), tiltedTop(f.shape(1)));
    s.wcs.originMode = Wcs::OriginMode::StockTopCenter;
    const Hash256 first = hashOf(resolveOk(snap, s, *resolver));
    for (int i = 0; i < 3; ++i) {
        CHECK(hashOf(resolveOk(snap, s, *resolver)) == first);
    }
    // Bounds and origin are whole nanometres in the WCS.
    const ResolvedSetup rs = resolveOk(snap, s, *resolver);
    for (const double v : {rs.stockBounds.min.x, rs.stockBounds.min.y, rs.stockBounds.min.z, rs.stockBounds.max.x,
                           rs.stockBounds.max.y, rs.stockBounds.max.z, rs.partBounds.min.z, rs.partBounds.max.y}) {
        CHECK(toMm(toNm(v)) == v);
    }
    CHECK(first.hex() == "54894a2c9bd975809b916886e301db9452499acd5f7a4b2ed1d2e8c2c32f53e0");
}

// ---------- Flip X and flip Z (T-044) ----------

TEST_CASE("flip X and flip Z reverse their axes and keep the frame right-handed", "[cam][setup][flip]")
{
    Block f;
    const CamSnapshot snap = f.snap();
    const auto resolver = naming::makeResolver(snap.bodies);
    const BodyId b1 = body('b', 1);
    const kernel::Shape& shape = f.shape(1);
    Setup s = snap.cam->setups.at(0);
    CHECK_FALSE(s.wcs.flipX); // a setup made without them
    CHECK_FALSE(s.wcs.flipZ);

    SECTION("no references")
    {
        s.wcs.flipX = true;
        checkAxes(resolveOk(snap, s, *resolver), Vec3{-1, 0, 0}, Vec3{0, -1, 0}, Vec3{0, 0, 1});
        s.wcs.flipX = false;
        s.wcs.flipZ = true; // looking up from below: X stays model X by its rule, Y turns over
        checkAxes(resolveOk(snap, s, *resolver), Vec3{1, 0, 0}, Vec3{0, -1, 0}, Vec3{0, 0, -1});
        s.wcs.flipX = true;
        checkAxes(resolveOk(snap, s, *resolver), Vec3{-1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, -1});
    }
    SECTION("the top face and an X edge: flip X turns the edge's direction round")
    {
        s.wcs.zFace = resolver->makeRef(b1, planeFace(shape, Vec3{20, 10, 10}));
        const auto edge = findEdge(shape, [](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && std::abs(c.start.x - 40) < 1e-9 &&
                   std::abs(c.end.x - 40) < 1e-9 && std::abs(c.start.z - 10) < 1e-9 && std::abs(c.end.z - 10) < 1e-9;
        });
        s.wcs.xEdge = resolver->makeRef(b1, edge);
        const double sign = kernel::curveInfo(shape, edge)->end.y > kernel::curveInfo(shape, edge)->start.y ? 1 : -1;
        checkAxes(resolveOk(snap, s, *resolver), Vec3{0, sign, 0}, Vec3{-sign, 0, 0}, Vec3{0, 0, 1});
        s.wcs.flipX = true;
        checkAxes(resolveOk(snap, s, *resolver), Vec3{0, -sign, 0}, Vec3{sign, 0, 0}, Vec3{0, 0, 1});
    }
    SECTION("the origin corner moves with the flipped axes")
    {
        // Corner 0 is the stock's (X min, Y min, Z max) in the WCS: with X and Y reversed it is the model's far corner.
        s.wcs.flipX = true;
        const ResolvedSetup rs = resolveOk(snap, s, *resolver);
        CHECK(same3(apply(rs.modelFromWcs, Vec3{0, 0, 0}), Vec3{40, 20, 10}));
        CHECK(nearBox(rs.stockBounds, Box3{Vec3{0, 0, -10}, Vec3{40, 20, 0}}, 0));
        s.wcs.flipX = false;
        s.wcs.flipZ = true; // Z down: the "top" is the model's bottom face, and Y is model -Y
        const ResolvedSetup down = resolveOk(snap, s, *resolver);
        CHECK(same3(apply(down.modelFromWcs, Vec3{0, 0, 0}), Vec3{0, 20, 0}));
    }
    SECTION("a flip is bit-identical run to run")
    {
        s.wcs.flipX = true;
        s.wcs.flipZ = true;
        const Hash256 first = hashOf(resolveOk(snap, s, *resolver));
        CHECK(hashOf(resolveOk(snap, s, *resolver)) == first);
    }
}

TEST_CASE("the flips are stored only when set, read as false when absent, and are in the input hash",
          "[cam][setup][flip]")
{
    using ray::doc::camjson::readWcs;
    using ray::doc::camjson::Source;
    using ray::doc::camjson::toJson;
    Block f;
    f.state = exec(f.state, opAdd(1, 1, 1));

    SECTION("the codec")
    {
        const Source stored{Source::Kind::Stored, "CAM object"};
        Wcs wcs;
        CHECK(doc::json::write(toJson(wcs)).find("flip") == std::string::npos); // as before T-044
        wcs.flipX = true;
        const std::string withX = doc::json::write(toJson(wcs));
        CHECK(withX.find("\"flipX\":true") != std::string::npos);
        CHECK(withX.find("flipZ") == std::string::npos);
        auto parsed = doc::json::parse(withX);
        REQUIRE(parsed);
        auto back = readWcs(*parsed, stored, "wcs");
        REQUIRE(back);
        CHECK(back->flipX);
        CHECK_FALSE(back->flipZ);
        // A document written before the flips existed.
        auto old = doc::json::parse(R"({"origin":{"mode":"stockCorner","corner":2},"workOffset":3})");
        REQUIRE(old);
        auto oldWcs = readWcs(*old, stored, "wcs");
        REQUIRE(oldWcs);
        CHECK_FALSE(oldWcs->flipX);
        CHECK_FALSE(oldWcs->flipZ);
        CHECK(oldWcs->stockCornerIndex == 2);
        // Not a boolean.
        auto bad = doc::json::parse(R"({"flipZ":1,"workOffset":1})");
        REQUIRE(bad);
        CHECK_FALSE(readWcs(*bad, stored, "wcs"));
    }
    SECTION("the commands and the input hash")
    {
        const auto hashOfOp = [&](const doc::DocStatePtr& state) {
            const CamSnapshot snap = doc::camSnapshot(state, f.build);
            const auto resolver = naming::makeResolver(snap.bodies);
            auto h = operationInputHash(snap, *resolver, opId(1));
            REQUIRE(h);
            return *h;
        };
        const Hash256 plain = hashOfOp(f.state);
        const std::string setup = R"({"id":")" + id('a', 1) + R"(","wcs":)";
        const doc::DocStatePtr flippedX =
            exec(f.state, command("cam.setup.edit", setup + R"({"flipX":true,"workOffset":1}})"));
        REQUIRE(flippedX->cam->setups.at(0).wcs.flipX);
        const Hash256 x = hashOfOp(flippedX);
        CHECK(x != plain);
        const doc::DocStatePtr flippedZ =
            exec(f.state, command("cam.setup.edit", setup + R"({"flipZ":true,"workOffset":1}})"));
        CHECK(hashOfOp(flippedZ) != plain);
        CHECK(hashOfOp(flippedZ) != x);
        // Back to unflipped, written as false or left out: the hash a setup had before the flips existed.
        const doc::DocStatePtr back =
            exec(flippedX, command("cam.setup.edit", setup + R"({"flipX":false,"workOffset":1}})"));
        CHECK_FALSE(back->cam->setups.at(0).wcs.flipX);
        CHECK(hashOfOp(back) == plain);
        // The work offset is G-code only: not in the hash.
        CHECK(hashOfOp(exec(f.state, command("cam.setup.edit", setup + R"({"workOffset":2}})"))) == plain);
        // Refused: an unknown member of the WCS.
        const Error e = applyFails(f.state, command("cam.setup.edit", setup + R"({"flipY":true}})"));
        CHECK(e.code == ErrorCode::InvalidArgument);
    }
}
