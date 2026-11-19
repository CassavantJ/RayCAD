// The hole feature and the thread table (task T-097): every kind against its closed-form volume, through holes on a
// stepped body, several holes from one sketch, stable names through sketch edits, the command messages, undo and
// redo, and the thread table's values and consistency.
#include "parametric_test_util.h"

#include "features/hole_feature.h"
#include "features/hole_standards.h"
#include "json.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>

using namespace ray;
using namespace ray::doc;
using namespace ray::doc::test;

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
    std::string fails(const Command& c)
    {
        const ICommandHandler* handler = builtinCommandRegistry()->find(c.name);
        REQUIRE(handler != nullptr);
        auto next = handler->apply(state, c.argsJson);
        if (next) {
            FAIL(c.name << " should have failed");
        }
        return next.error().message;
    }
    BuildResultPtr build() { return env.rebuild(state); }
};

const double kTan59 = std::tan(59.0 * kPi / 180.0);

double cylinder(double r, double h)
{
    return kPi * r * r * h;
}
double cone(double r, double h)
{
    return kPi * r * r * h / 3;
}
double frustum(double R, double r, double h)
{
    return kPi * h / 3 * (R * R + R * r + r * r);
}
double tip(double r) // the 118 degree drill point
{
    return cone(r, r / kTan59);
}

// Sketch 2: a 40 x 20 rectangle; extrude 3: 10 mm, body 'b' (z 0..10). Datum 4 at z = 10.
void block(Model& m)
{
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", SketchJson().rect(1, 0, 0, 40, 20))));
    m.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
    m.run(addFeature("datum_plane", 4, R"({"base":{"principal":"xy"},"offset":10})"));
}

std::string onDatum(std::uint64_t datum)
{
    return R"({"datum":")" + uuid('f', datum) + R"("})";
}

std::string holeProps(std::uint64_t sketch, const std::string& rest)
{
    return R"({"sketch":")" + uuid('f', sketch) + R"(","target":")" + uuid('b', 1) + "\"," + rest + "}";
}

const std::string kM6 =
    R"("thread":{"designation":"M6x1","standard":"iso_metric_coarse","nominalDiameter":6,"pitch":1,"class":"6H",)"
    R"("tapDrill":5,"depth":6})";

// The wall of a hole: the cylinder of radius r whose axis passes through (x, y).
std::optional<kernel::SubId> wallAt(const kernel::Shape& shape, double x, double y, double r)
{
    return findFace(shape, [&](const kernel::SurfaceInfo& s) {
        return s.type == kernel::SurfaceType::Cylinder && near(s.radius, r) && near(s.origin.x, x) &&
               near(s.origin.y, y);
    });
}

} // namespace

