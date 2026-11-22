// Hole recognition (task T-034) on holes made by the document's hole feature (T-097) and rebuilt by the real
// rebuilder: every kind of the feature is found with the sizes its props give; tapped holes carry their thread spec,
// picked up through the names of the bore (TopoName::origin of role "hole.*") and the feature's props
// (features::holeThreadOf, standing in for the lookup a caller of recognizeHoles provides: HoleOptions::threadOf); and
// the reference to a hole follows the hole when its sketch point moves.
#include "holes_test_util.h"
#include "parametric_test_util.h"

#include "features/hole_feature.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace ray;
using namespace ray::doc;
using namespace ray::doc::test;
using ray::cam::HoleBottom;
using ray::cam::RecognizedHole;

namespace {

const double kTip = 1.0 / std::tan(59.0 * kPi / 180.0);

struct Model {
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
    BuildResultPtr build() { return env.rebuild(state); }
};

std::string holeProps(std::uint64_t sketch, const std::string& rest)
{
    return R"({"sketch":")" + uuid('f', sketch) + R"(","target":")" + uuid('b', 1) + "\"," + rest + "}";
}
std::string onDatum(std::uint64_t datum)
{
    return R"({"datum":")" + uuid('f', datum) + R"("})";
}

// Block 120 x 30 x 15 (body 'b'), datum 4 on its top; hole n + 1 from sketch n (a point on the datum).
void buildPart(Model& m, double firstX = 25)
{
    m.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", SketchJson().rect(1, 0, 0, 120, 30))));
    m.run(addFeature("extrude", 3, extrudeProps(2, "15", "new", 'b')));
    m.run(addFeature("datum_plane", 4, R"({"base":{"principal":"xy"},"offset":15})"));
    const auto hole = [&](std::uint64_t n, const SketchJson& points, const std::string& props) {
        m.run(addFeature("sketch", n, sketchProps(onDatum(4), points)));
        m.run(addFeature("hole", n + 1, holeProps(n, props)));
    };
    hole(5, SketchJson().point(1, 10, 15), R"("kind":"simple","diameter":6,"end":"through")");
    hole(7, SketchJson().point(1, firstX, 15), R"("kind":"simple","diameter":6,"end":"blind","depth":8)");
    hole(9, SketchJson().point(1, 40, 15), R"("kind":"simple","diameter":6,"end":"blind","depth":8,"bottom":"flat")");
    hole(11, SketchJson().point(1, 55, 15),
         R"("kind":"counterbore","diameter":6.6,"end":"through","counterbore":{"diameter":11,"depth":6.5})");
    hole(13, SketchJson().point(1, 70, 15),
         R"("kind":"countersink","diameter":6.6,"end":"through","countersink":{"diameter":12})");
    hole(15, SketchJson().point(1, 85, 8).point(2, 85, 22),
         R"("kind":"tapped","end":"blind","depth":10,"thread":{"designation":"M6x1","standard":"iso_metric_coarse",)"
         R"("nominalDiameter":6,"pitch":1,"class":"6H","tapDrill":5,"depth":8})");
    hole(17, SketchJson().point(1, 102, 15),
         R"("kind":"counterbore_tapped","end":"blind","depth":7,"counterbore":{"diameter":14,"depth":3},)"
         R"("thread":{"designation":"M8x1.25","standard":"iso_metric_coarse","nominalDiameter":8,"pitch":1.25,)"
         R"("tapDrill":6.8})");
}

// The thread lookup a caller gives recognizeHoles: the hole feature's thread spec from its props.
cam::HoleOptions threadPickup(const DocState& state, const naming::ElementMap& names)
{
    cam::HoleOptions options;
    options.names = &names;
    options.threadOf = [&state](const FeatureId& id) -> std::optional<cam::HoleThread> {
        for (const Feature& f : state.features) {
            if (f.id == id) {
                auto thread = features::holeThreadOf(f, {});
                if (thread && *thread) {
                    return cam::HoleThread{(*thread)->designation, (*thread)->pitch};
                }
            }
        }
        return std::nullopt;
    };
    return options;
}

