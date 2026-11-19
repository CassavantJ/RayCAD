// propagate() and the resolver states Split, Merged and Repaired (task T-081a), on small parts built with the
// kernel's extrude, boolean, fillet and chamfer. The oracle-judged scenarios are in tests/naming/oracle.
#include <catch2/catch_test_macros.hpp>

#include "naming_test_util.h"

#include <propagate.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

using namespace ray;
using namespace ray::naming;
using kernel::SubId;
using kernel::SubKind;
using ray::naming::test::take;

namespace {

const FeatureId kBase = test::featureId(41);
const FeatureId kCut = test::featureId(42);
const FeatureId kCut2 = test::featureId(43);
const FeatureId kBlend = test::featureId(44);
const BodyId kBody = test::bodyId(4);

kernel::ProfileCurve line(Vec2 a, Vec2 b, std::uint64_t tag)
{
    return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Line, {a, b}, 0.0, false, tag};
}

kernel::ProfileLoop rectangle(double x0, double y0, double x1, double y1, std::uint64_t firstTag, std::uint64_t loop)
{
    return kernel::ProfileLoop{{line({x0, y0}, {x1, y0}, firstTag), line({x1, y0}, {x1, y1}, firstTag + 1),
                                line({x1, y1}, {x0, y1}, firstTag + 2), line({x0, y1}, {x0, y0}, firstTag + 3)},
                               loop};
}

kernel::ProfileLoop circle(double cx, double cy, double r, std::uint64_t tag, std::uint64_t loop)
{
    return kernel::ProfileLoop{{kernel::ProfileCurve{kernel::ProfileCurve::Kind::Circle, {{cx, cy}}, r, true, tag}},
                               loop};
}

kernel::Plane xy(double z)
{
    return kernel::Plane{Vec3{0, 0, z}, Vec3{0, 0, 1}, Vec3{1, 0, 0}};
}

struct Body {
    kernel::Shape shape;
    ElementMap map;
    std::vector<LineageEvent> lineage;
};

// An extruded profile, named.
Body extrude(const FeatureId& id, kernel::Plane plane, kernel::ProfileLoop loop, double distance)
{
    JobContext ctx;
    auto sweep = take(kernel::extrude(kernel::Profile{plane, {std::move(loop)}}, distance, 0.0, ctx));
    const NamingInput in{id, "extrude", {}, nullptr, &sweep.op.shape, sweep.origins};
    NamingOutput out = take(propagate(in));
    return Body{sweep.op.shape, std::move(out.map), std::move(out.lineage)};
}

// `body` cut (or joined) with an extruded profile, named as feature `id`.
Body boolean(const Body& body, const FeatureId& id, kernel::BoolOp op, kernel::Plane plane, kernel::ProfileLoop loop,
             double distance)
{
    const Body tool = extrude(id, plane, std::move(loop), distance);
    JobContext ctx;
    auto result = take(kernel::boolean(op, body.shape, tool.shape, ctx));
    const std::array<const ElementMap*, 2> maps{&body.map, &tool.map};
    const NamingInput in{id, "extrude", maps, &result.history, &result.shape, {}};
    NamingOutput out = take(propagate(in));
    return Body{result.shape, std::move(out.map), std::move(out.lineage)};
}

Body blend(const Body& body, const FeatureId& id, bool fillet, std::vector<SubId> edges, double size)
{
    JobContext ctx;
    auto result =
        take(fillet ? kernel::fillet(body.shape, edges, size, ctx) : kernel::chamfer(body.shape, edges, size, ctx));
    const std::array<const ElementMap*, 1> maps{&body.map};
    const NamingInput in{id, fillet ? "fillet" : "chamfer", maps, &result.history, &result.shape, {}};
    NamingOutput out = take(propagate(in));
    return Body{result.shape, std::move(out.map), std::move(out.lineage)};
}

// A 100 x 60 x 20 block from an extrusion.
Body block()
{
    return extrude(kBase, xy(0), rectangle(0, 0, 100, 60, 1, 1), 20);
}

std::unique_ptr<IResolver> resolverFor(const Body& b)
{
    return makeResolver({ResolverBody{kBody, b.map, b.shape}});
}

// The face whose centroid is closest to `p` (geometry straight from the kernel).
SubId faceNear(const kernel::Shape& shape, Vec3 p)
{
    SubId best{SubKind::Face, 0};
    double bd = 1e300;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
        const auto info = take(kernel::surfaceInfo(shape, SubId{SubKind::Face, i}));
        const double d = std::hypot(info.centroid.x - p.x, info.centroid.y - p.y, info.centroid.z - p.z);
        if (d < bd) {
            bd = d;
            best = SubId{SubKind::Face, i};
        }
    }
    return best;
}