TEST_CASE("every hole kind cuts its closed-form volume", "[doc][hole]")
{
    struct Case {
        const char* name;
        std::string props;
        double removed;
    };
    const double h5 = 2.7; // countersink 12 at 90 degrees on a 6.6 hole: (6 - 3.3) / tan 45
    const std::vector<Case> cases = {
        {"simple, blind, drill point", R"("kind":"simple","diameter":6,"end":"blind","depth":8)",
         cylinder(3, 8) + tip(3)},
        {"simple, blind, 90 degree point as an expression",
         R"("kind":"simple","diameter":"6 mm","end":"blind","depth":"6 mm","tipAngle":"90 deg")",
         cylinder(3, 6) + cone(3, 3)},
        {"simple, blind, flat", R"("kind":"simple","diameter":6,"end":"blind","depth":8,"bottom":"flat")",
         cylinder(3, 8)},
        {"simple, through", R"("kind":"simple","diameter":6,"end":"through")", cylinder(3, 10)},
        {"tapped M6x1, blind", R"("kind":"tapped","end":"blind","depth":8,)" + kM6, cylinder(2.5, 8) + tip(2.5)},
        {"counterbore, through",
         R"("kind":"counterbore","diameter":6.6,"end":"through","counterbore":{"diameter":11,"depth":6.5})",
         cylinder(5.5, 6.5) + cylinder(3.3, 3.5)},
        {"countersink, through", R"("kind":"countersink","diameter":6.6,"end":"through","countersink":{"diameter":12})",
         frustum(6, 3.3, h5) + cylinder(3.3, 10 - h5)},
        {"counterbore and tapped M8x1.25, blind",
         R"("kind":"counterbore_tapped","end":"blind","depth":7,"counterbore":{"diameter":14,"depth":3},)"
         R"("thread":{"designation":"M8x1.25","standard":"iso_metric_coarse","nominalDiameter":8,"pitch":1.25,)"
         R"("tapDrill":6.8})",
         cylinder(7, 3) + cylinder(3.4, 4) + tip(3.4)},
        {"countersink and tapped 1/4-20 UNC, blind, flat",
         R"("kind":"countersink_tapped","end":"blind","depth":8,"bottom":"flat",)"
         R"("countersink":{"diameter":10,"angle":"82 deg"},)"
         R"("thread":{"designation":"1/4-20 UNC","standard":"unc","nominalDiameter":"0.25 in","tpi":20,)"
         R"("tapDrill":"0.201 in"})",
         0},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name)
        {
            Model m;
            block(m);
            m.run(addFeature("sketch", 5, sketchProps(onDatum(4), SketchJson().point(1, 10, 10))));
            m.run(addFeature("hole", 6, holeProps(5, c.props)));
            const BuildResultPtr b = m.build();
            INFO(report(*b, 6).message);
            REQUIRE(report(*b, 6).status == FeatureStatus::Ok);
            double removed = c.removed;
            if (removed == 0) {
                const double r = 0.201 * 25.4 / 2, R = 5, h = (R - r) / std::tan(41.0 * kPi / 180.0);
                removed = frustum(R, r, h) + cylinder(r, 8 - h);
            }
            const double volume = volumeOf(*b, 'b');
            UNSCOPED_INFO("removed " << num(8000.0 - volume) << " expected " << num(removed));
            CHECK(std::abs(volume - (8000.0 - removed)) <= 1e-9 * (8000.0 - removed));
            // Every face of the body is named, the hole's from the role "hole".
            const BuiltBody& body = bodyOf(*b, 'b');
            int holeFaces = 0;
            for (std::uint32_t i = 0; i < body.shape.count(kernel::SubKind::Face); ++i) {
                const auto names = body.map.namesOf(kernel::SubId{kernel::SubKind::Face, i});
                REQUIRE(!names.empty());
                holeFaces += body.map.record(names[0])->role == "hole.side" ? 1 : 0;
            }
            CHECK(holeFaces >= 1);
        }
    }
}

TEST_CASE("through holes on a stepped body go through every step; several holes come from one sketch", "[doc][hole]")
{
    Model m;
    block(m);
    // A 20 x 20 x 5 step on the left half: z 0..15 for x < 20, z 0..10 beyond.
    m.run(addFeature("sketch", 5, sketchProps(onDatum(4), SketchJson().rect(1, 0, 0, 20, 20))));
    m.run(addFeature("extrude", 6, extrudeProps(5, "5", "join", 'b')));
    m.run(addFeature("datum_plane", 7, R"({"base":{"principal":"xy"},"offset":15})"));
    m.run(addFeature("sketch", 8, sketchProps(onDatum(7), SketchJson().point(1, 10, 10).point(2, 30, 10))));
    m.run(addFeature("hole", 9, holeProps(8, R"("kind":"simple","diameter":6,"end":"through")")));
    const BuildResultPtr b = m.build();
    INFO(report(*b, 9).message);
    REQUIRE(report(*b, 9).status == FeatureStatus::Ok);
    const double removed = cylinder(3, 15) + cylinder(3, 10);
    CHECK(std::abs(volumeOf(*b, 'b') - (10000.0 - removed)) <= 1e-9 * (10000.0 - removed));
    const kernel::Shape& shape = bodyOf(*b, 'b').shape;
    CHECK(wallAt(shape, 10, 10, 3));
    CHECK(wallAt(shape, 30, 10, 3));

    SECTION("flipped, the holes point away from the body")
    {
        m.run(editFeature("hole", 9, R"({"flip":true})"));
        const BuildResultPtr f = m.build();
        CHECK(report(*f, 9).status == FeatureStatus::Failed);
        CHECK(report(*f, 9).message.find("points away from the body; flip it") != std::string::npos);
    }
}

