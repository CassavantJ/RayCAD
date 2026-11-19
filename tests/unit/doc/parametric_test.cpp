// The parametric feature types (task T-096): datum plane, sketch, extrude, revolve, fillet and chamfer, built through
// their commands and the rebuilder.
#include "parametric_test_util.h"

#include "features/extrude_feature.h"
#include "features/sketch_feature.h"
#include "features/sketch_json.h"
#include "rebuild.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using namespace ray;
using namespace ray::doc;
using namespace ray::doc::test;

namespace {

// Applies commands to a snapshot with the built-in command handlers (no document, no background rebuild).
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
    BuildResultPtr build() { return env.rebuild(state); }
};

const std::string kXY = R"({"principal":"xy"})";

// Sketch 2 (on XY): a 40 x 20 rectangle at the origin; extrude 3: 10 mm, new body 'b'.
void block(Model& m, const std::string& distance = "10")
{
    m.run(addFeature("sketch", 2, sketchProps(kXY, SketchJson().rect(1, 0, 0, 40, 20))));
    m.run(addFeature("extrude", 3, extrudeProps(2, distance, "new", 'b')));
}

} // namespace

TEST_CASE("an extrude of a sketched rectangle makes a block named from the sketch entities", "[doc][parametric]")
{
    Model m;
    block(m);
    const BuildResultPtr build = m.build();
    CHECK(report(*build, 2).status == FeatureStatus::Ok);
    REQUIRE(report(*build, 3).status == FeatureStatus::Ok);
    CHECK(volumeOf(*build, 'b') == Catch::Approx(8000.0).epsilon(1e-12));
    const BuiltBody& body = bodyOf(*build, 'b');
    CHECK(body.shape.count(kernel::SubKind::Face) == 6);
    // Four side faces named from the four lines, two caps from the region; every element has a name.
    std::map<std::string, int> roles;
    for (std::uint32_t i = 0; i < 6; ++i) {
        const auto names = body.map.namesOf(kernel::SubId{kernel::SubKind::Face, i});
        REQUIRE(names.size() == 1);
        const naming::TopoName* record = body.map.record(names[0]);
        REQUIRE(record != nullptr);
        ++roles[record->role];
        CHECK(record->origin == featureId(3));
    }
    CHECK(roles == std::map<std::string, int>{{"extrude.endcap", 1}, {"extrude.side", 4}, {"extrude.startcap", 1}});
    CHECK(body.map.size() == 6 + 12 + 8);
    // The document lists the new body.
    REQUIRE(m.state->bodies.size() == 1);
    CHECK(m.state->bodies[0].id == bodyId('b'));
    CHECK(m.state->bodies[0].name == "Body 1");
    CHECK(m.state->features[1].name == "Extrude 1");
}

TEST_CASE("without picked regions a nested outline is a hole; picked regions sweep exactly those", "[doc][parametric]")
{
    Model m;
    // A rectangle, a circle inside it and a smaller circle inside that (an island in the hole).
    SketchJson s;
    s.rect(1, 0, 0, 40, 20).circleAt(10, 20, 10, 6).circleAt(12, 20, 10, 3);
    m.run(addFeature("sketch", 2, sketchProps(kXY, s)));
    SECTION("all regions: the plate and the pin are two solids, which one body cannot be")
    {
        m.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
        const BuildResultPtr b = m.build();
        INFO(report(*b, 3).message);
        CHECK(report(*b, 3).status == FeatureStatus::Failed);
    }
    SECTION("all regions without the pin: a plate with a hole")
    {
        m.run(cmd("sketch.entity.remove", R"({"id":")" + uuid('f', 2) + R"(","entities":[")" + ent(13) + R"("]})"));
        m.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
        CHECK(volumeOf(*m.build(), 'b') == Catch::Approx(8000.0 - kPi * 36 * 10).epsilon(1e-9));
    }
    SECTION("the ring between the circles only")
    {
        m.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b', R"(,"regions":[)" + ids({11}) + "]")));
        CHECK(volumeOf(*m.build(), 'b') == Catch::Approx(kPi * (36 - 9) * 10).epsilon(1e-9));
    }
}

