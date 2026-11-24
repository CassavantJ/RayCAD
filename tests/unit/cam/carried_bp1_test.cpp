// Acceptance of task T-075 on benchmark part 1 (PLAN.md 12.3), built here with the document's features as the
// templates test builds it (BP1 exists in the repository only as STEP files without threads): a 6.00 x 4.00 x 0.625 in
// 6061 plate with two closed pockets 0.375 deep and four 0.266 in through holes, from 6.25 x 4.25 x 0.75 in stock
// (1/16 in above and below the part). Setup 1, from the top: face, the two pockets (pocket2d), the holes (peck drill),
// the perimeter contoured full depth with four tabs. Setup 2, flipped onto its top (its Z face is the part's bottom),
// stock from setup 1: face to 0.625 in, chamfer the perimeter. The real simulator carries the stock
// (ray_api/cam_sim.h makeSetupSimulator, which the app gives its engine).
//   - Setup 2 starts from setup 1's simulated stock, turned over: the material is where setup 1 left it (sampled
//     inside the soup), and setup 2's stock is that soup moved through the model frame into its WCS, vertex for vertex.
//   - Changing a setup 1 operation makes setup 2's operations stale; regenerating setup 2 simulates setup 1 once.
//   - The toolpaths of both setups and the carried stock are the same bits for each of the three tier presets
//     (render::presetFor: the display's simulation cell, which an on-screen simulation of setup 1 uses meanwhile, and
//     nothing in CAM reads) and for 1, 4 and 16 workers.
//   - Hidden ([.measure]): time, size and memory of the carried stock on BP1 at kCarriedStockCellMm and two other
//   cells.
#include "engine_test_util.h"
#include "parametric_test_util.h"

#include <ray/cam.h>
#include <ray/render.h>
#include <ray/sim.h>
#include <ray_api/cam_sim.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first.
#include <psapi.h>
#endif

using namespace ray;
namespace dt = ray::doc::test;
namespace ct = ray::cam::test;

namespace {

constexpr double kIn = 25.4;
constexpr double L = 6 * kIn, W = 4 * kIn, T = 0.625 * kIn;
constexpr double kSkin = kIn / 16;  // stock above and below the part
constexpr double kMargin = kIn / 8; // stock beyond the part on each side

std::string mm(double v)
{
    return dt::num(v);
}

std::string toolItem(std::uint64_t n, const std::string& type, double diameter, double flute, const std::string& extra)
{
    return R"({"schema":1,"id":")" + ct::id('d', n) + R"(","name":"Tool )" + std::to_string(n) + R"(","type":")" +
           type + R"(","flutes":3,"diameter":)" + mm(diameter) + R"(,"shankDiameter":)" + mm(diameter) +
           R"(,"fluteLength":)" + mm(flute) + R"(,"overallLength":)" + mm(flute + 50) + extra + "}";
}

std::string machineItem()
{
    return R"({"schema":1,"id":")" + ct::id('9', 1) +
           R"(","name":"Mill","kind":"mill3","axes":[{"letter":"X","min":0,"max":762,"rapid":25400,"maxFeed":10000,)"
           R"("accel":3000},{"letter":"Y","min":0,"max":406,"rapid":25400,"maxFeed":10000,"accel":3000},)"
           R"({"letter":"Z","min":-508,"max":0,"rapid":25400,"maxFeed":10000,"accel":3000}],)"
           R"("spindle":{"maxRpm":10000},"toolChanger":{"capacity":20},)"
           R"("control":{"dialect":"generic-iso","defaultPost":"generic-iso","rigidTapping":true}})";
}