TEST_CASE("a hole keeps its names when its point moves or another point is deleted", "[doc][hole]")
{
    Model m;
    block(m);
    const auto sketchWith = [](double x1) {
        return SketchJson().point(1, x1, 10).point(2, 20, 10).point(3, 30, 10);
    };
    m.run(addFeature("sketch", 5, sketchProps(onDatum(4), sketchWith(10))));
    m.run(addFeature("hole", 6,
                     holeProps(5, R"("kind":"counterbore","diameter":4,"end":"blind","depth":7,)"
                                  R"("counterbore":{"diameter":7,"depth":2})")));
    const BuildResultPtr first = m.build();
    REQUIRE(report(*first, 6).status == FeatureStatus::Ok);
    const kernel::Shape& shape = bodyOf(*first, 'b').shape;
    std::vector<naming::TopoRef> walls, cbores;
    for (const double x : {10.0, 20.0, 30.0}) {
        const auto wall = wallAt(shape, x, 10, 2);
        const auto cbore = wallAt(shape, x, 10, 3.5);
        REQUIRE(wall);
        REQUIRE(cbore);
        walls.push_back(refTo(*first, 'b', *wall));
        cbores.push_back(refTo(*first, 'b', *cbore));
    }
    const auto floor = findFace(shape, [](const kernel::SurfaceInfo& s) {
        return s.type == kernel::SurfaceType::Plane && near(s.centroid.x, 30) && near(s.centroid.z, 8);
    });
    REQUIRE(floor);
    const naming::TopoRef floorRef = refTo(*first, 'b', *floor);

    SECTION("moving the first point moves its hole; every reference stays Exact")
    {
        m.run(cmd("sketch.set", R"({"id":")" + uuid('f', 5) + R"(","sketch":)" + sketchWith(6).json() + "}"));
        const BuildResultPtr b = m.build();
        REQUIRE(report(*b, 6).status == FeatureStatus::Ok);
        const naming::Resolved r = resolveOn(*b, 'b', walls[0]);
        CHECK(r.state == naming::ResolveState::Exact);
        REQUIRE(r.elements.size() == 1);
        auto info = kernel::surfaceInfo(bodyOf(*b, 'b').shape, r.elements[0]);
        REQUIRE(info);
        CHECK(near(info->origin.x, 6));
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(resolveOn(*b, 'b', walls[i]).state == naming::ResolveState::Exact);
            CHECK(resolveOn(*b, 'b', cbores[i]).state == naming::ResolveState::Exact);
        }
        CHECK(resolveOn(*b, 'b', floorRef).state == naming::ResolveState::Exact);
    }
    SECTION("deleting the middle point removes its hole; the others stay Exact")
    {
        m.run(cmd("sketch.entity.remove", R"({"id":")" + uuid('f', 5) + R"(","entities":[")" + ent(2) + R"("]})"));
        const BuildResultPtr b = m.build();
        REQUIRE(report(*b, 6).status == FeatureStatus::Ok);
        const double one = cylinder(3.5, 2) + cylinder(2, 5) + tip(2);
        CHECK(std::abs(volumeOf(*b, 'b') - (8000.0 - 2 * one)) <= 1e-9 * 8000.0);
        CHECK(resolveOn(*b, 'b', walls[1]).state == naming::ResolveState::Broken);
        for (const std::size_t i : {std::size_t{0}, std::size_t{2}}) {
            const naming::Resolved r = resolveOn(*b, 'b', walls[i]);
            CHECK(r.state == naming::ResolveState::Exact);
            CHECK(resolveOn(*b, 'b', cbores[i]).state == naming::ResolveState::Exact);
        }
        CHECK(resolveOn(*b, 'b', floorRef).state == naming::ResolveState::Exact);
    }
    SECTION("a listed point that is deleted fails the hole with a message naming it")
    {
        m.run(editFeature("hole", 6, R"({"points":[")" + ent(1) + R"(",")" + ent(3) + R"("]})"));
        REQUIRE(report(*m.build(), 6).status == FeatureStatus::Ok);
        m.run(cmd("sketch.entity.remove", R"({"id":")" + uuid('f', 5) + R"(","entities":[")" + ent(3) + R"("]})"));
        const BuildResultPtr b = m.build();
        const FeatureReport& r = report(*b, 6);
        CHECK(r.status == FeatureStatus::Failed);
        CHECK(r.message == "Hole 1: its point " + ent(3) +
                               " is no longer in the sketch 'Sketch 2'; pick the points again or remove it.");
        // The body is the extrude's: the document stays usable.
        CHECK(volumeOf(*b, 'b') == Catch::Approx(8000.0).epsilon(1e-12));
    }
}