// Faces with a planar normal along `n` whose plane passes through `p`.
std::vector<SubId> facesOnPlane(const kernel::Shape& shape, Vec3 p, Vec3 n)
{
    std::vector<SubId> out;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
        const auto info = take(kernel::surfaceInfo(shape, SubId{SubKind::Face, i}));
        if (info.type != kernel::SurfaceType::Plane)
            continue;
        const double d = (info.centroid.x - p.x) * n.x + (info.centroid.y - p.y) * n.y + (info.centroid.z - p.z) * n.z;
        const double c = std::abs(info.axis.x * n.x + info.axis.y * n.y + info.axis.z * n.z);
        if (std::abs(d) < 1e-6 && c > 1 - 1e-9)
            out.push_back(SubId{SubKind::Face, i});
    }
    return out;
}

// The vertical edge at (x, y).
SubId verticalEdgeAt(const kernel::Shape& shape, double x, double y)
{
    for (std::uint32_t i = 0; i < shape.count(SubKind::Edge); ++i) {
        const auto c = take(kernel::curveInfo(shape, SubId{SubKind::Edge, i}));
        if (c.type == kernel::CurveType::Line && std::abs(c.start.x - x) < 1e-6 && std::abs(c.end.x - x) < 1e-6 &&
            std::abs(c.start.y - y) < 1e-6 && std::abs(c.end.y - y) < 1e-6)
            return SubId{SubKind::Edge, i};
    }
    FAIL("no vertical edge there");
    return {};
}

bool everyElementNamed(const Body& b)
{
    for (const SubId s : test::allElements(b.shape))
        if (b.map.namesOf(s).empty())
            return false;
    return true;
}

} // namespace

TEST_CASE("propagate names every face, edge and vertex of an extrusion from its sweep origins", "[naming][propagate]")
{
    const Body b = block();
    CHECK(everyElementNamed(b));
    CHECK(b.map.size() == 6 + 12 + 8);
    // Side faces carry the curve tag, caps the loop tag; no name is ambiguous.
    int sides = 0, caps = 0;
    for (std::uint32_t i = 0; i < 6; ++i) {
        const auto names = b.map.namesOf(SubId{SubKind::Face, i});
        REQUIRE(names.size() == 1);
        const TopoName* r = b.map.record(names[0]);
        REQUIRE(r != nullptr);
        CHECK(r->origin == kBase);
        CHECK_FALSE(r->ambiguous());
        CHECK(r->siblingCount == 1);
        if (r->role == "extrude.side") {
            ++sides;
            REQUIRE(r->parents.size() == 1);
        } else if (r->role == "extrude.startcap" || r->role == "extrude.endcap") {
            ++caps;
            CHECK(r->parents == std::vector<Hash256>{naming::detail::loopTagParent(1)});
        }
    }
    CHECK(sides == 4);
    CHECK(caps == 2);
    // Edges and vertices of a prism have no history: named from their faces (rule 3).
    const TopoName* e = b.map.record(b.map.namesOf(SubId{SubKind::Edge, 0})[0]);
    REQUIRE(e != nullptr);
    CHECK(e->role == "nb.edge");
    CHECK(e->parents.size() == 2);
    // The family table holds every name.
    for (const SubId s : test::allElements(b.shape))
        for (const Hash256& n : b.map.namesOf(s)) {
            const TopoName* r = b.map.record(n);
            REQUIRE(r != nullptr);
            const auto fam = b.map.family(r->family);
            CHECK(std::find(fam.begin(), fam.end(), n) != fam.end());
        }
    // Round trip.
    const ElementMap back = take(ElementMap::deserialize(b.map.serialize()));
    CHECK(back.serialize() == b.map.serialize());
}