TEST_CASE("the curve tags of the doc match those closedLoops gives", "[doc][parametric]")
{
    sketch::Sketch s;
    auto pt = [&](std::uint64_t n, double x, double y) {
        s.entities.push_back(sketch::Entity{SketchEntId{Uuid::fromString(ent(n))}, sketch::Point{Vec2{x, y}}, false});
    };
    pt(1, 0, 0);
    pt(2, 10, 0);
    pt(3, 30, 0);
    s.entities.push_back(sketch::Entity{
        SketchEntId{Uuid::fromString(ent(4))},
        sketch::Slot{SketchEntId{Uuid::fromString(ent(1))}, SketchEntId{Uuid::fromString(ent(2))}, 4.0}, false});
    s.entities.push_back(sketch::Entity{SketchEntId{Uuid::fromString(ent(5))},
                                        sketch::Circle{SketchEntId{Uuid::fromString(ent(3))}, 3.0}, false});
    auto loops = sketch::closedLoops(s);
    REQUIRE(loops);
    std::set<std::uint64_t> tags;
    for (const auto& l : *loops) {
        for (const auto& c : l.curves) {
            tags.insert(c.tag);
        }
    }
    const SketchEntId slot{Uuid::fromString(ent(4))};
    std::set<std::uint64_t> expected{features::entityCurveTag(SketchEntId{Uuid::fromString(ent(5))})};
    for (int part = 0; part < 4; ++part) {
        expected.insert(features::slotPartCurveTag(slot, part));
    }
    CHECK(tags == expected);
    std::optional<std::string> error;
    const auto regions = features::sketchRegions(s, &error);
    CHECK(!error);
    REQUIRE(regions.size() == 2);
    for (const auto& r : regions) {
        CHECK(r.area2 > 0);
        CHECK(r.outerEntities.size() == 1);
    }
}

TEST_CASE("join, cut, intersect, reverse, symmetric and draft extrudes have the expected volumes", "[doc][parametric]")
{
    Model m;
    block(m);
    // Sketch 4 on a datum 10 mm up: a 10 x 10 square at (5, 5).
    m.run(addFeature("datum_plane", 4, R"({"base":{"principal":"xy"},"offset":"10 mm"})"));
    m.run(addFeature("sketch", 5,
                     sketchProps(R"({"datum":")" + uuid('f', 4) + R"("})", SketchJson().rect(1, 5, 5, 15, 15))));
    SECTION("join")
    {
        m.run(addFeature("extrude", 6, extrudeProps(5, "5", "join", 'b')));
        CHECK(volumeOf(*m.build(), 'b') == Catch::Approx(8000.0 + 500.0).epsilon(1e-9));
    }
    SECTION("cut, reverse")
    {
        m.run(addFeature("extrude", 6, extrudeProps(5, "\"4 mm\"", "cut", 'b', R"(,"direction":"reverse")")));
        CHECK(volumeOf(*m.build(), 'b') == Catch::Approx(8000.0 - 400.0).epsilon(1e-9));
    }
    SECTION("cut, symmetric: half of it is above the block")
    {
        m.run(addFeature("extrude", 6, extrudeProps(5, "8", "cut", 'b', R"(,"symmetric":true)")));
        CHECK(volumeOf(*m.build(), 'b') == Catch::Approx(8000.0 - 400.0).epsilon(1e-9));
    }
    SECTION("intersect")
    {
        m.run(addFeature("extrude", 6, extrudeProps(5, "30", "intersect", 'b', R"(,"direction":"reverse")")));
        CHECK(volumeOf(*m.build(), 'b') == Catch::Approx(1000.0).epsilon(1e-9));
    }
    SECTION("new body with a draft: a frustum")
    {
        m.run(addFeature("extrude", 6, extrudeProps(5, "5", "new", 'c', R"(,"draft":"10 deg")")));
        const BuildResultPtr build = m.build();
        REQUIRE(report(*build, 6).status == FeatureStatus::Ok);
        const double t = std::tan(10 * kPi / 180) * 5;
        const double a1 = 100.0, a2 = (10 - 2 * t) * (10 - 2 * t);
        CHECK(volumeOf(*build, 'c') == Catch::Approx(5.0 / 3.0 * (a1 + a2 + std::sqrt(a1 * a2))).epsilon(1e-9));
        CHECK(volumeOf(*build, 'b') == Catch::Approx(8000.0).epsilon(1e-12));
        CHECK(m.state->bodies.size() == 2);
    }
}