TEST_CASE("the hole commands check props with messages that name the feature and the field", "[doc][hole]")
{
    Model m;
    block(m);
    m.run(addFeature(
        "sketch", 5,
        sketchProps(onDatum(4), SketchJson().point(1, 10, 10).point(2, 20, 10).point(3, 30, 15).line(4, 2, 3))));
    auto add = [&](const std::string& rest) {
        return m.fails(addFeature("hole", 6, holeProps(5, rest)));
    };
    const std::string simple = R"("kind":"simple","diameter":6,"end":"blind","depth":8)";
    CHECK(add(simple + R"(,"colour":1)") == "Hole 1: 'colour' is not a property of this feature");
    CHECK(add(R"("kind":"drilled","diameter":6,"end":"through")") ==
          "Hole 1: 'kind' must be one of simple, tapped, counterbore, countersink, counterbore_tapped, "
          "countersink_tapped");
    CHECK(add(R"("kind":"simple","end":"through")") == "Hole 1: 'diameter' is required for a simple hole");
    CHECK(add(R"("kind":"simple","diameter":6,"end":"blind")") == "Hole 1: 'depth' is required for a blind hole");
    CHECK(add(R"("kind":"tapped","diameter":5,"end":"through",)" + kM6) ==
          "Hole 1: 'diameter' is set by 'thread.tapDrill' for a tapped hole; leave it out");
    CHECK(add(R"("kind":"tapped","end":"through")") ==
          "Hole 1: 'thread' is required for a tapped hole (standards.threads.list gives the sizes)");
    CHECK(add(R"("kind":"counterbore","diameter":6,"end":"through")") ==
          "Hole 1: 'counterbore' is required for a counterbore hole: {\"diameter\", \"depth\"}");
    CHECK(add(R"("kind":"counterbore","diameter":6,"end":"through","counterbore":{"diameter":5,"depth":2})") ==
          "Hole 1: the counterbore diameter (5 mm) must be larger than the hole diameter (6 mm).");
    CHECK(add(R"("kind":"counterbore","diameter":6,"end":"blind","depth":4,"counterbore":{"diameter":9,"depth":4})") ==
          "Hole 1: the counterbore depth (4 mm) must be greater than 0 and less than the hole depth (4 mm).");
    CHECK(add(R"("kind":"countersink","diameter":6,"end":"blind","depth":2,"countersink":{"diameter":12})") ==
          "Hole 1: the countersink (3 mm deep) must be shallower than the hole depth (2 mm).");
    CHECK(add(R"("kind":"simple","diameter":6,"end":"blind","depth":8,"tipAngle":"180 deg")") ==
          "Hole 1: the tip angle must be more than 0 and less than 180 degrees.");
    CHECK(add(R"("kind":"simple","diameter":0,"end":"through")") ==
          "Hole 1: the hole diameter must be greater than 0.");
    CHECK(add(R"("kind":"simple","diameter":"dia","end":"through")")
              .find("Hole 1: 'diameter': Expression 'dia': unknown name 'dia'") == 0);
    CHECK(add(R"("kind":"tapped","end":"through","thread":{"designation":"M6x1","standard":"iso_metric_coarse",)"
              R"("nominalDiameter":6,"pitch":1,"tpi":25,"tapDrill":5})") ==
          "Hole 1: 'thread' must give exactly one of 'pitch' (metric) and 'tpi' (threads per inch)");
    CHECK(add(R"("kind":"tapped","end":"through","thread":{"designation":"M6x1","standard":"iso",)"
              R"("nominalDiameter":6,"pitch":1,"tapDrill":5})") ==
          "Hole 1: 'thread.standard' must be one of iso_metric_coarse, iso_metric_fine, unc, unf, custom");
    CHECK(add(R"("kind":"tapped","end":"through","thread":{"designation":"M6x1","standard":"custom",)"
              R"("nominalDiameter":6,"pitch":1,"tapDrill":6.2})") ==
          "Hole 1: the tap drill (6.2 mm) must be smaller than the thread's nominal diameter (6 mm).");
    CHECK(add(R"("kind":"counterbore_tapped","end":"blind","depth":8,"counterbore":{"diameter":11,"depth":3},)"
              R"("thread":{"designation":"M6x1","standard":"iso_metric_coarse","nominalDiameter":6,"pitch":1,)"
              R"("tapDrill":5,"depth":6})") ==
          "Hole 1: the thread depth (6 mm) is more than the tapped length of the hole (5 mm).");
    CHECK(add(simple + R"(,"points":[")" + ent(9) + R"("])") ==
          "Hole 1: its point " + ent(9) +
              " is no longer in the sketch 'Sketch 2'; pick the points again or remove it.");
    CHECK(add(simple + R"(,"points":[")" + ent(4) + R"("])") ==
          "Hole 1: " + ent(4) + " is not a point of the sketch 'Sketch 2'.");
    CHECK(add(simple + R"(,"points":[")" + ent(1) + R"(",")" + ent(1) + R"("])") ==
          "Hole 1: 'points[1]' lists the point " + ent(1) + " a second time");
    CHECK(m.fails(addFeature(
              "hole", 6, R"({"sketch":")" + uuid('f', 3) + R"(","target":")" + uuid('b', 1) + "\"," + simple + "}")) ==
          "Hole 1: 'Extrude 1' is not a sketch; pick a sketch.");
    CHECK(m.fails(addFeature("hole", 6,
                             R"({"sketch":")" + uuid('f', 5) + R"(","target":")" + uuid('d', 1) + "\"," + simple + "}"))
              .find("is not in the document; pick a body") != std::string::npos);

    // Free-standing points only: point 1 (point 2 and 3 are the line's ends).
    m.run(addFeature("hole", 6, holeProps(5, simple)));
    const BuildResultPtr b = m.build();
    REQUIRE(report(*b, 6).status == FeatureStatus::Ok);
    CHECK(std::abs(volumeOf(*b, 'b') - (8000.0 - cylinder(3, 8) - tip(3))) <= 1e-9 * 8000.0);
    // An option the kind does not use is kept and ignored.
    m.run(editFeature("hole", 6, R"({"counterbore":{"diameter":9,"depth":2}})"));
    CHECK(m.state->features.back().propsJson.find("counterbore") != std::string::npos);
    CHECK(std::abs(volumeOf(*m.build(), 'b') - (8000.0 - cylinder(3, 8) - tip(3))) <= 1e-9 * 8000.0);
    m.run(editFeature("hole", 6, R"({"kind":"counterbore"})"));
    CHECK(std::abs(volumeOf(*m.build(), 'b') - (8000.0 - cylinder(4.5, 2) - cylinder(3, 6) - tip(3))) <= 1e-9 * 8000.0);
}