TEST_CASE("propagate rejects inconsistent input", "[naming][propagate]")
{
    const Body b = block();
    const NamingInput none{kBase, "extrude", {}, nullptr, nullptr, {}};
    CHECK_FALSE(propagate(none).has_value());
    const kernel::Shape empty;
    const NamingInput nullShape{kBase, "extrude", {}, nullptr, &empty, {}};
    CHECK_FALSE(propagate(nullShape).has_value());
    // Input maps without a history.
    const std::array<const ElementMap*, 1> maps{&b.map};
    const NamingInput noHistory{kCut, "extrude", maps, nullptr, &b.shape, {}};
    CHECK_FALSE(propagate(noHistory).has_value());
    // A history entry for an input that was not given.
    kernel::ShapeHistory h;
    h.entries.push_back(
        {kernel::HistoryEntry::Relation::Modified, 3, SubId{SubKind::Face, 0}, {SubId{SubKind::Face, 0}}});
    const NamingInput badInput{kCut, "extrude", maps, &h, &b.shape, {}};
    CHECK_FALSE(propagate(badInput).has_value());
    // A history result the shape does not have.
    kernel::ShapeHistory h2;
    h2.entries.push_back(
        {kernel::HistoryEntry::Relation::Modified, 0, SubId{SubKind::Face, 0}, {SubId{SubKind::Face, 999}}});
    const NamingInput badResult{kCut, "extrude", maps, &h2, &b.shape, {}};
    CHECK_FALSE(propagate(badResult).has_value());
}

TEST_CASE("an unrelated cut leaves every other reference Exact", "[naming][propagate]")
{
    const Body b0 = boolean(block(), kCut, kernel::BoolOp::Cut, xy(20), circle(30, 30, 5, 1, 1), -30);
    auto r0 = resolverFor(b0);
    std::vector<TopoRef> refs;
    for (const SubId s : test::allElements(b0.shape))
        refs.push_back(r0->makeRef(kBody, s));
    // The same tree with an unrelated pocket added before the hole.
    const Body pocket = boolean(block(), kCut2, kernel::BoolOp::Cut, xy(20), rectangle(70, 10, 90, 30, 1, 1), -5);
    const Body b1 = boolean(pocket, kCut, kernel::BoolOp::Cut, xy(20), circle(30, 30, 5, 1, 1), -30);
    auto r1 = resolverFor(b1);
    for (std::size_t i = 0; i < refs.size(); ++i) {
        const Resolved res = r1->resolve(refs[i]);
        INFO("reference " << i);
        REQUIRE(res.state == ResolveState::Exact);
        REQUIRE(res.elements.size() == 1);
        // The same element: the geometry is unchanged except where the pocket touched the top face.
        const SubId was = test::allElements(b0.shape)[i];
        if (was.kind != SubKind::Face || std::abs(test::geometryOf(b0.shape, was)[2] - 20) > 1e-9)
            CHECK(
                test::sameGeometry(test::geometryOf(b0.shape, was), test::geometryOf(b1.shape, res.elements[0]), 1e-9));
    }
}

TEST_CASE("a cut that splits a referenced face gives Split with both pieces", "[naming][propagate]")
{
    const Body b0 = block();
    const SubId front = faceNear(b0.shape, Vec3{50, 0, 10});
    const TopoRef ref = resolverFor(b0)->makeRef(kBody, front);
    // A through notch across the front face splits it in two.
    const Body b1 = boolean(b0, kCut, kernel::BoolOp::Cut, xy(30), rectangle(40, -5, 60, 10, 1, 1), -40);
    const Resolved res = resolverFor(b1)->resolve(ref);
    REQUIRE(res.state == ResolveState::Split);
    CHECK(res.usable());
    const std::vector<SubId> pieces = facesOnPlane(b1.shape, Vec3{0, 0, 0}, Vec3{0, 1, 0});
    REQUIRE(pieces.size() == 2);
    CHECK(res.elements == pieces);
    // Lineage: the face's name was split into the two piece names.
    const Hash256 name = b0.map.namesOf(front)[0];
    bool found = false;
    for (const LineageEvent& e : b1.lineage)
        if (e.kind == LineageEvent::Kind::Split && e.from == name) {
            found = true;
            CHECK(e.to.size() == 2);
            CHECK(e.cause == kCut);
        }
    CHECK(found);
}

TEST_CASE("a fuse that unifies two faces gives Merged", "[naming][propagate]")
{
    // A boss whose front face sits 5 mm behind the block's front face...
    const Body b0 = boolean(block(), kCut, kernel::BoolOp::Union, xy(20), rectangle(10, 5, 40, 30, 1, 1), 10);
    const SubId bossFront = faceNear(b0.shape, Vec3{25, 5, 25});
    const TopoRef ref = resolverFor(b0)->makeRef(kBody, bossFront);
    // ...and the same boss flush with it, from below the top: its front face unifies with the block's.
    const Body b1 = boolean(block(), kCut, kernel::BoolOp::Union, xy(10), rectangle(10, 0, 40, 30, 1, 1), 20);
    const Resolved res = resolverFor(b1)->resolve(ref);
    REQUIRE(res.state == ResolveState::Merged);
    CHECK(res.usable());
    const std::vector<SubId> front = facesOnPlane(b1.shape, Vec3{0, 0, 0}, Vec3{0, 1, 0});
    REQUIRE(front.size() == 1);
    CHECK(res.elements == front);
    CHECK(b1.map.namesOf(front[0]).size() == 2);
    bool merged = false;
    for (const LineageEvent& e : b1.lineage)
        merged = merged || e.kind == LineageEvent::Kind::Merged;
    CHECK(merged);
}