TEST_CASE("a revolve turns a region about a sketch line or a world axis", "[doc][parametric]")
{
    Model m;
    // On XZ: a rectangle from x 10..15, z 0..20 and a construction line on the z axis.
    SketchJson s;
    s.rect(1, 10, 0, 15, 20).point(20, 0, 0).point(21, 0, 10).line(22, 20, 21, true);
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xz"})", s)));
    SECTION("full turn about the sketch line: a tube")
    {
        m.run(addFeature("revolve", 3,
                         R"({"sketch":")" + uuid('f', 2) + R"(","axis":{"entity":")" + ent(22) +
                             R"("},"operation":"new","target":")" + uuid('b', 1) + R"("})"));
        const BuildResultPtr build = m.build();
        REQUIRE(report(*build, 3).status == FeatureStatus::Ok);
        CHECK(volumeOf(*build, 'b') == Catch::Approx(kPi * (225.0 - 100.0) * 20.0).epsilon(1e-9));
        // No caps on a full turn: every face is a side face of one of the four lines.
        const BuiltBody& body = bodyOf(*build, 'b');
        for (std::uint32_t i = 0; i < body.shape.count(kernel::SubKind::Face); ++i) {
            const auto names = body.map.namesOf(kernel::SubId{kernel::SubKind::Face, i});
            REQUIRE(!names.empty());
            CHECK(body.map.record(names[0])->role == "revolve.side");
        }
    }
    SECTION("a quarter turn about the world z axis, the other way")
    {
        m.run(addFeature("revolve", 3,
                         R"({"sketch":")" + uuid('f', 2) +
                             R"(","axis":{"principal":"z"},"angle":"90 deg","direction":"reverse","operation":"new",)"
                             R"("target":")" +
                             uuid('b', 1) + R"("})"));
        const BuildResultPtr build = m.build();
        REQUIRE(report(*build, 3).status == FeatureStatus::Ok);
        CHECK(volumeOf(*build, 'b') == Catch::Approx(kPi * (225.0 - 100.0) * 20.0 / 4).epsilon(1e-9));
    }
    SECTION("an axis that is not a line fails with a message")
    {
        m.run(addFeature("revolve", 3,
                         R"({"sketch":")" + uuid('f', 2) + R"(","axis":{"entity":")" + ent(20) +
                             R"("},"operation":"new","target":")" + uuid('b', 1) + R"("})"));
        const BuildResultPtr build = m.build();
        CHECK(report(*build, 3).status == FeatureStatus::Failed);
        CHECK(report(*build, 3).message.find("is not a line") != std::string::npos);
    }
}

TEST_CASE("fillet and chamfer blend referenced edges", "[doc][parametric]")
{
    Model m;
    block(m);
    const BuildResultPtr before = m.build();
    const kernel::Shape& shape = bodyOf(*before, 'b').shape;
    // The vertical edge at (40, 20) and the top edge along x at y = 0.
    const auto vertical = findEdge(shape, [](const kernel::CurveInfo& c) {
        return c.type == kernel::CurveType::Line && near(c.start.x, 40) && near(c.end.x, 40) && near(c.start.y, 20) &&
               near(c.end.y, 20);
    });
    const auto top = findEdge(shape, [](const kernel::CurveInfo& c) {
        return c.type == kernel::CurveType::Line && near(c.start.y, 0) && near(c.end.y, 0) && near(c.start.z, 10) &&
               near(c.end.z, 10);
    });
    REQUIRE(vertical);
    REQUIRE(top);
    const naming::TopoRef v = refTo(*before, 'b', *vertical);
    const naming::TopoRef t = refTo(*before, 'b', *top);
    m.run(addFeature("fillet", 4, R"({"edges":[)" + refJson(v) + R"(],"radius":"3 mm"})"));
    m.run(addFeature("chamfer", 5, R"({"edges":[)" + refJson(t) + R"(],"distance":2})"));
    const BuildResultPtr build = m.build();
    REQUIRE(report(*build, 4).status == FeatureStatus::Ok);
    REQUIRE(report(*build, 5).status == FeatureStatus::Ok);
    const double fillet = (1 - kPi / 4) * 9 * 10;
    const double chamfer = 2.0 * 40; // 2 x 2 / 2 along 40
    CHECK(volumeOf(*build, 'b') == Catch::Approx(8000.0 - fillet - chamfer).epsilon(1e-9));
    const naming::ElementMap& map = bodyOf(*build, 'b').map;
    std::set<std::string> roles;
    for (std::uint32_t i = 0; i < bodyOf(*build, 'b').shape.count(kernel::SubKind::Face); ++i) {
        for (const Hash256& n : map.namesOf(kernel::SubId{kernel::SubKind::Face, i})) {
            roles.insert(map.record(n)->role);
        }
    }
    CHECK(roles.count("fillet.gE_F") == 1);
    CHECK(roles.count("chamfer.gE_F") == 1);

    SECTION("editing the radius rebuilds only the fillet and the chamfer after it")
    {
        m.run(editFeature("fillet", 4, R"({"radius":"4 mm"})"));
        const BuildResultPtr again = m.build();
        CHECK(report(*again, 2).cacheHit);
        CHECK(report(*again, 3).cacheHit);
        CHECK(!report(*again, 4).cacheHit);
        CHECK(!report(*again, 5).cacheHit);
        CHECK(volumeOf(*again, 'b') == Catch::Approx(8000.0 - (1 - kPi / 4) * 16 * 10 - chamfer).epsilon(1e-9));
    }
    SECTION("an edge that is gone fails the fillet with a message naming it; later features are not built")
    {
        // The rectangle loses its corner at (40, 20): a corner cut (line 11) replaces it; lines 6 and 7 are shorter.
        m.run(cmd("sketch.set", R"({"id":")" + uuid('f', 2) + R"(","sketch":)" +
                                    SketchJson()
                                        .point(1, 0, 0)
                                        .point(2, 40, 0)
                                        .point(9, 40, 15)
                                        .point(10, 35, 20)
                                        .point(4, 0, 20)
                                        .line(5, 1, 2)
                                        .line(6, 2, 9)
                                        .line(11, 9, 10)
                                        .line(7, 10, 4)
                                        .line(8, 4, 1)
                                        .json() +
                                    "}"));
        const BuildResultPtr again = m.build();
        CHECK(report(*again, 3).status == FeatureStatus::Ok);
        const FeatureReport& r = report(*again, 4);
        CHECK(r.status == FeatureStatus::Failed);
        CHECK(r.message.find("Fillet 1: its edge no longer exists") == 0);
        CHECK(r.message.find("pick it again or remove it") != std::string::npos);
        CHECK(report(*again, 5).status == FeatureStatus::NotBuilt);
        // The body is the extrude's output: the document stays usable.
        CHECK(volumeOf(*again, 'b') == Catch::Approx(8000.0 - 12.5 * 10).epsilon(1e-9));
    }
}