TEST_CASE("parameters reach hole sizes; the thread spec is readable for CAM", "[doc][hole]")
{
    Model m;
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","name":"td","expression":"5 mm","kind":"length"})"));
    block(m);
    m.run(addFeature("sketch", 5, sketchProps(onDatum(4), SketchJson().point(1, 10, 10))));
    m.run(addFeature("hole", 6,
                     holeProps(5, R"("kind":"tapped","end":"blind","depth":8,"bottom":"flat","thread":{)"
                                  R"("designation":"M6x1","standard":"iso_metric_coarse","nominalDiameter":6,)"
                                  R"("pitch":"1 mm","class":"6H","tapDrill":"td"})")));
    BuildResultPtr b = m.build();
    REQUIRE(report(*b, 6).status == FeatureStatus::Ok);
    CHECK(std::abs(volumeOf(*b, 'b') - (8000.0 - cylinder(2.5, 8))) <= 1e-9 * 8000.0);
    m.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","expression":"5.1 mm"})"));
    b = m.build();
    CHECK(report(*b, 3).cacheHit);
    CHECK(!report(*b, 6).cacheHit);
    CHECK(std::abs(volumeOf(*b, 'b') - (8000.0 - cylinder(2.55, 8))) <= 1e-9 * 8000.0);

    const Feature& hole = m.state->features.back();
    auto thread = features::holeThreadOf(hole, {{"td", 5.1}});
    REQUIRE(thread);
    REQUIRE(thread->has_value());
    const features::HoleThread& t = **thread;
    CHECK(t.designation == "M6x1");
    CHECK(t.standard == "iso_metric_coarse");
    CHECK(t.threadClass == std::optional<std::string>("6H"));
    CHECK(t.nominalDiameter == 6.0);
    CHECK(t.pitch == 1.0);
    CHECK(!t.tpi);
    CHECK(t.tapDrill == 5.1);
    CHECK(!t.depth);
    // A hole that is not tapped has no thread.
    m.run(editFeature("hole", 6, R"({"kind":"simple","diameter":5})"));
    auto none = features::holeThreadOf(m.state->features.back(), {{"td", 5.1}});
    REQUIRE(none);
    CHECK(!none->has_value());
}