const RecognizedHole& holeAt(const std::vector<RecognizedHole>& holes, double x, double y)
{
    const auto it = std::find_if(holes.begin(), holes.end(), [&](const RecognizedHole& h) {
        return std::abs(toMm(h.x) - x) < 1e-6 && std::abs(toMm(h.y) - y) < 1e-6;
    });
    REQUIRE(it != holes.end());
    return *it;
}

void segmentIs(const cam::HoleSegment& g, double dTop, double dBottom, double zTop, double zBottom, double angle = 0)
{
    CHECK(g.conical == (angle != 0));
    CHECK(std::abs(g.diameter - dTop) < 1e-6);
    CHECK(std::abs(g.bottomDiameter - dBottom) < 1e-6);
    CHECK(std::abs(toMm(g.zTop) - zTop) < 1e-6);
    CHECK(std::abs(toMm(g.zBottom) - zBottom) < 1e-6);
    if (angle != 0) {
        CHECK(std::abs(g.coneAngleDeg - angle) < 1e-6);
    }
}

} // namespace

TEST_CASE("holes made by the hole feature are found with their sizes and tapped ones with their thread",
          "[cam][holes][document]")
{
    Model m;
    buildPart(m);
    const BuildResultPtr b = m.build();
    for (std::uint64_t n = 6; n <= 18; n += 2) {
        INFO("hole feature " << n << ": " << report(*b, n).message);
        REQUIRE(report(*b, n).status == FeatureStatus::Ok);
    }
    const BuiltBody& body = bodyOf(*b, 'b');
    auto resolver = naming::makeResolver({naming::ResolverBody{bodyId('b'), body.map, body.shape}});
    JobContext ctx;
    auto found = cam::recognizeHoles(body.shape, *resolver, bodyId('b'), cam::test::identityFrame(), ctx,
                                     threadPickup(*m.state, body.map));
    REQUIRE(found);
    for (const RecognizedHole& h : *found) {
        UNSCOPED_INFO(cam::test::describe(h));
    }
    REQUIRE(found->size() == 8);

    const RecognizedHole& through = holeAt(*found, 10, 15);
    CHECK(through.bottom == HoleBottom::Through);
    REQUIRE(through.segments.size() == 1);
    segmentIs(through.segments[0], 6, 6, 15, 0);

    const RecognizedHole& point = holeAt(*found, 25, 15);
    CHECK(point.bottom == HoleBottom::Conical);
    REQUIRE(point.segments.size() == 2);
    segmentIs(point.segments[0], 6, 6, 15, 7);
    segmentIs(point.segments[1], 6, 0, 7, 7 - 3 * kTip, 118);

    const RecognizedHole& flat = holeAt(*found, 40, 15);
    CHECK(flat.bottom == HoleBottom::Flat);
    REQUIRE(flat.segments.size() == 1);
    segmentIs(flat.segments[0], 6, 6, 15, 7);

    const RecognizedHole& cbore = holeAt(*found, 55, 15);
    CHECK(cbore.bottom == HoleBottom::Through);
    CHECK(cbore.counterbored);
    REQUIRE(cbore.segments.size() == 2);
    segmentIs(cbore.segments[0], 11, 11, 15, 8.5);
    segmentIs(cbore.segments[1], 6.6, 6.6, 8.5, 0);

    const RecognizedHole& csink = holeAt(*found, 70, 15);
    CHECK(csink.bottom == HoleBottom::Through);
    CHECK(csink.countersunk);
    CHECK(!csink.chamfered);
    REQUIRE(csink.segments.size() == 2);
    segmentIs(csink.segments[0], 12, 6.6, 15, 15 - 2.7, 90);
    segmentIs(csink.segments[1], 6.6, 6.6, 15 - 2.7, 0);

    for (const double y : {8.0, 22.0}) {
        const RecognizedHole& tapped = holeAt(*found, 85, y);
        CHECK(tapped.bottom == HoleBottom::Conical);
        REQUIRE(tapped.segments.size() == 2);
        segmentIs(tapped.segments[0], 5, 5, 15, 5); // modelled at the tap drill
        segmentIs(tapped.segments[1], 5, 0, 5, 5 - 2.5 * kTip, 118);
        CHECK(tapped.threadSpec == "M6x1");
        CHECK(tapped.threadPitch == 1.0);
    }

    const RecognizedHole& cboreTapped = holeAt(*found, 102, 15);
    CHECK(cboreTapped.counterbored);
    REQUIRE(cboreTapped.segments.size() == 3);
    segmentIs(cboreTapped.segments[0], 14, 14, 15, 12);
    segmentIs(cboreTapped.segments[1], 6.8, 6.8, 12, 8);
    segmentIs(cboreTapped.segments[2], 6.8, 0, 8, 8 - 3.4 * kTip, 118);
    CHECK(cboreTapped.threadSpec == "M8x1.25");
    CHECK(cboreTapped.threadPitch == 1.25);

    // Holes that are not tapped have no thread; without the lookup no hole has one.
    for (const RecognizedHole* h : {&through, &point, &flat, &cbore, &csink}) {
        CHECK(h->threadSpec.empty());
        CHECK(h->threadPitch == 0.0);
    }
    JobContext ctx2;
    auto plain = cam::recognizeHoles(body.shape, cam::test::identityFrame(), ctx2);
    REQUIRE(plain);
    REQUIRE(plain->size() == 8);
    for (const RecognizedHole& h : *plain) {
        CHECK(h.threadSpec.empty());
        CHECK(h.axisFace.names.empty()); // the overload without a resolver makes no references
    }
    // Every reference names the bore wall and resolves to it.
    for (const RecognizedHole& h : *found) {
        const naming::Resolved r = resolver->resolve(h.axisFace);
        CHECK(r.state == naming::ResolveState::Exact);
        CHECK(r.elements == std::vector<kernel::SubId>{h.axisSub});
    }
}