doc::Command opAdd(std::uint64_t n, std::uint64_t setup, const std::string& type, std::uint64_t tool,
                   const std::string& refs, const std::string& params, const std::string& heights)
{
    return ct::command("cam.operation.add",
                       R"({"id":")" + ct::id('c', n) + R"(","setup":")" + ct::id('a', setup) + R"(","type":")" + type +
                           R"(","name":"Op )" + std::to_string(n) + R"(","tool":{"tool":")" + ct::id('d', tool) +
                           R"(","holder":")" + ct::id('e', 1) +
                           R"(","stickOut":35},"cutting":{"rpm":8000,"feedCut":1000,"feedPlunge":300,"feedRamp":600},)"
                           R"("heights":)" +
                           heights + R"(,"geometry":[)" + refs + R"(],"params":)" + params + "}");
}

const char* kMillHeights = R"({"clearance":15,"retract":5,"feedPlane":2,"top":0,"bottom":0})";
const char* kDrillHeights = R"({"clearance":15,"retract":2,"feedPlane":1,"top":0,"bottom":-5})";

std::string ref(const doc::BuildResult& build, kernel::SubId sub)
{
    const doc::BuiltBody& body = dt::bodyOf(build, 'b');
    auto resolver = naming::makeResolver({naming::ResolverBody{dt::bodyId('b'), body.map, body.shape}});
    return naming::toJson(resolver->makeRef(dt::bodyId('b'), sub));
}

// The planar face of body b with normal `n` whose centroid is near `c` (within `tol` in each coordinate).
kernel::SubId planeFace(const kernel::Shape& shape, Vec3 n, Vec3 c, double tol)
{
    return ct::findFace(shape, [&](const kernel::SurfaceInfo& s) {
        Vec3 normal = s.axis;
        if (s.reversed) {
            normal = Vec3{-normal.x, -normal.y, -normal.z};
        }
        return s.type == kernel::SurfaceType::Plane && std::abs(normal.x - n.x) < 1e-9 &&
               std::abs(normal.y - n.y) < 1e-9 && std::abs(normal.z - n.z) < 1e-9 &&
               std::abs(s.centroid.x - c.x) < tol && std::abs(s.centroid.y - c.y) < tol &&
               std::abs(s.centroid.z - c.z) < tol;
    });
}

// The straight edges of body b lying at height z on the part's outline.
std::string outlineEdges(const doc::BuildResult& build, double z)
{
    const kernel::Shape& shape = dt::bodyOf(build, 'b').shape;
    std::string out;
    int n = 0;
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Edge); ++i) {
        const kernel::SubId e{kernel::SubKind::Edge, i};
        auto c = kernel::curveInfo(shape, e);
        if (!c || c->type != kernel::CurveType::Line || std::abs(c->start.z - z) > 1e-6 ||
            std::abs(c->end.z - z) > 1e-6) {
            continue;
        }
        const bool onX = std::abs(c->start.x) < 1e-6 && std::abs(c->end.x) < 1e-6;
        const bool onX1 = std::abs(c->start.x - L) < 1e-6 && std::abs(c->end.x - L) < 1e-6;
        const bool onY = std::abs(c->start.y) < 1e-6 && std::abs(c->end.y) < 1e-6;
        const bool onY1 = std::abs(c->start.y - W) < 1e-6 && std::abs(c->end.y - W) < 1e-6;
        if (onX || onX1 || onY || onY1) {
            out += (out.empty() ? "" : ",") + ref(build, e);
            ++n;
        }
    }
    REQUIRE(n == 4);
    return out;
}

// Pocket A and B (closed rectangles 0.375 in deep, where the corpus generator puts BP1's), and the holes.
struct Rect {
    double x0, y0, x1, y1;
};
const Rect kPocketA{L * 0.3 - L * 0.125, W * 0.5 - W * 0.225, L * 0.3 + L * 0.125, W * 0.5 + W * 0.225};
const Rect kPocketB{L * 0.7 - L * 0.1, W * 0.5 - W * 0.175, L * 0.7 + L * 0.1, W * 0.5 + W * 0.175};
constexpr double kPocketDepth = 0.375 * kIn;
const Vec2 kHoles[] = {{L * 0.08, W * 0.12}, {L * 0.92, W * 0.12}, {L * 0.08, W * 0.88}, {L * 0.92, W * 0.88}};
constexpr double kHoleDiameter = 0.266 * kIn;