TEST_CASE("a merged face cut apart again is Split on its own plane, never answered with a face elsewhere",
          "[naming][propagate]")
{
    // The boss flush with the block's front face: one front face holding two names.
    const Body b0 = boolean(block(), kCut, kernel::BoolOp::Union, xy(10), rectangle(10, 0, 40, 30, 1, 1), 20);
    const std::vector<SubId> merged = facesOnPlane(b0.shape, Vec3{0, 0, 0}, Vec3{0, 1, 0});
    REQUIRE(merged.size() == 1);
    REQUIRE(b0.map.namesOf(merged[0]).size() == 2);
    const TopoRef ref = resolverFor(b0)->makeRef(kBody, merged[0]);
    {
        // A notch across the front face: two pieces, both on the picked plane.
        const Body b1 = boolean(b0, kCut2, kernel::BoolOp::Cut, xy(30), rectangle(60, -5, 70, 10, 1, 1), -40);
        const Resolved res = resolverFor(b1)->resolve(ref);
        INFO(res.reason);
        REQUIRE(res.state == ResolveState::Split);
        CHECK(res.usable());
        CHECK(res.elements == facesOnPlane(b1.shape, Vec3{0, 0, 0}, Vec3{0, 1, 0}));
    }
    {
        // The boss brought 5 mm forward instead: its front face is no longer on the picked plane, and is not a
        // piece of what was picked.
        const Body b1 = boolean(block(), kCut, kernel::BoolOp::Union, xy(10), rectangle(10, -5, 40, 30, 1, 1), 20);
        const Resolved res = resolverFor(b1)->resolve(ref);
        INFO(res.reason);
        CHECK(res.state == ResolveState::Broken);
        CHECK_FALSE(res.usable());
    }
}

TEST_CASE("a cut that removes a referenced face gives Broken", "[naming][propagate]")
{
    const Body b0 = boolean(block(), kCut, kernel::BoolOp::Cut, xy(20), rectangle(20, 20, 40, 40, 1, 1), -5);
    const SubId floor = faceNear(b0.shape, Vec3{30, 30, 15});
    const TopoRef ref = resolverFor(b0)->makeRef(kBody, floor);
    // A through cut over the whole pocket removes its floor.
    const Body b1 = boolean(b0, kCut2, kernel::BoolOp::Cut, xy(30), rectangle(15, 15, 45, 45, 1, 1), -40);
    const Resolved res = resolverFor(b1)->resolve(ref);
    CHECK(res.state == ResolveState::Broken);
    CHECK(res.elements.empty());
    CHECK_FALSE(res.usable());
    CHECK_FALSE(res.reason.empty());
    bool deleted = false;
    const Hash256 name = b0.map.namesOf(floor)[0];
    for (const LineageEvent& e : b1.lineage)
        deleted = deleted || (e.kind == LineageEvent::Kind::Deleted && e.from == name);
    CHECK(deleted);
}

TEST_CASE("blend faces are generated from their edge, boundary edges named from their faces", "[naming][propagate]")
{
    const Body b0 = block();
    const Body b1 = blend(b0, kBlend, true, {verticalEdgeAt(b0.shape, 100, 60)}, 5);
    CHECK(everyElementNamed(b1));
    int blendFaces = 0;
    for (std::uint32_t i = 0; i < b1.shape.count(SubKind::Face); ++i) {
        const TopoName* r = b1.map.record(b1.map.namesOf(SubId{SubKind::Face, i})[0]);
        REQUIRE(r != nullptr);
        if (r->origin == kBlend) {
            ++blendFaces;
            CHECK(r->role == "fillet.gE_F");
        }
    }
    CHECK(blendFaces == 1);
    // A chamfer on the same edge of the same block has different names (role) but the untouched faces keep theirs.
    const Body b2 = blend(b0, kBlend, false, {verticalEdgeAt(b0.shape, 100, 60)}, 5);
    const SubId bottom = faceNear(b0.shape, Vec3{50, 30, 0});
    const Resolved res = resolverFor(b2)->resolve(resolverFor(b0)->makeRef(kBody, bottom));
    CHECK(res.state == ResolveState::Exact);
}

