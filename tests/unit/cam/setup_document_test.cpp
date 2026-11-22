// Setup resolution inside a document (task T-033): a WCS taken from a face and an edge follows the model when it is
// edited, rebuilt by the real rebuilder, and comes back bit for bit through undo and redo.
#include "engine_test_util.h"
#include "setup_test_util.h"

#include "parametric_test_util.h" // the doc tests' sketch and extrude builders (tests/unit/doc)

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

namespace dt = ray::doc::test;

// A quadrilateral 1 (0, 0), 2 (40, y2), 3 (40, 20), 4 (0, 20) with lines 5 (1-2), 6, 7, 8.
dt::SketchJson quad(double y2)
{
    return dt::SketchJson()
        .point(1, 0, 0)
        .point(2, 40, y2)
        .point(3, 40, 20)
        .point(4, 0, 20)
        .line(5, 1, 2)
        .line(6, 2, 3)
        .line(7, 3, 4)
        .line(8, 4, 1);
}

// The prism of quad(y2) extruded to `height`, as its eight corners.
std::array<Vec3, 8> prism(double y2, double height)
{
    const Vec2 base[] = {{0, 0}, {40, y2}, {40, 20}, {0, 20}};
    std::array<Vec3, 8> out;
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = Vec3{base[i].x, base[i].y, 0};
        out[i + 4] = Vec3{base[i].x, base[i].y, height};
    }
    return out;
}

Vec3 unitOf(Vec3 v)
{
    const double len = std::sqrt(dotOf(v, v));
    return Vec3{v.x / len, v.y / len, v.z / len};
}

} // namespace

TEST_CASE("a WCS from a face and an edge follows the model through edits, undo and redo", "[cam][setup][document]")
{
    dt::DocEnv env;
    const std::string xy = R"({"principal":"xy"})";
    env.run(dt::addFeature("sketch", 2, dt::sketchProps(xy, quad(0))));
    env.run(dt::addFeature("extrude", 3, dt::extrudeProps(2, "10", "new", 'b')));
    REQUIRE(env.settle());
    const BodyId b = dt::bodyId('b');

    // Z from the top face, X from its edge over sketch line 5 (from point 1 towards point 2).
    std::string zFace;
    std::string xEdge;
    double sign = 0; // the edge's curve direction along model X; it must keep it through the edits
    {
        const auto build = env.doc->build();
        const auto snap = doc::camSnapshot(env.doc->state(), build);
        const auto resolver = naming::makeResolver(snap.bodies);
        const kernel::Shape& shape = build->bodies.at(b).shape;
        zFace = naming::toJson(resolver->makeRef(b, findFace(shape, [](const kernel::SurfaceInfo& s) {
                                                     return s.type == kernel::SurfaceType::Plane &&
                                                            std::abs(s.centroid.z - 10) < 1e-9;
                                                 })));
        const auto edge = findEdge(shape, [](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && std::abs(c.start.z - 10) < 1e-9 &&
                   std::abs(c.end.z - 10) < 1e-9 && std::abs(c.start.y) < 1e-9 && std::abs(c.end.y) < 1e-9;
        });
        xEdge = naming::toJson(resolver->makeRef(b, edge));
        const auto info = kernel::curveInfo(shape, edge);
        REQUIRE(info);
        sign = info->end.x > info->start.x ? 1.0 : -1.0;
    }
    env.run(command("cam.library.embed", library(toolJson(1), {}, machineJson(1))));
    env.run(setupAdd(1, "\"" + b.value.toString() + "\"",
                     R"(,"stock":{"type":"box","xNeg":1,"xPos":2,"yNeg":3,"yPos":4,"zNeg":5,"zPos":6},"wcs":{"zFace":)" +
                         zFace + R"(,"xEdge":)" + xEdge + R"(,"origin":{"mode":"stockCorner","corner":0}})"));
    env.run(opAdd(1, 1, 1));
    REQUIRE(env.settle());

    // Resolves the setup of the document as it is now, checks it against the prism of quad(y2) at `height`, and
    // returns it with the operation's input hash.
    const BoxStock offsets{1, 2, 3, 4, 5, 6};
    const auto check = [&](double y2, double height) {
        INFO("quad(" << y2 << ") at height " << height);
        REQUIRE(env.doc->builtState() == env.doc->state());
        const auto snap = doc::camSnapshot(env.doc->state(), env.doc->build());
        const auto resolver = naming::makeResolver(snap.bodies);
        const ResolvedSetup rs = resolveOk(snap, snap.cam->setups.at(0), *resolver);
        checkFrame(rs);
        // Z is the top face's normal, X follows line 5 as it turns.
        const Vec3 x = axisOf(rs.wcsFromModel, 0);
        const Vec3 y = axisOf(rs.wcsFromModel, 1);
        const Vec3 z = axisOf(rs.wcsFromModel, 2);
        CHECK(same3(z, Vec3{0, 0, 1}));
        CHECK(near3(x, unitOf(Vec3{sign * 40, sign * y2, 0}), 1e-12));
        // The stock box of the prism in that orientation, grown by the offsets, with its corner 0 at the origin: the
        // origin rises with the top face (to its height plus zPos) and turns with the edge.
        const Box3 oriented = boxIn(prism(y2, height), x, y, z);
        const Box3 stock = grown(oriented, offsets);
        const Vec3 o = cornerOf(stock, 0);
        const Vec3 originModel{x.x * o.x + y.x * o.y + z.x * o.z, x.y * o.x + y.y * o.y + z.y * o.z,
                               x.z * o.x + y.z * o.y + z.z * o.z};
        INFO("stock " << text(rs.stockBounds) << ", origin " << text(apply(rs.modelFromWcs, Vec3{})));
        CHECK(near3(apply(rs.modelFromWcs, Vec3{}), originModel, 2e-6));
        CHECK(std::abs(originModel.z - (height + 6)) < 1e-9);
        CHECK(nearBox(rs.stockBounds, shifted(stock, o), 2e-6));
        CHECK(nearBox(rs.partBounds, shifted(oriented, o), 2e-6));
        auto hash = operationInputHash(snap, *resolver, opId(1));
        REQUIRE(hash);
        return std::make_pair(hashOf(rs), *hash);
    };

    const auto before = check(0, 10);

    // The edge turns (point 2 moves up to y = 10) and the face moves (height 10 -> 15), one undo step each.
    env.run(dt::editFeature("sketch", 2, R"({"sketch":)" + quad(10).json() + "}"));
    env.run(dt::editFeature("extrude", 3, R"({"distance":15})"));
    REQUIRE(env.settle());
    const auto after = check(10, 15);
    CHECK(after.first != before.first);
    CHECK(after.second != before.second); // and the operation's input hash saw it: the operation goes stale

    // Undo both edits: the frame is the first one again, bit for bit; redo: the edited one.
    REQUIRE(env.doc->undo());
    REQUIRE(env.doc->undo());
    REQUIRE(env.settle());
    CHECK(check(0, 10) == before);
    REQUIRE(env.doc->redo());
    REQUIRE(env.settle());
    const auto turnedOnly = check(10, 10);
    CHECK(turnedOnly.first != before.first);
    REQUIRE(env.doc->redo());
    REQUIRE(env.settle());
    CHECK(check(10, 15) == after);
}