std::string onTop()
{
    return R"({"datum":")" + dt::uuid('f', 4) + R"("})";
}

// The document of the header, built and with both setups. Setup 1 operations c1..c5, setup 2 c11, c12.
struct Bp1 {
    doc::DocStatePtr state;
    doc::BuildResultPtr build;
};

Bp1 makeBp1(dt::RebuildEnv& env)
{
    Bp1 b;
    auto s = std::make_shared<const doc::DocState>();
    s = ct::exec(
        s, dt::addFeature("sketch", 2, dt::sketchProps(R"({"principal":"xy"})", dt::SketchJson().rect(1, 0, 0, L, W))));
    s = ct::exec(s, dt::addFeature("extrude", 3, dt::extrudeProps(2, mm(T), "new", 'b')));
    s = ct::exec(s, dt::addFeature("datum_plane", 4, R"({"base":{"principal":"xy"},"offset":)" + mm(T) + "}"));
    s = ct::exec(s, dt::addFeature("sketch", 5,
                                   dt::sketchProps(onTop(), dt::SketchJson().rect(1, kPocketA.x0, kPocketA.y0,
                                                                                  kPocketA.x1, kPocketA.y1))));
    s = ct::exec(s, dt::addFeature("extrude", 6,
                                   dt::extrudeProps(5, mm(kPocketDepth), "cut", 'b', R"(,"direction":"reverse")")));
    s = ct::exec(s, dt::addFeature("sketch", 7,
                                   dt::sketchProps(onTop(), dt::SketchJson().rect(1, kPocketB.x0, kPocketB.y0,
                                                                                  kPocketB.x1, kPocketB.y1))));
    s = ct::exec(s, dt::addFeature("extrude", 8,
                                   dt::extrudeProps(7, mm(kPocketDepth), "cut", 'b', R"(,"direction":"reverse")")));
    dt::SketchJson points;
    for (std::uint64_t i = 0; i < 4; ++i) {
        points.point(i + 1, kHoles[i].x, kHoles[i].y);
    }
    s = ct::exec(s, dt::addFeature("sketch", 9, dt::sketchProps(onTop(), points)));
    s = ct::exec(s,
                 dt::addFeature("hole", 10,
                                R"({"sketch":")" + dt::uuid('f', 9) + R"(","target":")" + dt::uuid('b', 1) +
                                    R"(","kind":"simple","diameter":)" + mm(kHoleDiameter) + R"(,"end":"through"})"));
    b.build = env.rebuild(s);
    for (const auto& r : b.build->reports) {
        INFO(r.message);
        REQUIRE(r.status == doc::FeatureStatus::Ok);
    }
    const kernel::Shape& part = dt::bodyOf(*b.build, 'b').shape;
    const double floorZ = T - kPocketDepth;
    const std::string floorA =
        ref(*b.build, planeFace(part, {0, 0, 1},
                                {(kPocketA.x0 + kPocketA.x1) / 2, (kPocketA.y0 + kPocketA.y1) / 2, floorZ}, 1e-6));
    const std::string floorB =
        ref(*b.build, planeFace(part, {0, 0, 1},
                                {(kPocketB.x0 + kPocketB.x1) / 2, (kPocketB.y0 + kPocketB.y1) / 2, floorZ}, 1e-6));
    std::string holes;
    for (const Vec2& h : kHoles) {
        const kernel::SubId bore = ct::findFace(part, [&](const kernel::SurfaceInfo& si) {
            return si.type == kernel::SurfaceType::Cylinder && std::abs(si.origin.x - h.x) < 1e-6 &&
                   std::abs(si.origin.y - h.y) < 1e-6;
        });
        holes += (holes.empty() ? "" : ",") + ref(*b.build, bore);
    }
    const std::string bottom = ref(*b.build, planeFace(part, {0, 0, -1}, {L / 2, W / 2, 0}, 1e-3));
    const std::string outline = outlineEdges(*b.build, 0);

    s = ct::exec(s, ct::command("cam.library.embed",
                                ct::library(toolItem(1, "flatEndMill", 12.7, 25, "") + "," +
                                                toolItem(2, "flatEndMill", 6.35, 25, "") + "," +
                                                toolItem(3, "drill", kHoleDiameter, 40, R"(,"tipAngle":118)") + "," +
                                                toolItem(4, "chamferMill", 12.7, 10, R"(,"tipAngle":90)"),
                                            ct::holderJson(1), machineItem())));
    const std::string box = R"(,"stock":{"type":"box","xNeg":)" + mm(kMargin) + R"(,"xPos":)" + mm(kMargin) +
                            R"(,"yNeg":)" + mm(kMargin) + R"(,"yPos":)" + mm(kMargin) + R"(,"zNeg":)" + mm(kSkin) +
                            R"(,"zPos":)" + mm(kSkin) + "}";
    s = ct::exec(s, ct::setupAdd(1, "\"" + dt::uuid('b', 1) + "\"", box));
    s = ct::exec(s, opAdd(1, 1, "face", 1, "", R"({"stepdown":1})", kMillHeights));
    const char* rough = R"({"stepover":15,"stepdown":5,"stockToLeaveWall":0.25,"stockToLeaveFloor":0.13})";
    s = ct::exec(s, opAdd(2, 1, "adaptive2d", 2, floorA, rough, kMillHeights));
    s = ct::exec(s, opAdd(3, 1, "adaptive2d", 2, floorB, rough, kMillHeights));
    s = ct::exec(s, opAdd(4, 1, "drill", 3, holes, R"({"cycle":"peck","peck":4})", kDrillHeights));
    s = ct::exec(s, opAdd(5, 1, "contour2d", 2, outline,
                          R"({"side":"outside","maxStepdown":4,"tabs":true,"tabCount":4,"tabWidth":6,"tabHeight":2})",
                          kMillHeights));
    s = ct::exec(s, opAdd(6, 1, "pocket2d", 2, floorA, R"({"stepdown":10,"finishWalls":true})", kMillHeights));
    s = ct::exec(s, opAdd(7, 1, "pocket2d", 2, floorB, R"({"stepdown":10,"finishWalls":true})", kMillHeights));
    // Setup 2: turned over onto its top; its stock is what setup 1 leaves.
    s = ct::exec(s,
                 ct::setupAdd(2, "\"" + dt::uuid('b', 1) + "\"",
                              R"(,"stock":{"type":"fromSetup","setup":")" + ct::id('a', 1) + R"("},"wcs":{"zFace":)" +
                                  bottom + R"(,"origin":{"mode":"stockCorner","corner":0},"workOffset":2})"));
    s = ct::exec(s, opAdd(11, 2, "face", 1, "", R"({"stepdown":1})", kMillHeights));
    s = ct::exec(s, opAdd(12, 2, "chamfer2d", 4, outline, R"({"width":0.5,"tipOffset":0.3})", kMillHeights));
    // Not BP1's own, and here because it reads the carried stock's slices: the skin taken off by adaptive clearing
    // down to the part's face, as rest machining of what setup 1 left.
    s = ct::exec(s, opAdd(13, 2, "adaptive2d", 2, bottom, R"({"stepover":15,"stepdown":5})", kMillHeights));
    b.state = s;
    return b;
}