TEST_CASE("a Repaired reference is never blended until the user confirms it", "[doc][parametric]")
{
    Model m;
    block(m);
    const BuildResultPtr before = m.build();
    const auto edge = findEdge(bodyOf(*before, 'b').shape, [](const kernel::CurveInfo& c) {
        return c.type == kernel::CurveType::Line && near(c.start.x, 40) && near(c.end.x, 40) && near(c.start.y, 20);
    });
    REQUIRE(edge);
    naming::TopoRef ref = refTo(*before, 'b', *edge);
    // A reference whose names this build never issued, with the right hint and neighbours: at best a suggestion.
    for (Hash256& n : ref.names) {
        n.bytes[0] ^= 0x5a;
    }
    for (Hash256& f : ref.families) {
        f.bytes[0] ^= 0x5a;
    }
    const naming::Resolved resolved = resolveOn(*before, 'b', ref);
    CHECK(!resolved.usable());
    m.run(addFeature("fillet", 4, R"({"edges":[)" + refJson(ref) + R"(],"radius":2})"));
    BuildResultPtr build = m.build();
    CHECK(report(*build, 4).status == FeatureStatus::Failed);
    CHECK(volumeOf(*build, 'b') == Catch::Approx(8000.0).epsilon(1e-12));
    if (resolved.state == naming::ResolveState::Repaired) {
        CHECK(report(*build, 4).message.find("confirm it in the reference review") != std::string::npos);
        ref.repairConfirmed = true;
        m.run(editFeature("fillet", 4, R"({"edges":[)" + refJson(ref) + "]}"));
        build = m.build();
        INFO(report(*build, 4).message);
        CHECK(report(*build, 4).status == FeatureStatus::Warning);
        CHECK(volumeOf(*build, 'b') == Catch::Approx(8000.0 - (1 - kPi / 4) * 4 * 10).epsilon(1e-9));
    } else {
        CHECK(report(*build, 4).message.find("no longer exists") != std::string::npos);
    }
}

TEST_CASE("parameters reach dimensions, distances and radii; only the features using one are rebuilt",
          "[doc][parametric]")
{
    Model m;
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","name":"w","expression":"40 mm","kind":"length"})"));
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 2) + R"(","name":"h","expression":"10 mm","kind":"length"})"));
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 3) + R"(","name":"r","expression":"2 mm","kind":"length"})"));
    SketchJson s;
    s.rect(1, 0, 0, 40, 20)
        .constraint(1, "fix", {1})
        .constraint(2, "horizontal", {5})
        .constraint(3, "horizontal", {7})
        .constraint(4, "vertical", {6})
        .constraint(5, "vertical", {8})
        .constraint(6, "distanceX", {1, 2}, "w")
        .constraint(7, "distanceY", {1, 4}, "20 mm");
    m.run(addFeature("sketch", 2, sketchProps(kXY, s)));
    m.run(addFeature("extrude", 3, extrudeProps(2, "\"h\"", "new", 'b')));
    const BuildResultPtr first = m.build();
    REQUIRE(report(*first, 3).status == FeatureStatus::Ok);
    const auto edge = findEdge(bodyOf(*first, 'b').shape, [](const kernel::CurveInfo& c) {
        return c.type == kernel::CurveType::Line && near(c.start.x, 0) && near(c.end.x, 0) && near(c.start.y, 0);
    });
    REQUIRE(edge);
    m.run(addFeature("fillet", 4, R"({"edges":[)" + refJson(refTo(*first, 'b', *edge)) + R"(],"radius":"r"})"));
    const BuildResultPtr built = m.build();
    REQUIRE(report(*built, 4).status == FeatureStatus::Ok);
    CHECK(volumeOf(*built, 'b') == Catch::Approx(8000.0 - (1 - kPi / 4) * 4 * 10).epsilon(1e-9));

    m.run(cmd("param.set", R"({"id":")" + uuid('a', 3) + R"(","expression":"3 mm"})"));
    BuildResultPtr b = m.build();
    CHECK(report(*b, 2).cacheHit);
    CHECK(report(*b, 3).cacheHit);
    CHECK(!report(*b, 4).cacheHit);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(8000.0 - (1 - kPi / 4) * 9 * 10).epsilon(1e-9));

    m.run(cmd("param.set", R"({"id":")" + uuid('a', 2) + R"(","expression":"12 mm"})"));
    b = m.build();
    CHECK(report(*b, 2).cacheHit);
    CHECK(!report(*b, 3).cacheHit);
    CHECK(!report(*b, 4).cacheHit);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(9600.0 - (1 - kPi / 4) * 9 * 12).epsilon(1e-9));

    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","expression":"50 mm"})"));
    b = m.build();
    CHECK(!report(*b, 2).cacheHit);
    CHECK(report(*b, 4).status == FeatureStatus::Ok);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(12000.0 - (1 - kPi / 4) * 9 * 12).epsilon(1e-9));
}