TEST_CASE("undo and redo of hole commands restore the snapshots and their builds", "[doc][hole]")
{
    DocEnv env;
    env.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", SketchJson().rect(1, 0, 0, 40, 20))));
    env.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
    env.run(addFeature("datum_plane", 4, R"({"base":{"principal":"xy"},"offset":10})"));
    env.run(addFeature("sketch", 5, sketchProps(onDatum(4), SketchJson().point(1, 10, 10).point(2, 30, 10))));
    env.run(addFeature("hole", 6, holeProps(5, R"("kind":"simple","diameter":6,"end":"through")")));
    REQUIRE(env.settle());
    const std::string h1 = hashOf(env.doc->state());
    const std::string f1 = fingerprint(*env.doc->state(), *env.doc->build());
    CHECK(std::abs(volumeOf(*env.doc->build(), 'b') - (8000.0 - 2 * cylinder(3, 10))) <= 1e-9 * 8000.0);
    env.run(editFeature("hole", 6, R"({"diameter":"8 mm"})"));
    REQUIRE(env.settle());
    CHECK(std::abs(volumeOf(*env.doc->build(), 'b') - (8000.0 - 2 * cylinder(4, 10))) <= 1e-9 * 8000.0);
    REQUIRE(env.doc->undo());
    REQUIRE(env.settle());
    CHECK(hashOf(env.doc->state()) == h1);
    CHECK(fingerprint(*env.doc->state(), *env.doc->build()) == f1);
    REQUIRE(env.doc->redo());
    REQUIRE(env.settle());
    CHECK(std::abs(volumeOf(*env.doc->build(), 'b') - (8000.0 - 2 * cylinder(4, 10))) <= 1e-9 * 8000.0);
    REQUIRE(env.doc->undo());
    REQUIRE(env.doc->undo());
    REQUIRE(env.settle());
    CHECK(env.doc->state()->features.size() == 4);
    CHECK(volumeOf(*env.doc->build(), 'b') == Catch::Approx(8000.0).epsilon(1e-12));
}

TEST_CASE("a hole build is the same for 1 and 8 workers", "[doc][hole]")
{
    auto buildWith = [](int workers) {
        Model m(workers);
        block(m);
        m.run(addFeature("sketch", 5, sketchProps(onDatum(4), SketchJson().point(1, 10, 10).point(2, 30, 10))));
        m.run(addFeature("hole", 6,
                         holeProps(5, R"("kind":"countersink","diameter":5,"end":"blind","depth":7,)"
                                      R"("countersink":{"diameter":9})")));
        const BuildResultPtr b = m.build();
        return fingerprint(*m.state, *b);
    };
    CHECK(buildWith(1) == buildWith(8));
}

// ---------- The thread table ----------

TEST_CASE("the thread table has the well-known tap drills", "[doc][hole][threads]")
{
    auto table = features::threadTable();
    REQUIRE(table);
    struct Known {
        const char* series;
        const char* designation;
        const char* size;
        double diameter;
    };
    const Known known[] = {
        {"iso_metric_coarse", "M6x1", "5.0 mm", 5.0},    {"iso_metric_coarse", "M8x1.25", "6.8 mm", 6.8},
        {"iso_metric_coarse", "M10x1.5", "8.5 mm", 8.5}, {"unc", "1/4-20 UNC", "#7", 0.201 * 25.4},
        {"unc", "3/8-16", "5/16 in", 0.3125 * 25.4},     {"unf", "10-32 UNF", "#21", 0.159 * 25.4},
        {"iso_metric_coarse", "M6", "5.0 mm", 5.0},      {"iso_metric_fine", "M10 x 1.25", "8.8 mm", 8.8},
        {"unf", "#10-32", "#21", 0.159 * 25.4},
    };
    for (const Known& k : known) {
        INFO(k.series << " " << k.designation);
        const features::ThreadSize* s = features::findThread(**table, k.series, k.designation);
        REQUIRE(s != nullptr);
        CHECK(s->tapDrill.size == k.size);
        CHECK(std::abs(s->tapDrill.diameter - k.diameter) <= 1e-9);
    }
    CHECK(features::findThread(**table, "iso_metric_fine", "M10") == nullptr); // two fine pitches
    CHECK(features::findThread(**table, "unc", "M6x1") == nullptr);
    CHECK(features::findThread(**table, "acme", "1/2-10") == nullptr);
}