const std::vector<std::uint64_t> kSetup1{1, 2, 3, 4, 5, 6, 7};
const std::vector<std::uint64_t> kSetup2{11, 12, 13};

// An engine with the app's simulator, on `jobs`.
struct Engine {
    ray::test::TestDispatcher* ui;
    std::unique_ptr<cam::ICamEngine> engine;

    Engine(IJobSystem& jobs, ray::test::TestDispatcher& dispatcher) : ui(&dispatcher)
    {
        auto types = std::make_shared<cam::OperationRegistry>();
        cam::registerBuiltinOperationTypes(*types);
        cam::CamEngineOptions options;
        options.simulateSetup = api::cam_sim::makeSetupSimulator(jobs);
        engine = cam::makeCamEngine(jobs, types, &doc::camSnapshot, std::move(options));
        REQUIRE(engine);
    }

    template <class T>
    Result<T> wait(JobHandle<T> job)
    {
        std::optional<Result<T>> result;
        job.then([&](Result<T> r) { result = std::move(r); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        while (!result && std::chrono::steady_clock::now() < deadline) {
            ui->runPending();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(result.has_value());
        ui->runPending();
        return std::move(*result);
    }

    std::shared_ptr<const cam::Toolpath> generate(const Bp1& b, std::uint64_t op)
    {
        auto r = wait(engine->generate(b.state, b.build, ct::opId(op)));
        if (!r) {
            FAIL("operation " << op << " failed: " << r.error().message);
        }
        return *r;
    }

    cam::OpStatus statusOf(const Bp1& b, std::uint64_t op)
    {
        const auto all = engine->status(b.state, b.build);
        const auto it = all.find(ct::opId(op));
        REQUIRE(it != all.end());
        return it->second.status;
    }

    std::shared_ptr<const cam::CarriedStock> carried(const Bp1& b)
    {
        auto r = wait(engine->carriedStock(b.state, b.build, SetupId{Uuid::fromString(ct::id('a', 2))}));
        if (!r) {
            FAIL("the carried stock failed: " << r.error().message);
        }
        return *r;
    }
};

Hash256 soupHash(const kernel::TriSoup& s)
{
    Hasher h;
    h.i64(static_cast<std::int64_t>(s.positions.size()));
    for (const double v : s.positions) {
        h.f64(v);
    }
    for (const std::uint32_t i : s.indices) {
        h.i64(i);
    }
    return h.finish();
}

// Whether `p` lies inside the closed soup: crossings of the ray from `p` along +Z, counted on triangles whose XY
// projection contains p (sampled off the column grid, so the ray meets no edge).
bool insideSoup(const kernel::TriSoup& s, Vec3 p)
{
    int crossings = 0;
    for (std::size_t t = 0; t + 2 < s.indices.size(); t += 3) {
        const double* a = &s.positions[3 * s.indices[t]];
        const double* b = &s.positions[3 * s.indices[t + 1]];
        const double* c = &s.positions[3 * s.indices[t + 2]];
        const auto side = [&](const double* u, const double* v) {
            return (v[0] - u[0]) * (p.y - u[1]) - (v[1] - u[1]) * (p.x - u[0]);
        };
        const double d0 = side(a, b), d1 = side(b, c), d2 = side(c, a);
        const bool in = (d0 > 0 && d1 > 0 && d2 > 0) || (d0 < 0 && d1 < 0 && d2 < 0);
        if (!in) {
            continue;
        }
        // Height of the triangle's plane over p.
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        if (std::abs(nz) < 1e-12) {
            continue;
        }
        const double z = a[2] - (nx * (p.x - a[0]) + ny * (p.y - a[1])) / nz;
        if (z > p.z) {
            ++crossings;
        }
    }
    return crossings % 2 == 1;
}

Vec3 apply(const Xform& xf, Vec3 p)
{
    const double* m = xf.m;
    return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

std::uint64_t peakWorkingSet()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.PeakWorkingSetSize;
    }
#endif
    return 0;
}

} // namespace

TEST_CASE("BP1: setup 2 starts from setup 1's simulated stock, turned over; a setup 1 edit makes it stale",
          "[cam][carried][bp1]")
{
    dt::RebuildEnv env(8);
    const Bp1 b = makeBp1(env);
    Engine e(*env.jobs, env.ui);
    // Setup 2 first: its operations make setup 1's toolpaths and the carried stock on the way.
    std::map<std::uint64_t, std::shared_ptr<const cam::Toolpath>> paths;
    for (const std::uint64_t op : kSetup2) {
        paths[op] = e.generate(b, op);
    }
    for (const std::uint64_t op : kSetup1) {
        CHECK(e.statusOf(b, op) == cam::OpStatus::UpToDate); // filed by the carried stock's simulation
    }
    const auto carried = e.carried(b);
    REQUIRE(carried->solid);
    CHECK(carried->setup == SetupId{Uuid::fromString(ct::id('a', 1))});
    CHECK(carried->cellMm == cam::kCarriedStockCellMm);

    // The material where setup 1 left it, sampled in setup 1's WCS (origin at the stock's top front left corner:
    // the part spans X 3.175..155.575, Y 3.175..105.775, Z -17.4625..-1.5875), off the column grid.
    const kernel::TriSoup& end = *carried->solid;
    const double x0 = kMargin, y0 = kMargin, top = -kSkin, bottom = -kSkin - T;
    const auto at = [&](double x, double y, double z) {
        return insideSoup(end, Vec3{x0 + x + 0.0331, y0 + y + 0.0617, z});
    };
    CHECK(at(L / 2, W * 0.95, top - 1));                                       // the part
    CHECK_FALSE(at(L / 2, W * 0.95, top + 0.5));                               // faced off
    CHECK_FALSE(at((kPocketA.x0 + kPocketA.x1) / 2, W / 2, top - 2));          // inside pocket A
    CHECK(at((kPocketA.x0 + kPocketA.x1) / 2, W / 2, top - kPocketDepth - 1)); // under its floor
    CHECK_FALSE(at(kHoles[0].x, kHoles[0].y, top - T / 2));                    // a drilled hole
    CHECK_FALSE(at(-kMargin / 2, W / 3, top - 3));                             // the margin, cut away by the contour
    CHECK(at(-kMargin / 2, W / 3, bottom - kSkin / 2));                        // the skin under it, left for setup 2
    CHECK(at(L / 2, W / 2, bottom - kSkin / 2));

    // Setup 2, turned over about X: its frame, and its stock = setup 1's end moved through the model frame.
    const cam::CamSnapshot snap = doc::camSnapshot(b.state, b.build);
    const auto resolver = naming::makeResolver(snap.bodies);
    JobContext ctx;
    cam::SetupResolveOptions options;
    options.carried = carried.get();
    auto two = cam::resolveSetup(snap, snap.cam->setups.at(1), *resolver, ctx, options);
    REQUIRE(two);
    const double* r = two->wcsFromModel.m;
    CHECK((r[0] == 1 && r[1] == 0 && r[2] == 0 && r[4] == 0 && r[5] == -1 && r[6] == 0 && r[8] == 0 && r[9] == 0 &&
           r[10] == -1));
    // 6.25 x 4.25 in, as the stock was (the skin spans it); 0.6875 in high: the part and the skin under it.
    CHECK(two->stockBounds.min.x == 0.0);
    CHECK(two->stockBounds.max.x == Catch::Approx(L + 2 * kMargin).margin(1e-9));
    CHECK(two->stockBounds.min.y == 0.0);
    CHECK(two->stockBounds.max.y == Catch::Approx(W + 2 * kMargin).margin(1e-9));
    CHECK(two->stockBounds.max.z == 0.0);
    CHECK(two->stockBounds.min.z == Catch::Approx(-(T + kSkin)).margin(1e-5));
    REQUIRE(two->carriedStock);
    REQUIRE(two->carriedStock->positions.size() == end.positions.size());
    double worst = 0;
    for (std::size_t i = 0; i + 2 < end.positions.size(); i += 3) {
        const Vec3 model =
            apply(carried->modelFromWcs, Vec3{end.positions[i], end.positions[i + 1], end.positions[i + 2]});
        const Vec3 w = apply(two->wcsFromModel, model);
        worst = std::max({worst, std::abs(w.x - two->carriedStock->positions[i]),
                          std::abs(w.y - two->carriedStock->positions[i + 1]),
                          std::abs(w.z - two->carriedStock->positions[i + 2])});
    }
    CHECK(worst < 1e-9);
    // The face of setup 2 starts at the carried stock's top and stops at the part (WCS -1/16 in).
    Nm lowest = 0;
    for (const cam::Move& m : paths[11]->moves) {
        if (m.role == cam::MoveRole::Cut) {
            lowest = std::min(lowest, m.z);
        }
    }
    CHECK(lowest == toNm(-kSkin));

    // A setup 1 edit (pocket A's roughing stepover) makes setup 2 stale; regenerating it simulates setup 1 again.
    Bp1 edited = b;
    edited.state =
        ct::exec(b.state, ct::command("cam.operation.edit",
                                      R"({"id":")" + ct::id('c', 2) +
                                          R"(","params":{"stepover":12,"stepdown":5,"stockToLeaveWall":0.25,)"
                                          R"("stockToLeaveFloor":0.13}})"));
    for (const std::uint64_t op : kSetup2) {
        CHECK(e.statusOf(edited, op) == cam::OpStatus::Stale);
    }
    CHECK(e.statusOf(edited, 3) == cam::OpStatus::UpToDate); // the other pocket is not affected
    const auto again = e.carried(edited);
    CHECK(again->key != carried->key);
    // A name changes nothing.
    Bp1 renamed = b;
    renamed.state =
        ct::exec(b.state, ct::command("cam.operation.edit", R"({"id":")" + ct::id('c', 2) + R"(","name":"Pocket A"})"));
    for (const std::uint64_t op : kSetup2) {
        CHECK(e.statusOf(renamed, op) == cam::OpStatus::UpToDate);
    }
}