TEST_CASE("a sketch on a planar face follows the face", "[doc][parametric]")
{
    Model m;
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","name":"h","expression":"10 mm","kind":"length"})"));
    block(m, "\"h\"");
    const BuildResultPtr first = m.build();
    const auto topFace = findFace(bodyOf(*first, 'b').shape, [](const kernel::SurfaceInfo& s) {
        return s.type == kernel::SurfaceType::Plane && near(s.centroid.z, 10);
    });
    REQUIRE(topFace);
    const std::string plane = R"({"face":)" + refJson(refTo(*first, 'b', *topFace)) + "}";
    m.run(addFeature("sketch", 4, sketchProps(plane, SketchJson().circleAt(1, 20, 10, 5))));
    m.run(addFeature("extrude", 5, extrudeProps(4, "4", "cut", 'b', R"(,"direction":"reverse")")));
    BuildResultPtr b = m.build();
    REQUIRE(report(*b, 5).status == FeatureStatus::Ok);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(8000.0 - kPi * 25 * 4).epsilon(1e-9));
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","expression":"15 mm"})"));
    b = m.build();
    REQUIRE(report(*b, 5).status == FeatureStatus::Ok);
    CHECK(volumeOf(*b, 'b') == Catch::Approx(12000.0 - kPi * 25 * 4).epsilon(1e-9));
    const auto floor = findFace(bodyOf(*b, 'b').shape, [](const kernel::SurfaceInfo& s) {
        return s.type == kernel::SurfaceType::Plane && near(s.centroid.z, 11) && near(s.area, kPi * 25, 1e-6);
    });
    CHECK(floor);
}

TEST_CASE("sketch failures, suppression and reordering leave defined states", "[doc][parametric]")
{
    Model m;
    block(m);
    SECTION("conflicting dimensions fail the sketch; the extrude is not built")
    {
        m.run(cmd("sketch.constraint.add", R"({"id":")" + uuid('f', 2) + R"(","constraints":[{"id":")" + uuid('c', 1) +
                                               R"(","type":"distanceX","refs":[")" + ent(1) + R"(",")" + ent(2) +
                                               R"("],"value":40},{"id":")" + uuid('c', 2) +
                                               R"(","type":"distanceX","refs":[")" + ent(1) + R"(",")" + ent(2) +
                                               R"("],"value":30}]})"));
        const BuildResultPtr b = m.build();
        CHECK(report(*b, 2).status == FeatureStatus::Failed);
        CHECK(report(*b, 2).message.find("the constraints conflict") != std::string::npos);
        CHECK(report(*b, 3).status == FeatureStatus::NotBuilt);
        CHECK(report(*b, 3).message.find("sketch or datum") != std::string::npos);
        CHECK(b->bodies.empty());
    }
    SECTION("a suppressed sketch fails its extrude with a message")
    {
        m.run(cmd("feature.suppress", R"({"id":")" + uuid('f', 2) + R"(","suppressed":true})"));
        const BuildResultPtr b = m.build();
        CHECK(report(*b, 2).status == FeatureStatus::Suppressed);
        CHECK(report(*b, 3).status == FeatureStatus::Failed);
        CHECK(report(*b, 3).message.find("is suppressed") != std::string::npos);
    }
    SECTION("an extrude moved above its sketch fails until it is moved back")
    {
        REQUIRE(report(*m.build(), 3).status == FeatureStatus::Ok);
        m.run(cmd("feature.reorder", R"({"id":")" + uuid('f', 3) + R"(","index":0})"));
        BuildResultPtr b = m.build();
        CHECK(report(*b, 3).status == FeatureStatus::Failed);
        CHECK(report(*b, 3).message.find("comes after it in the history") != std::string::npos);
        CHECK(report(*b, 2).status == FeatureStatus::Ok);
        m.run(cmd("feature.reorder", R"({"id":")" + uuid('f', 3) + R"(","index":1})"));
        b = m.build();
        CHECK(report(*b, 3).status == FeatureStatus::Ok);
        CHECK(report(*b, 3).cacheHit);
    }
    SECTION("curves outside a closed outline are a warning")
    {
        m.run(cmd("sketch.entity.add", R"({"id":")" + uuid('f', 2) + R"(","entities":[)" + R"({"id":")" + ent(30) +
                                           R"(","type":"point","x":50,"y":0},)" + R"({"id":")" + ent(31) +
                                           R"(","type":"point","x":60,"y":5},)" + R"({"id":")" + ent(32) +
                                           R"(","type":"line","a":")" + ent(30) + R"(","b":")" + ent(31) + R"("}]})"));
        const BuildResultPtr b = m.build();
        CHECK(report(*b, 2).status == FeatureStatus::Warning);
        CHECK(report(*b, 2).message.find("1 curve is not part of a closed outline") != std::string::npos);
        CHECK(report(*b, 3).status == FeatureStatus::Ok);
    }
}