TEST_CASE("Repaired is a suggestion and never usable", "[naming][propagate]")
{
    // A reference whose names are all foreign to the map, with the hint and neighbour names of a real face.
    const Body b0 = block();
    auto r0 = resolverFor(b0);
    const SubId top = faceNear(b0.shape, Vec3{50, 30, 20});
    TopoRef ref = r0->makeRef(kBody, top);
    Hasher h;
    h.str("not a name of this build");
    ref.names = {h.finish()};
    ref.families = {h.finish()};
    ref.ambiguous = {false};
    const Resolved res = r0->resolve(ref);
    REQUIRE(res.state == ResolveState::Repaired);
    CHECK(res.elements == std::vector<SubId>{top});
    CHECK_FALSE(res.usable());
    ref.repairConfirmed = true;
    CHECK_FALSE(r0->resolve(ref).usable());
    // Without neighbour names, or with a neighbour that is not around the candidate, there is no repair.
    TopoRef lonely = ref;
    lonely.neighbourNames.clear();
    CHECK(r0->resolve(lonely).state == ResolveState::Broken);
    TopoRef wrongNeighbours = ref;
    wrongNeighbours.neighbourNames = {b0.map.namesOf(faceNear(b0.shape, Vec3{50, 30, 0}))[0]};
    CHECK(r0->resolve(wrongNeighbours).state == ResolveState::Broken);
}

namespace {

// The same chain as the helpers above, without Catch2 macros (they are not thread-safe): any failure gives an
// empty result.
std::vector<std::vector<std::byte>> namingChain()
{
    std::vector<std::vector<std::byte>> out;
    JobContext ctx;
    auto toolOf = [&](kernel::Plane plane, kernel::ProfileLoop loop, double d, const FeatureId& id,
                      kernel::Shape& shape) -> std::optional<ElementMap> {
        auto sweep = kernel::extrude(kernel::Profile{plane, {std::move(loop)}}, d, 0.0, ctx);
        if (!sweep)
            return std::nullopt;
        auto named = propagate(NamingInput{id, "extrude", {}, nullptr, &sweep->op.shape, sweep->origins});
        if (!named)
            return std::nullopt;
        shape = sweep->op.shape;
        return std::move(named->map);
    };
    kernel::Shape shape;
    auto map = toolOf(xy(0), rectangle(0, 0, 100, 60, 1, 1), 20, kBase, shape);
    if (!map)
        return {};
    out.push_back(map->serialize());
    const std::array<std::pair<const FeatureId*, kernel::ProfileLoop>, 2> cuts{
        std::pair{&kCut, rectangle(40, -5, 60, 10, 1, 1)}, std::pair{&kCut2, circle(20, 40, 4, 1, 1)}};
    const std::array<double, 2> depth{-40, -30};
    const std::array<double, 2> at{30, 20};
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        kernel::Shape toolShape;
        auto toolMap = toolOf(xy(at[i]), cuts[i].second, depth[i], *cuts[i].first, toolShape);
        if (!toolMap)
            return {};
        auto result = kernel::boolean(kernel::BoolOp::Cut, shape, toolShape, ctx);
        if (!result)
            return {};
        const std::array<const ElementMap*, 2> maps{&*map, &*toolMap};
        auto named = propagate(NamingInput{*cuts[i].first, "extrude", maps, &result->history, &result->shape, {}});
        if (!named)
            return {};
        shape = result->shape;
        map = std::move(named->map);
        out.push_back(map->serialize());
    }
    return out;
}

} // namespace

TEST_CASE("propagate output is byte-identical across runs and thread counts", "[naming][propagate]")
{
    const auto reference = namingChain();
    REQUIRE(reference.size() == 3);
    CHECK(namingChain() == reference);
    std::vector<std::vector<std::vector<std::byte>>> results(8);
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < results.size(); ++t)
        threads.emplace_back([&, t] { results[t] = namingChain(); });
    for (std::thread& t : threads)
        t.join();
    for (const auto& r : results)
        CHECK(r == reference);
}

// ---------------------------------------------------------------- T-081a gate regressions (mechanisms)