TEST_CASE("BP1: both setups' toolpaths and the carried stock are the same bits for every tier preset and 1, 4, 16 "
          "workers",
          "[cam][carried][bp1][tiers]")
{
    dt::RebuildEnv env(4);
    const Bp1 b = makeBp1(env);
    std::optional<std::map<std::uint64_t, Hash256>> first;
    std::optional<Hash256> firstStock;
    std::map<double, std::size_t> displayTriangles;
    for (const render::Tier tier : {render::Tier::Low, render::Tier::Medium, render::Tier::High}) {
        const render::TierPreset preset = render::presetFor(tier);
        for (const int workers : {1, 4, 16}) {
            INFO("tier " << static_cast<int>(tier) << " (cell " << preset.simCellMm << " mm), " << workers
                         << " workers");
            env.jobs->setWorkerCount(workers);
            Engine e(*env.jobs, env.ui); // a fresh engine: nothing cached
            std::map<std::uint64_t, Hash256> hashes;
            for (const std::uint64_t op : kSetup2) {
                hashes[op] = e.generate(b, op)->hash();
            }
            for (const std::uint64_t op : kSetup1) {
                hashes[op] = e.generate(b, op)->hash();
            }
            const auto carried = e.carried(b);
            const Hash256 stock = soupHash(*carried->solid);
            // What the tier does change: the on-screen simulation of setup 1 at the preset's cell (T-074's path: the
            // same builder), run here between the generations of other configurations. Once per preset.
            if (workers == 4) {
                std::vector<std::shared_ptr<const cam::Toolpath>> paths;
                for (const std::uint64_t op : kSetup1) {
                    paths.push_back(e.generate(b, op));
                }
                JobContext ctx;
                auto program =
                    api::cam_sim::buildSimProgram(b.state, b.build, SetupId{Uuid::fromString(ct::id('a', 1))}, paths,
                                                  api::cam_sim::ProgramOptions{false}, ctx);
                REQUIRE(program);
                auto simulator = sim::makeSimulator(*env.jobs, nullptr);
                sim::SimOptions run;
                run.resolution = sim::StockResolution{preset.simCellMm};
                run.safetyChecks = false;
                run.gouges = false;
                REQUIRE(simulator->run(program->program, run, ctx).has_value());
                displayTriangles[preset.simCellMm] = simulator->finalStock().indices.size() / 3;
            }
            if (!first) {
                first = hashes;
                firstStock = stock;
            } else {
                CHECK(hashes == *first);
                CHECK(stock == *firstStock);
            }
        }
    }
    env.jobs->setWorkerCount(4);
    // The display's stock did differ by tier; the CAM results did not.
    REQUIRE(displayTriangles.size() == 3);
    CHECK(displayTriangles[0.2] < displayTriangles[0.1]);
    CHECK(displayTriangles[0.1] < displayTriangles[0.05]);
}