TEST_CASE("the typed commands check props with messages that name the field", "[doc][parametric]")
{
    Model m;
    block(m);
    auto message = [&](const Command& c) {
        return m.fails(c).message;
    };
    CHECK(message(addFeature("extrude", 4, extrudeProps(2, "\"10 mm\"", "join", 'b', R"(,"colour":1)"))) ==
          "Extrude 2: 'colour' is not a property of this feature");
    CHECK(message(addFeature("extrude", 4, extrudeProps(2, "\"depth\"", "join", 'b')))
              .find("Extrude 2: 'distance': Expression 'depth': unknown name 'depth'") == 0);
    CHECK(message(addFeature("extrude", 4, extrudeProps(2, "\"10\"", "join", 'b'))).find("add a unit") !=
          std::string::npos);
    CHECK(message(addFeature("extrude", 4, extrudeProps(3, "10", "join", 'b'))) ==
          "Extrude 2: 'Extrude 1' is not a sketch; pick a sketch.");
    const std::string noBody = message(addFeature("extrude", 4, extrudeProps(2, "10", "join", 'd')));
    INFO(noBody);
    CHECK(noBody.find("is not in the document") != std::string::npos);
    CHECK(message(addFeature("extrude", 4, extrudeProps(2, "10", "new", 'b'))).find("is already made by 'Extrude 1'") !=
          std::string::npos);
    CHECK(message(addFeature("extrude", 4, extrudeProps(2, "10", "join", 'b'), R"(,"index":0)"))
              .find("comes after it in the history") != std::string::npos);
    CHECK(message(addFeature("fillet", 4, R"({"edges":[],"radius":1})")) ==
          "Fillet 1: 'edges' must list at least one edge");
    CHECK(message(addFeature("sketch", 4, R"({"plane":{"principal":"ab"},"sketch":{}})")) ==
          "Sketch 2: 'plane.principal' must be one of xy, yz, xz");
    CHECK(message(cmd("feature.extrude.edit", R"({"id":")" + uuid('f', 2) + R"(","props":{"distance":1}})"))
              .find("names a feature of type sketch; use feature.sketch.edit") != std::string::npos);
    // An edit patches the props: null removes an optional key.
    m.run(editFeature("extrude", 3, R"({"symmetric":true})"));
    CHECK(m.state->features[1].propsJson.find("\"symmetric\":true") != std::string::npos);
    m.run(editFeature("extrude", 3, R"({"symmetric":null})"));
    CHECK(m.state->features[1].propsJson.find("symmetric") == std::string::npos);
}

TEST_CASE("TopoRef JSON round-trips", "[doc][parametric]")
{
    Model m;
    block(m);
    const BuildResultPtr b = m.build();
    const BuiltBody& body = bodyOf(*b, 'b');
    for (const kernel::SubKind kind : {kernel::SubKind::Face, kernel::SubKind::Edge, kernel::SubKind::Vertex}) {
        for (std::uint32_t i = 0; i < body.shape.count(kind); ++i) {
            const naming::TopoRef ref = refTo(*b, 'b', kernel::SubId{kind, i});
            const json::Value v = features::topoRefToJson(ref);
            const features::PropsReader r("test", v);
            auto back = features::topoRefFromJson(r, *json::parse(json::write(v)), "ref");
            REQUIRE(back);
            CHECK(back->body == ref.body);
            CHECK(back->kind == ref.kind);
            CHECK(back->names == ref.names);
            CHECK(back->families == ref.families);
            CHECK(back->ambiguous == ref.ambiguous);
            CHECK(back->splitParents == ref.splitParents);
            CHECK(back->neighbourNames == ref.neighbourNames);
            CHECK(back->hint == ref.hint);
            CHECK(json::write(features::topoRefToJson(*back)) == json::write(v));
            const naming::Resolved r1 = resolveOn(*b, 'b', *back);
            CHECK(r1.state == naming::ResolveState::Exact);
            CHECK(r1.elements == std::vector<kernel::SubId>{kernel::SubId{kind, i}});
        }
    }
}