TEST_CASE("siblings told apart by all faces around them are marked and need their hint; by the issuing feature's "
          "faces they are not",
          "[naming][propagate][t081a-gate]")
{
    // Random sequence 19:1: two pieces of a cut wall, told apart by the cutting feature's faces in one build and by
    // all faces in the next, swapped names because the two modes gave equal discriminators. Here: a rectangle whose
    // bottom and top lines share a sketch tag, so their side faces are siblings that only all-around faces (which are
    // the same for both) could tell apart: the fallback mode, marked.
    JobContext ctx;
    kernel::ProfileLoop loop = rectangle(0, 0, 100, 60, 1, 1);
    loop.curves[2].tag = loop.curves[0].tag;
    auto sweep = take(kernel::extrude(kernel::Profile{xy(0), {loop}}, 20, 0.0, ctx));
    const NamingInput in{kBase, "extrude", {}, nullptr, &sweep.op.shape, sweep.origins};
    const NamingOutput out = take(propagate(in));
    const Hash256 mark = naming::detail::allFacesDiscriminatorMark();
    int marked = 0;
    for (std::uint32_t i = 0; i < sweep.op.shape.count(SubKind::Face); ++i)
        for (const Hash256& n : out.map.namesOf(SubId{SubKind::Face, i})) {
            const TopoName* r = out.map.record(n);
            REQUIRE(r != nullptr);
            const bool hasMark = std::binary_search(r->discriminator.begin(), r->discriminator.end(), mark);
            if (r->siblingCount > 1) {
                CHECK(hasMark);
                CHECK(naming::detail::needsHint(*r));
                ++marked;
            } else {
                CHECK_FALSE(hasMark);
            }
        }
    CHECK(marked == 2);
    // Pieces of a face split by a cut, told apart by the cut's own walls: not marked, and a face piece does not need
    // its hint (spike S4 V34 must stay Exact when an unrelated cut shrinks one piece).
    const Body cut = boolean(block(), kCut, kernel::BoolOp::Cut, xy(15), rectangle(40, -5, 60, 70, 11, 2), 10);
    int pieces = 0;
    for (const SubId s : test::allElements(cut.shape))
        for (const Hash256& n : cut.map.namesOf(s)) {
            const TopoName* r = cut.map.record(n);
            REQUIRE(r != nullptr);
            if (r->role != "split" || s.kind != SubKind::Face)
                continue;
            ++pieces;
            CHECK_FALSE(std::binary_search(r->discriminator.begin(), r->discriminator.end(), mark));
            CHECK_FALSE(naming::detail::needsHint(*r));
        }
    CHECK(pieces >= 2);
}

TEST_CASE("the only piece of a split element that keeps a piece name counts every piece and needs its hint",
          "[naming][propagate][t081a-gate]")
{
    // Random sequence 3:10: an edge came out of a boolean in two pieces, one of which no longer lay on the edge's
    // faces and was named from its own; the other got a piece name issued "alone", the same name whichever piece it
    // was, and in the next build it was a piece on another line. Here a history says edge e0 of a block became itself
    // and an edge on none of its faces.
    const Body b = block();
    const auto facesOf = [&](SubId e) {
        return kernel::adjacent(b.shape, e, SubKind::Face);
    };
    const SubId e0{SubKind::Edge, 0};
    SubId other{};
    bool found = false;
    for (std::uint32_t i = 1; i < b.shape.count(SubKind::Edge) && !found; ++i) {
        const SubId e{SubKind::Edge, i};
        std::vector<SubId> common;
        const auto a = facesOf(e0), c = facesOf(e);
        std::set_intersection(a.begin(), a.end(), c.begin(), c.end(), std::back_inserter(common));
        if (common.empty()) {
            other = e;
            found = true;
        }
    }
    REQUIRE(found);
    kernel::ShapeHistory h;
    for (const SubId s : test::allElements(b.shape)) {
        std::vector<SubId> results{s};
        if (s == e0)
            results = {e0, other};
        std::sort(results.begin(), results.end());
        h.entries.push_back({kernel::HistoryEntry::Relation::Modified, 0, s, results});
    }
    const std::array<const ElementMap*, 1> maps{&b.map};
    const NamingInput in{kCut, "extrude", maps, &h, &b.shape, {}};
    const NamingOutput out = take(propagate(in));
    const auto names = out.map.namesOf(e0);
    REQUIRE(names.size() == 1);
    const TopoName* r = out.map.record(names[0]);
    REQUIRE(r != nullptr);
    CHECK(r->role == "split");
    CHECK(r->siblingCount == 2);
    CHECK(naming::detail::needsHint(*r));
}