TEST_CASE("BP1: time, size and memory of the carried stock at kCarriedStockCellMm and two other cells",
          "[.measure][cam][carried][bp1]")
{
    dt::RebuildEnv env(8);
    const Bp1 b = makeBp1(env);
    Engine e(*env.jobs, env.ui);
    std::vector<std::shared_ptr<const cam::Toolpath>> paths;
    std::size_t moves = 0;
    for (const std::uint64_t op : kSetup1) {
        paths.push_back(e.generate(b, op));
        moves += paths.back()->moves.size();
    }
    const cam::CamSnapshot snap = doc::camSnapshot(b.state, b.build);
    const auto resolver = naming::makeResolver(snap.bodies);
    JobContext ctx;
    auto one = cam::resolveSetup(snap, snap.cam->setups.at(0), *resolver, ctx, cam::SetupResolveOptions{true});
    REQUIRE(one);
    const cam::SimulateSetupFn simulate = api::cam_sim::makeSetupSimulator(*env.jobs);
    std::cout << "BP1 setup 1: " << moves << " IR moves, " << env.jobs->workerCount() << " workers\n";
    for (const double cell : {0.5, cam::kCarriedStockCellMm, 0.2}) {
        const std::uint64_t before = peakWorkingSet();
        const auto t0 = std::chrono::steady_clock::now();
        cam::SimulateSetupRequest request{snap, *resolver, snap.cam->setups.at(0), *one, paths, cell};
        auto soup = simulate(request, ctx);
        const auto t1 = std::chrono::steady_clock::now();
        REQUIRE(soup);
        const std::uint64_t after = peakWorkingSet();
        // Slicing it in setup 2 (what each of setup 2's operations does when it resolves).
        cam::CarriedStock c;
        c.setup = snap.cam->setups.at(0).id;
        c.solid = std::make_shared<const kernel::TriSoup>(*soup);
        c.modelFromWcs = one->modelFromWcs;
        c.bounds = one->stockBounds;
        c.cellMm = cell;
        const auto t2 = std::chrono::steady_clock::now();
        cam::GeometryRequest g{snap, snap.cam->setups.at(1), snap.cam->operations.at(ct::opId(11)), *resolver, &c};
        auto geom = cam::resolveSetupGeometry(g, ctx);
        const auto t3 = std::chrono::steady_clock::now();
        REQUIRE(geom);
        const double bytes =
            static_cast<double>(soup->positions.size() * sizeof(double) + soup->indices.size() * sizeof(std::uint32_t));
        std::cout << "cell " << cell << " mm: simulate " << std::chrono::duration<double>(t1 - t0).count() << " s, "
                  << soup->indices.size() / 3 << " triangles, " << bytes / 1048576.0 << " MB of soup, peak working set "
                  << static_cast<double>(before) / 1048576.0 << " -> " << static_cast<double>(after) / 1048576.0
                  << " MB; resolve and slice in setup 2 " << std::chrono::duration<double>(t3 - t2).count() << " s, "
                  << geom->stockSlices->size() << " levels\n";
    }
}