TEST_CASE("a document saved before references carried pick-time evidence opens and rebuilds to the same geometry",
          "[doc][parametric]")
{
    // golden/old_toporef: the storage objects of a document saved by the code before TopoRef had territory and
    // pickNeighbours (a block, a fillet and a chamfer on picked edges, a sketch on a picked face and a cut from it),
    // with the geometry that code built (geometry.json). Its references load without evidence and resolve as they did
    // then; the rebuild gives the same body, and saving again writes the same bytes.
    const std::string dir = std::string(RAY_DOC_GOLDEN_DIR) + "/old_toporef";
    auto read = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        std::string s = text.str();
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        return s;
    };
    const std::string manifest = read(dir + "/manifest.json");
    REQUIRE(!manifest.empty());
    auto state = deserializeState(manifest, [&](const Hash256& h) -> Result<std::string> {
        const std::string bytes = read(dir + "/" + h.hex() + ".json");
        if (bytes.empty()) {
            return tl::unexpected(Error{ErrorCode::NotFound, "missing object " + h.hex(), {}});
        }
        return bytes;
    });
    REQUIRE(state.has_value());
    const auto loaded = std::make_shared<const DocState>(std::move(*state));
    REQUIRE(loaded->features.size() == 6);
    // The references are in the old form: no evidence.
    std::size_t refs = 0;
    for (const Feature& f : loaded->features) {
        const std::size_t at = f.propsJson.find("\"names\"");
        if (at == std::string::npos) {
            continue;
        }
        CHECK(f.propsJson.find("territory") == std::string::npos);
        CHECK(f.propsJson.find("pickNeighbours") == std::string::npos);
        ++refs;
    }
    CHECK(refs == 3);
    // Saved again unchanged: the same manifest (so the same feature objects).
    CHECK(serializeState(*loaded).manifest == manifest);

    RebuildEnv env(4);
    const BuildResultPtr b = env.rebuild(loaded);
    for (const Feature& f : loaded->features) {
        const FeatureReport& r = reportOf(*b, f.id);
        INFO(f.name << ": " << r.message);
        CHECK(r.status == FeatureStatus::Ok);
    }
    const BuiltBody& body = bodyOf(*b, 'b');
    const auto geometry = json::parse(read(dir + "/geometry.json"));
    REQUIRE(geometry.has_value());
    auto mp = kernel::massProps(body.shape);
    REQUIRE(mp);
    CHECK(mp->volume == Catch::Approx(geometry->find("volume")->asNumber()).epsilon(1e-9));
    CHECK(mp->area == Catch::Approx(geometry->find("area")->asNumber()).epsilon(1e-9));
    const Box3 box = kernel::bounds(body.shape);
    const auto& lo = geometry->find("min")->asArray();
    const auto& hi = geometry->find("max")->asArray();
    CHECK(box.min.x == Catch::Approx(lo[0].asNumber()).margin(1e-7));
    CHECK(box.min.y == Catch::Approx(lo[1].asNumber()).margin(1e-7));
    CHECK(box.min.z == Catch::Approx(lo[2].asNumber()).margin(1e-7));
    CHECK(box.max.x == Catch::Approx(hi[0].asNumber()).margin(1e-7));
    CHECK(box.max.y == Catch::Approx(hi[1].asNumber()).margin(1e-7));
    CHECK(box.max.z == Catch::Approx(hi[2].asNumber()).margin(1e-7));
    CHECK(body.shape.count(kernel::SubKind::Face) == static_cast<std::uint32_t>(geometry->find("faces")->asNumber()));
    CHECK(body.shape.count(kernel::SubKind::Edge) == static_cast<std::uint32_t>(geometry->find("edges")->asNumber()));
    CHECK(body.shape.count(kernel::SubKind::Vertex) ==
          static_cast<std::uint32_t>(geometry->find("vertices")->asNumber()));
}