TEST_CASE("every entry of the thread table is consistent", "[doc][hole][threads]")
{
    auto table = features::threadTable();
    REQUIRE(table);
    std::set<std::string> ids;
    for (const features::ThreadSeries& s : (*table)->series) {
        ids.insert(s.id);
        INFO(s.id);
        CHECK(s.sizes.size() >= 20);
        std::set<std::string> designations;
        for (const features::ThreadSize& z : s.sizes) {
            INFO(z.designation);
            CHECK(designations.insert(z.designation).second);
            CHECK(z.pitch > 0);
            CHECK(z.majorDiameter > z.pitch);
            if (s.system == "inch") {
                REQUIRE(z.tpi);
                CHECK(std::abs(z.pitch - 25.4 / *z.tpi) <= 1e-12);
                CHECK(z.designation.find(s.id == "unc" ? " UNC" : " UNF") != std::string::npos);
            } else {
                CHECK(!z.tpi);
                CHECK(z.designation.rfind(z.nominal + "x", 0) == 0);
                CHECK(std::abs(z.majorDiameter - std::stod(z.nominal.substr(1))) <= 1e-12);
            }
            // D1 = D - 2 x 5/8 H, H = sqrt(3)/2 P (rounded to 1e-6 mm in the file).
            CHECK(std::abs(z.basicMinorDiameter - (z.majorDiameter - 0.625 * std::sqrt(3.0) * z.pitch)) <= 1e-6);
            CHECK(z.tapDrill.diameter >= z.basicMinorDiameter - 1e-9);
            CHECK(z.tapDrill.diameter < z.majorDiameter);
            // The rule of the file: within one drill step of the formula's value.
            const double formula = s.system == "inch" ? z.majorDiameter - 0.75 * 1.5 * std::sqrt(3.0) / 2 * z.pitch
                                                      : z.majorDiameter - z.pitch;
            CHECK(std::abs(z.tapDrill.diameter - formula) <= (s.system == "inch" ? 0.4 : 0.26));
            if (s.system == "metric") {
                CHECK(z.tapDrill.size ==
                      json::write(json::Value::number(z.tapDrill.diameter)) +
                          (z.tapDrill.diameter == std::floor(z.tapDrill.diameter) ? ".0 mm" : " mm"));
            }
        }
    }
    CHECK(ids == std::set<std::string>{"iso_metric_coarse", "iso_metric_fine", "unc", "unf"});
    // The listing the standards.threads.list action returns.
    auto all = features::threadListJson("");
    REQUIRE(all);
    auto parsed = json::parse(*all);
    REQUIRE(parsed);
    REQUIRE(parsed->find("series") != nullptr);
    CHECK(parsed->find("series")->asArray().size() == 4);
    CHECK(parsed->find("sources") != nullptr);
    CHECK(parsed->find("method") != nullptr);
    auto unc = features::threadListJson("unc");
    REQUIRE(unc);
    CHECK(json::parse(*unc)->find("series")->asArray().size() == 1);
    auto bad = features::threadListJson("acme");
    REQUIRE(!bad);
    CHECK(bad.error().code == ErrorCode::NotFound);
    // A damaged table is reported, not guessed at.
    auto broken = features::parseThreadTable(R"({"series":[{"id":"x","name":"x","system":"metric","standard":"x",)"
                                             R"("sizes":[{"designation":"M1","nominal":"M1","majorDiameter":1}]}]})");
    REQUIRE(!broken);
    CHECK(broken.error().code == ErrorCode::FormatError);
}