TEST_CASE("a reference to a recognised hole follows the hole when its sketch point moves", "[cam][holes][document]")
{
    Model m;
    buildPart(m, 25);
    const BuildResultPtr before = m.build();
    const BuiltBody& b0 = bodyOf(*before, 'b');
    auto r0 = naming::makeResolver({naming::ResolverBody{bodyId('b'), b0.map, b0.shape}});
    JobContext ctx;
    auto holes = cam::recognizeHoles(b0.shape, *r0, bodyId('b'), cam::test::identityFrame(), ctx);
    REQUIRE(holes);
    const naming::TopoRef ref = holeAt(*holes, 25, 15).axisFace; // what a drilling operation stores

    // The point of hole 8 moves 3 mm along X: the same hole, elsewhere.
    m.run(editFeature("sketch", 7, sketchProps(onDatum(4), SketchJson().point(1, 28, 15))));
    const BuildResultPtr after = m.build();
    REQUIRE(report(*after, 8).status == FeatureStatus::Ok);
    const BuiltBody& b1 = bodyOf(*after, 'b');
    auto r1 = naming::makeResolver({naming::ResolverBody{bodyId('b'), b1.map, b1.shape}});
    const naming::Resolved resolved = r1->resolve(ref);
    INFO(resolved.reason);
    CHECK(resolved.postableWithoutReview());
    REQUIRE(resolved.elements.size() == 1);
    JobContext ctx2;
    auto moved = cam::recognizeHoles(b1.shape, cam::test::identityFrame(), ctx2);
    REQUIRE(moved);
    const RecognizedHole& h = holeAt(*moved, 28, 15);
    CHECK(std::binary_search(h.faces.begin(), h.faces.end(), resolved.elements[0]));
    CHECK(h.bottom == HoleBottom::Conical);
}