TEST_CASE("the size of the pick-time evidence in a saved document (printed)", "[.toporef-size]")
{
    // The document of golden/old_toporef built again with references made now (with evidence): the storage objects'
    // sizes, against the old form's, and per reference.
    Model m;
    block(m);
    BuildResultPtr b = m.build();
    const auto vertical = findEdge(bodyOf(*b, 'b').shape, [](const kernel::CurveInfo& c) {
        return c.type == kernel::CurveType::Line && near(c.start.x, 40) && near(c.end.x, 40) && near(c.start.y, 20) &&
               near(c.end.y, 20);
    });
    REQUIRE(vertical);
    const naming::TopoRef r1 = refTo(*b, 'b', *vertical);
    m.run(addFeature("fillet", 4, R"({"edges":[)" + refJson(r1) + R"(],"radius":2})"));
    b = m.build();
    const auto front = findEdge(bodyOf(*b, 'b').shape, [](const kernel::CurveInfo& c) {
        return c.type == kernel::CurveType::Line && near(c.start.y, 0) && near(c.end.y, 0) && near(c.start.z, 10) &&
               near(c.end.z, 10);
    });
    REQUIRE(front);
    const naming::TopoRef r2 = refTo(*b, 'b', *front);
    m.run(addFeature("chamfer", 5, R"({"edges":[)" + refJson(r2) + R"(],"distance":1})"));
    b = m.build();
    const auto top = findFace(bodyOf(*b, 'b').shape, [](const kernel::SurfaceInfo& s) {
        return s.type == kernel::SurfaceType::Plane && near(s.centroid.z, 10, 1e-3) && s.area > 500;
    });
    REQUIRE(top);
    const naming::TopoRef r3 = refTo(*b, 'b', *top);
    m.run(addFeature("sketch", 6, sketchProps(R"({"face":)" + refJson(r3) + "}", SketchJson().circleAt(1, 20, 10, 4))));
    m.run(addFeature("extrude", 7, extrudeProps(6, "4", "cut", 'b', R"(,"direction":"reverse")")));
    const StateObjects objects = serializeState(*m.state);
    std::size_t total = objects.manifest.size();
    for (const auto& [hash, bytes] : objects.features) {
        total += bytes.size();
    }
    std::size_t oldTotal = 0;
    for (const auto& entry : std::filesystem::directory_iterator(std::string(RAY_DOC_GOLDEN_DIR) + "/old_toporef")) {
        if (entry.path().filename() != "geometry.json") {
            oldTotal += static_cast<std::size_t>(entry.file_size()) - 1; // each file ends in a newline
        }
    }
    for (const naming::TopoRef* r : {&r1, &r2, &r3}) {
        naming::TopoRef bare = *r;
        bare.territory = naming::Territory{};
        bare.pickNeighbours.clear();
        std::printf("reference (%s): %zu bytes with evidence (%zu neighbours, %zu points), %zu without\n",
                    features::subKindName(r->kind), naming::toJson(*r).size(), r->pickNeighbours.size(),
                    r->territory.points.size(), naming::toJson(bare).size());
    }
    std::printf("document: %zu bytes with evidence, %zu in the old form\n", total, oldTotal);
}

TEST_CASE("undo and redo of feature commands restore the snapshots and their builds", "[doc][parametric]")
{
    DocEnv env;
    env.run(addFeature("sketch", 2, sketchProps(kXY, SketchJson().rect(1, 0, 0, 40, 20))));
    env.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
    REQUIRE(env.settle());
    const std::string h1 = hashOf(env.doc->state());
    const std::string f1 = fingerprint(*env.doc->state(), *env.doc->build());
    env.run(editFeature("extrude", 3, R"({"distance":"25 mm"})"));
    REQUIRE(env.settle());
    CHECK(volumeOf(*env.doc->build(), 'b') == Catch::Approx(20000.0).epsilon(1e-12));
    REQUIRE(env.doc->undo());
    REQUIRE(env.settle());
    CHECK(hashOf(env.doc->state()) == h1);
    CHECK(fingerprint(*env.doc->state(), *env.doc->build()) == f1);
    for (const FeatureReport& r : env.doc->build()->reports) {
        CHECK(r.cacheHit);
    }
    REQUIRE(env.doc->redo());
    REQUIRE(env.settle());
    CHECK(volumeOf(*env.doc->build(), 'b') == Catch::Approx(20000.0).epsilon(1e-12));
    // Undo of the add removes the feature and the body entry it made.
    REQUIRE(env.doc->undo());
    REQUIRE(env.doc->undo());
    CHECK(env.doc->state()->bodies.empty());
    CHECK(env.doc->state()->features.size() == 1);
}

TEST_CASE("a parametric build is the same for 1 and 8 workers", "[doc][parametric]")
{
    auto buildWith = [](int workers) {
        Model m(workers);
        block(m);
        m.run(addFeature("datum_plane", 4, R"({"base":{"principal":"xy"},"offset":10})"));
        m.run(addFeature("sketch", 5,
                         sketchProps(R"({"datum":")" + uuid('f', 4) + R"("})", SketchJson().circleAt(1, 20, 10, 4))));
        m.run(addFeature("extrude", 6, extrudeProps(5, "6", "cut", 'b', R"(,"direction":"reverse")")));
        const BuildResultPtr b = m.build();
        return fingerprint(*m.state, *b);
    };
    CHECK(buildWith(1) == buildWith(8));
}
