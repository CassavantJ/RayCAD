// Pick-time evidence in TopoRef (naming.h Territory, PickNeighbour), its JSON form, and Resolved::usable() /
// postableWithoutReview(), on small parts built with the kernel's extrude and boolean.
#include <catch2/catch_test_macros.hpp>

#include "naming_test_util.h"

#include <propagate.h>
#include <shape_testing.h>
#include <territory.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::naming;
using kernel::SubId;
using kernel::SubKind;
using ray::naming::test::take;

namespace {

const FeatureId kBlock = test::featureId(61);
const FeatureId kBelow = test::featureId(62);
const FeatureId kPocket = test::featureId(63);
const BodyId kBody = test::bodyId(6);

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

struct Body {
    kernel::Shape shape;
    ElementMap map;
};

Body extrude(const FeatureId& id, double z, kernel::ProfileLoop loop, double distance)
{
    JobContext ctx;
    const kernel::Plane plane{Vec3{0, 0, z}, Vec3{0, 0, 1}, Vec3{1, 0, 0}};
    auto sweep = take(kernel::extrude(kernel::Profile{plane, {std::move(loop)}}, distance, 0.0, ctx));
    const NamingInput in{id, "extrude", {}, nullptr, &sweep.op.shape, sweep.origins};
    return Body{sweep.op.shape, take(propagate(in)).map};
}

Body boolean(const Body& body, const FeatureId& id, kernel::BoolOp op, double z, kernel::ProfileLoop loop,
             double distance)
{
    const Body tool = extrude(id, z, std::move(loop), distance);
    JobContext ctx;
    auto result = take(kernel::boolean(op, body.shape, tool.shape, ctx));
    const std::array<const ElementMap*, 2> maps{&body.map, &tool.map};
    const std::array<const kernel::Shape*, 2> shapes{&body.shape, &tool.shape};
    const NamingInput in{id, "extrude", maps, &result.history, &result.shape, {}, shapes};
    return Body{result.shape, take(propagate(in)).map};
}

// A block `length` x 60 x 20 (feature kBlock). With `below`: a block 20..80 x 10..50 x -10..0 joined under it
// (kBelow), and a pocket 40..60 x 20..40 cut into that from below, up to the big block's bottom (kPocket): the
// pocket's ceiling is a patch of the big block's bottom plane, surrounded by the lower block's material.
Body part(double length, bool below)
{
    Body b = extrude(kBlock, 0, rectangle(0, 0, length, 60, 1, 1), 20);
    if (!below)
        return b;
    b = boolean(b, kBelow, kernel::BoolOp::Union, -10, rectangle(20, 10, 80, 50, 11, 2), 10);
    return boolean(b, kPocket, kernel::BoolOp::Cut, 0, rectangle(40, 20, 60, 40, 21, 3), -15);
}

std::unique_ptr<IResolver> resolverFor(const Body& b)
{
    return makeResolver({ResolverBody{kBody, b.map, b.shape}});
}

// The planar face facing `normal` that point p lies on (by the kernel: a small box at p touches it).
std::optional<SubId> faceAt(const kernel::Shape& shape, Vec3 p, Vec3 normal)
{
    std::optional<SubId> out;
    const kernel::Shape probe =
        take(kernel::makeBox(Vec3{p.x - 1e-4, p.y - 1e-4, p.z - 1e-4}, Vec3{p.x + 1e-4, p.y + 1e-4, p.z + 1e-4}));
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
        const auto info = take(kernel::surfaceInfo(shape, SubId{SubKind::Face, i}));
        if (info.type != kernel::SurfaceType::Plane)
            continue;
        const double s = info.reversed ? -1.0 : 1.0;
        const double d = s * (info.axis.x * normal.x + info.axis.y * normal.y + info.axis.z * normal.z);
        if (d > 0.999 &&
            take(kernel::minDistance(shape, SubId{SubKind::Face, i}, probe, SubId{SubKind::Solid, 0})).value <= 1e-6) {
            if (out)
                return std::nullopt; // not unique
            out = SubId{SubKind::Face, i};
        }
    }
    return out;
}

TopoRef withoutEvidence(TopoRef ref)
{
    ref.territory = Territory{};
    ref.pickNeighbours.clear();
    return ref;
}

} // namespace

TEST_CASE("A reference records where its element lay and which elements it could be confused with",
          "[naming][evidence]")
{
    const Body b = part(100, true);
    const auto bottom = faceAt(b.shape, Vec3{5, 5, 0}, Vec3{0, 0, -1});
    const auto patch = faceAt(b.shape, Vec3{50, 30, 0}, Vec3{0, 0, -1});
    REQUIRE(bottom);
    REQUIRE(patch);
    REQUIRE(*bottom != *patch);
    const TopoRef ref = resolverFor(b)->makeRef(kBody, *bottom);
    // Its own territory: points on it, none on the patch.
    REQUIRE(ref.territory.known());
    CHECK(ref.territory.points.size() == kTerritoryPoints);
    const naming::detail::Probe onBottom(b.shape, *bottom), onPatch(b.shape, *patch);
    CHECK(onBottom.count(ref.territory) == ref.territory.points.size());
    CHECK(onPatch.count(ref.territory) == 0);
    CHECK(ref.territory.qMeasure > 0);
    // The patch lies on the same plane, facing the same way, though not next to it: recorded with its names.
    const std::span<const Hash256> patchNames = b.map.namesOf(*patch);
    bool found = false;
    for (const PickNeighbour& n : ref.pickNeighbours)
        if (n.relation == PickNeighbour::Relation::SameCarrier && n.names.size() == patchNames.size() &&
            std::equal(n.names.begin(), n.names.end(), patchNames.begin())) {
            found = true;
            CHECK(onPatch.count(n.territory) == n.territory.points.size());
            CHECK(onBottom.count(n.territory) == 0);
        }
    CHECK(found);
    CHECK(ref.pickNeighbours.size() <= kMaxPickNeighbours);
    // Deterministic: the same shape gives the same evidence.
    const TopoRef again = makeResolver({ResolverBody{kBody, b.map, b.shape}})->makeRef(kBody, *bottom);
    CHECK(toJson(again) == toJson(ref));
}

TEST_CASE("A face that grew over a coplanar face recorded at pick time is Merged; one that grew into space nobody "
          "held stays Exact",
          "[naming][evidence]")
{
    const Body picked = part(100, true);
    const auto bottom = faceAt(picked.shape, Vec3{5, 5, 0}, Vec3{0, 0, -1});
    REQUIRE(bottom);
    const TopoRef ref = resolverFor(picked)->makeRef(kBody, *bottom);

    // The lower block and the pocket suppressed: the big block's bottom is whole again and covers the pocket's
    // ceiling, a face of its own when the reference was made. It keeps its name, so names alone say Exact (T-081a
    // random 6:29).
    const Body whole = part(100, false);
    const auto wholeBottom = faceAt(whole.shape, Vec3{50, 30, 0}, Vec3{0, 0, -1});
    REQUIRE(wholeBottom);
    const Resolved merged = resolverFor(whole)->resolve(ref);
    CHECK(merged.state == ResolveState::Merged);
    CHECK(merged.elements == std::vector<SubId>{*wholeBottom});
    CHECK(merged.usable());
    CHECK_FALSE(merged.postableWithoutReview());
    // A reference saved without evidence resolves as it did before evidence existed.
    CHECK(resolverFor(whole)->resolve(withoutEvidence(ref)).state == ResolveState::Exact);

    // A dimension change (the block 20 mm longer): the bottom grows into space nobody held, and stays Exact (T-096).
    const Body longer = part(120, true);
    const auto longerBottom = faceAt(longer.shape, Vec3{5, 5, 0}, Vec3{0, 0, -1});
    REQUIRE(longerBottom);
    const Resolved exact = resolverFor(longer)->resolve(ref);
    CHECK(exact.state == ResolveState::Exact);
    CHECK(exact.elements == std::vector<SubId>{*longerBottom});
    CHECK(exact.postableWithoutReview());
}

TEST_CASE("The JSON form of a reference round-trips, evidence included", "[naming][evidence]")
{
    const Body b = part(100, true);
    const auto r = resolverFor(b);
    std::size_t withNeighbours = 0;
    for (const SubId& s : test::allElements(b.shape)) {
        const TopoRef ref = r->makeRef(kBody, s);
        if (ref.names.empty())
            continue;
        const std::string text = toJson(ref);
        const TopoRef back = take(topoRefFromJson(text));
        CHECK(toJson(back) == text);
        CHECK(back.names == ref.names);
        CHECK(back.territory.points == ref.territory.points);
        CHECK(back.pickNeighbours.size() == ref.pickNeighbours.size());
        CHECK(r->resolve(back).state == r->resolve(ref).state);
        withNeighbours += ref.pickNeighbours.empty() ? 0 : 1;
    }
    CHECK(withNeighbours > 0);
}

TEST_CASE("A reference in the JSON form written before evidence existed still loads", "[naming][evidence]")
{
    const Body b = part(100, true);
    const auto bottom = faceAt(b.shape, Vec3{5, 5, 0}, Vec3{0, 0, -1});
    REQUIRE(bottom);
    const TopoRef ref = resolverFor(b)->makeRef(kBody, *bottom);
    // Exactly the members (and order) the feature props and the model.reference action wrote before.
    const std::string old = toJson(withoutEvidence(ref));
    CHECK(old.find("territory") == std::string::npos);
    CHECK(old.find("pickNeighbours") == std::string::npos);
    const std::string prefix = "{\"ambiguous\":[";
    CHECK(old.compare(0, prefix.size(), prefix) == 0);
    const TopoRef back = take(topoRefFromJson(old));
    CHECK_FALSE(back.territory.known());
    CHECK(back.pickNeighbours.empty());
    CHECK(back.hint == ref.hint);
    CHECK(resolverFor(b)->resolve(back).state == ResolveState::Exact);
    // Optional members may be left out altogether; integers may be decimal strings, whitespace is allowed.
    const std::string minimal = "{ \"body\": \"" + ref.body.value.toString() +
                                "\", \"kind\": \"face\", \"names\": [\"" + ref.names.front().hex() +
                                "\"], \"hint\": {\"area\": \"" + std::to_string(ref.hint.qArea) + "\"} }";
    const TopoRef small = take(topoRefFromJson(minimal));
    CHECK(small.names == std::vector<Hash256>{ref.names.front()});
    CHECK(small.hint.qArea == ref.hint.qArea);
    CHECK(small.ambiguous.empty());
    CHECK_FALSE(small.repairConfirmed);
}

TEST_CASE("A damaged reference is refused, naming the member", "[naming][evidence]")
{
    const Body b = part(100, true);
    const auto bottom = faceAt(b.shape, Vec3{5, 5, 0}, Vec3{0, 0, -1});
    REQUIRE(bottom);
    const std::string good = toJson(resolverFor(b)->makeRef(kBody, *bottom));
    auto member = [](const std::string& json) {
        const auto r = topoRefFromJson(json);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::InvalidArgument);
        return r.error().context.empty() ? std::string("<none>") : r.error().context.front();
    };
    auto replaced = [&](const std::string& from, const std::string& to, bool last = false) {
        std::string s = good;
        const std::size_t at = last ? s.rfind(from) : s.find(from);
        REQUIRE(at != std::string::npos);
        return s.replace(at, from.size(), to);
    };
    CHECK(member(replaced("\"names\":[", "\"names\":[1,")) == "names");
    CHECK(member(replaced("\"kind\":\"face\"", "\"kind\":\"solid\"")) == "kind");
    CHECK(member(replaced("\"relation\":\"", "\"relation\":\"x")) == "pickNeighbours[0].relation");
    CHECK(member(replaced("\"points\":[[", "\"points\":[[1,2],[", true)) == "territory.points");
    CHECK(member(replaced("\"repairConfirmed\":false", "\"repairConfirmed\":0")) == "repairConfirmed");
    CHECK(member(replaced("{\"ambiguous\"", "{\"extra\":1,\"ambiguous\"")) == "extra");
    CHECK(member(replaced("\"area\":", "\"area\":1.5,\"x\":")) == "hint.x");
    CHECK(member("{\"body\":\"00000000-0000-0000-0000-000000000000\",\"kind\":\"face\",\"names\":[]}") == "body");
    CHECK_FALSE(topoRefFromJson("{\"body\":").has_value());
    CHECK_FALSE(topoRefFromJson("[]").has_value());
}

TEST_CASE("Resolved: usable for a rebuild, and postable without review", "[naming][evidence]")
{
    const std::vector<SubId> one{SubId{SubKind::Face, 0}};
    auto r = [&](ResolveState s, bool confirmed, bool withElements = true) {
        Resolved out{s, withElements ? one : std::vector<SubId>{}, {}};
        out.repairConfirmed = confirmed;
        return out;
    };
    CHECK(r(ResolveState::Exact, false).usable());
    CHECK(r(ResolveState::Exact, false).postableWithoutReview());
    for (const ResolveState s : {ResolveState::Split, ResolveState::Merged}) {
        CHECK(r(s, false).usable());
        CHECK_FALSE(r(s, false).postableWithoutReview());
        CHECK_FALSE(r(s, true).postableWithoutReview());
    }
    CHECK_FALSE(r(ResolveState::Repaired, false).usable());
    CHECK_FALSE(r(ResolveState::Repaired, false).postableWithoutReview());
    CHECK(r(ResolveState::Repaired, true).usable());
    CHECK(r(ResolveState::Repaired, true).postableWithoutReview());
    CHECK_FALSE(r(ResolveState::Broken, true).usable());
    CHECK_FALSE(r(ResolveState::Broken, true).postableWithoutReview());
    CHECK_FALSE(r(ResolveState::Exact, false, false).usable());
    CHECK_FALSE(r(ResolveState::Exact, false, false).postableWithoutReview());
}

namespace {

// A block with a pocket from the top: x x0..x1, y 20..40, `depth` deep.
Body pocketed(double x0, double x1, double depth)
{
    const Body b = extrude(kBlock, 0, rectangle(0, 0, 100, 60, 1, 1), 20);
    return boolean(b, kPocket, kernel::BoolOp::Cut, 20, rectangle(x0, 20, x1, 40, 21, 3), -depth);
}

// The vertical line edge at (x, y).
std::optional<SubId> verticalEdgeAt(const kernel::Shape& shape, double x, double y)
{
    for (std::uint32_t i = 0; i < shape.count(SubKind::Edge); ++i) {
        const auto c = take(kernel::curveInfo(shape, SubId{SubKind::Edge, i}));
        if (c.type == kernel::CurveType::Line && std::abs(c.start.x - x) < 1e-6 && std::abs(c.end.x - x) < 1e-6 &&
            std::abs(c.start.y - y) < 1e-6 && std::abs(c.end.y - y) < 1e-6)
            return SubId{SubKind::Edge, i};
    }
    return std::nullopt;
}

Hash256 foreignName(const char* text)
{
    Hasher h;
    h.str(text);
    return h.finish();
}

} // namespace

TEST_CASE("An element that moved and grew while a recorded sibling's names are gone is Merged", "[naming][evidence]")
{
    // The pocket's front left corner edge, picked with a sibling piece recorded beside it (T-081a random 17:44: two
    // stretches of one meeting line, named by different features; after the edit only one name is left, on the whole
    // line, which moved). The pocket then moves 10 mm and gets 2 mm deeper: the edge moved and grew.
    const Body picked = pocketed(40, 60, 10);
    const auto corner = verticalEdgeAt(picked.shape, 40, 20);
    REQUIRE(corner);
    TopoRef ref = resolverFor(picked)->makeRef(kBody, *corner);
    REQUIRE(ref.territory.known());
    PickNeighbour stretch;
    stretch.relation = PickNeighbour::Relation::Sibling;
    stretch.names = {foreignName("a stretch named by a feature that is gone")};
    stretch.territory = ref.territory; // where it lay does not matter: the edge no longer covers either
    for (auto& p : stretch.territory.points)
        p[2] -= 40000; // 4 mm lower
    ref.pickNeighbours.push_back(stretch);
    const Body edited = pocketed(50, 70, 12);
    const auto now = verticalEdgeAt(edited.shape, 50, 20);
    REQUIRE(now);
    const Resolved merged = resolverFor(edited)->resolve(ref);
    CHECK(merged.state == ResolveState::Merged);
    CHECK(merged.elements == std::vector<SubId>{*now});
    // Without the sibling it is the plain continuation: Exact.
    TopoRef alone = ref;
    alone.pickNeighbours.pop_back();
    CHECK(resolverFor(edited)->resolve(alone).state == ResolveState::Exact);
    // Moved but not grown: Exact.
    const Body moved = pocketed(50, 70, 10);
    CHECK(resolverFor(moved)->resolve(ref).state == ResolveState::Exact);
}

TEST_CASE("Siblings that no longer lie as they did when picked cannot be told apart: Broken", "[naming][evidence]")
{
    // The pocket's front left corner edge, with the front right corner edge recorded as a sibling (as the pieces of one
    // meeting line are, T-081a Linux holdout3 17:0). Moved together they keep their arrangement: Exact. Moved by
    // different amounts (the pocket moved and narrowed), which is which cannot be told: Broken.
    const Body picked = pocketed(40, 60, 10);
    const auto left = verticalEdgeAt(picked.shape, 40, 20);
    const auto right = verticalEdgeAt(picked.shape, 60, 20);
    REQUIRE(left);
    REQUIRE(right);
    const auto r0 = resolverFor(picked);
    TopoRef ref = r0->makeRef(kBody, *left);
    const TopoRef other = r0->makeRef(kBody, *right);
    PickNeighbour sibling;
    sibling.relation = PickNeighbour::Relation::Sibling;
    sibling.names = other.names;
    sibling.territory = other.territory;
    ref.pickNeighbours.push_back(sibling);
    CHECK(resolverFor(pocketed(50, 70, 10))->resolve(ref).state == ResolveState::Exact);
    const Resolved slid = resolverFor(pocketed(50, 65, 10))->resolve(ref);
    CHECK(slid.state == ResolveState::Broken);
    // A change that leaves the picked edge where it was is not affected.
    CHECK(resolverFor(pocketed(40, 65, 10))->resolve(ref).state == ResolveState::Exact);
}

namespace {

// A block 100 x 60 x 20 with a slot 5 deep across its top, x x0..x0+10 (all of y): the top face in two pieces, each
// named apart by the slot wall beside it.
Body slotted(double x0)
{
    const Body b = extrude(kBlock, 0, rectangle(0, 0, 100, 60, 1, 1), 20);
    return boolean(b, kPocket, kernel::BoolOp::Cut, 20, rectangle(x0, -1, x0 + 10, 61, 21, 3), -5);
}

} // namespace

TEST_CASE("An Exact answer part of whose picked territory is now another element on its surface is Broken",
          "[naming][evidence]")
{
    // The top face piece right of the slot (x 50..100) is picked. The slot then moves 22 mm right, past its own width:
    // the right piece keeps its name (it is still the piece beside the slot's right wall) and shrinks to x 72..100,
    // while the left piece grows over x 50..62, which was the picked piece's. Names alone say Exact (T-081a holdout4
    // 9:9); whether the left piece now continues the pick cannot be told, so the answer is Broken.
    const Body picked = slotted(40);
    const auto right = faceAt(picked.shape, Vec3{80, 30, 20}, Vec3{0, 0, 1});
    REQUIRE(right);
    const TopoRef ref = resolverFor(picked)->makeRef(kBody, *right);
    REQUIRE(ref.territory.known());
    std::size_t inStrip = 0;
    for (const auto& q : ref.territory.points) {
        const Vec3 p = naming::detail::fromQuanta(q);
        inStrip += p.x > 50 && p.x < 62 ? 1 : 0;
    }
    REQUIRE(inStrip > 0);
    REQUIRE(inStrip < ref.territory.points.size());

    const Body moved = slotted(62);
    const auto movedRight = faceAt(moved.shape, Vec3{85, 30, 20}, Vec3{0, 0, 1});
    const auto movedLeft = faceAt(moved.shape, Vec3{55, 30, 20}, Vec3{0, 0, 1});
    REQUIRE(movedRight);
    REQUIRE(movedLeft);
    REQUIRE(*movedRight != *movedLeft);
    // By its names the reference leads to the right piece: without evidence (an older document) it is Exact there.
    const Resolved old = resolverFor(moved)->resolve(withoutEvidence(ref));
    CHECK(old.state == ResolveState::Exact);
    CHECK(old.elements == std::vector<SubId>{*movedRight});
    const Resolved r = resolverFor(moved)->resolve(ref);
    CHECK(r.state == ResolveState::Broken);
    CHECK(r.reason.find("another element on the same surface") != std::string::npos);

    // The slot moved by less than its width: the strip the right piece lost is the slot's now (air at the top), on no
    // element; the piece only shrank, and stays Exact.
    const Body nudged = slotted(45);
    const auto nudgedRight = faceAt(nudged.shape, Vec3{80, 30, 20}, Vec3{0, 0, 1});
    REQUIRE(nudgedRight);
    const Resolved shrunk = resolverFor(nudged)->resolve(ref);
    CHECK(shrunk.state == ResolveState::Exact);
    CHECK(shrunk.elements == std::vector<SubId>{*nudgedRight});
}

namespace {

// A block `length` x 60 x 20 at y y0..y0+60, cut through by a groove along y whose walls meet the bottom: with
// `apart`, a trapezoid with a flat bottom below the block (x 44.5..55.5 at z 0: two solids 11 mm apart), else a V whose
// apex is on the bottom at x 50 (two solids touching along that line). The groove's walls carry the same tags either
// way.
Body grooveCut(const Body& b, double y0, bool apart)
{
    // A profile in the plane y = y0 + 61, swept 62 mm towards -y: (u, v) is (x, z).
    const kernel::Plane plane{Vec3{0, y0 + 61, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0}};
    std::vector<kernel::ProfileCurve> curves;
    if (apart) // the walls cross z 0 at x 44.5 and 55.5
        curves = {line({34, 21}, {45, -1}, 31), line({45, -1}, {55, -1}, 34), line({55, -1}, {66, 21}, 32),
                  line({66, 21}, {34, 21}, 33)};
    else
        curves = {line({40, 21}, {50, 0}, 31), line({50, 0}, {60, 21}, 32), line({60, 21}, {40, 21}, 33)};
    JobContext ctx;
    auto sweep = take(kernel::extrude(kernel::Profile{plane, {kernel::ProfileLoop{curves, 4}}}, 62, 0.0, ctx));
    const NamingInput toolIn{kPocket, "extrude", {}, nullptr, &sweep.op.shape, sweep.origins};
    const Body tool{sweep.op.shape, take(propagate(toolIn)).map};
    auto result = take(kernel::boolean(kernel::BoolOp::Cut, b.shape, tool.shape, ctx));
    const std::array<const ElementMap*, 2> maps{&b.map, &tool.map};
    const std::array<const kernel::Shape*, 2> shapes{&b.shape, &tool.shape};
    const NamingInput in{kPocket, "extrude", maps, &result.history, &result.shape, {}, shapes};
    return Body{result.shape, take(propagate(in)).map};
}

Body grooved(double y0, bool apart, double length = 100)
{
    return grooveCut(extrude(kBlock, 0, rectangle(0, y0, length, y0 + 60, 1, 1), 20), y0, apart);
}

// A block 100 x 60 x 20 at y 5..65 with a round hole of radius 10 through it about (50, 35) (feature kBelow).
Body holed()
{
    const Body b = extrude(kBlock, 0, rectangle(0, 5, 100, 65, 1, 1), 20);
    const kernel::ProfileLoop circle{
        {kernel::ProfileCurve{kernel::ProfileCurve::Kind::Circle, {Vec2{50, 35}}, 10.0, true, 51}}, 6};
    return boolean(b, kBelow, kernel::BoolOp::Cut, -1, circle, 22);
}

// The edges of the hole's rim on the bottom (z 0), sorted.
std::vector<SubId> rimEdges(const kernel::Shape& shape)
{
    std::vector<SubId> out;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Edge); ++i) {
        const auto c = take(kernel::curveInfo(shape, SubId{SubKind::Edge, i}));
        if (c.type == kernel::CurveType::Circle && std::abs(c.center.z) < 1e-6 && std::abs(c.radius - 10) < 1e-6)
            out.push_back(SubId{SubKind::Edge, i});
    }
    return out;
}

// The line edge from a to b (either way round), if there is one.
std::optional<SubId> lineEdge(const kernel::Shape& shape, Vec3 a, Vec3 b)
{
    auto near = [](const Vec3& p, const Vec3& q) {
        return std::abs(p.x - q.x) < 1e-6 && std::abs(p.y - q.y) < 1e-6 && std::abs(p.z - q.z) < 1e-6;
    };
    for (std::uint32_t i = 0; i < shape.count(SubKind::Edge); ++i) {
        const auto c = take(kernel::curveInfo(shape, SubId{SubKind::Edge, i}));
        if (c.type == kernel::CurveType::Line &&
            ((near(c.start, a) && near(c.end, b)) || (near(c.start, b) && near(c.end, a))))
            return SubId{SubKind::Edge, i};
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("An Exact answer that moved and now runs on into another element on the same surfaces is Broken",
          "[naming][evidence]")
{
    // Two solids a V groove cut apart, touching along its apex on the bottom: the front bottom edge of the left one
    // (x 0..50) and of the right one (x 50..100) lie on one line, between planes that are the same on both sides, and
    // share the apex vertex, with nothing geometric between them (T-081a holdout4 5:9, where a sliver face did what the
    // touching solids do here). The Exact stage's answer is the left one, and its pick-time evidence says it was picked
    // 5 mm towards -y (it moved: it covers none of that territory) with no recorded neighbour touching it. Whether the
    // pick now also runs on into the right one cannot be told: Broken. (Names cannot carry an edge through the edits
    // that make such a shape, so the rule is driven directly: the resolver path is the holdout4 5:9 regression.)
    const Body touching = grooved(5, false);
    const auto left = lineEdge(touching.shape, Vec3{0, 5, 0}, Vec3{50, 5, 0});
    const auto right = lineEdge(touching.shape, Vec3{50, 5, 0}, Vec3{100, 5, 0});
    REQUIRE(left);
    REQUIRE(right);
    bool shared = false; // the two share the apex vertex
    for (const SubId& v : kernel::adjacent(touching.shape, *left, SubKind::Vertex))
        for (const SubId& e : kernel::adjacent(touching.shape, v, SubKind::Edge))
            shared = shared || e == *right;
    REQUIRE(shared);
    const auto shifted = [](Territory t, std::int64_t dy) {
        t.qMin[1] += dy;
        t.qMax[1] += dy;
        for (auto& p : t.points)
            p[1] += dy;
        return t;
    };
    constexpr std::int64_t kBack = -50000; // 5 mm, in quanta
    TopoRef ref = resolverFor(touching)->makeRef(kBody, *left);
    REQUIRE(ref.territory.known());
    ref.territory = shifted(ref.territory, kBack);
    ref.hint.qCy += kBack;
    ref.pickNeighbours.clear();
    const naming::detail::TerritoryCache cache(touching.shape);
    const auto out = naming::detail::exactAgainstEvidence(touching.map, touching.shape, cache, *left, ref);
    REQUIRE(out.has_value());
    CHECK(out->state == ResolveState::Broken);
    CHECK(out->reason.find("runs on") != std::string::npos);

    // The solids apart (a flat-bottomed groove): nothing continues the left edge, and the evidence does not object.
    const Body apart = grooved(5, true);
    const auto apartLeft = lineEdge(apart.shape, Vec3{0, 5, 0}, Vec3{44.5, 5, 0});
    REQUIRE(apartLeft);
    const naming::detail::TerritoryCache apartCache(apart.shape);
    CHECK_FALSE(naming::detail::exactAgainstEvidence(apart.map, apart.shape, apartCache, *apartLeft, ref).has_value());

    // A recorded neighbour that touched it at pick time (the line was already in stretches then): the stretch beyond
    // the vertex may be that neighbour moved along with it, and the rule does not apply.
    TopoRef stretches = ref;
    PickNeighbour beyond;
    beyond.relation = PickNeighbour::Relation::SameCarrier;
    beyond.names = {foreignName("the stretch beyond the apex at pick time")};
    beyond.territory = shifted(resolverFor(touching)->makeRef(kBody, *right).territory, kBack);
    stretches.pickNeighbours.push_back(beyond);
    CHECK_FALSE(
        naming::detail::exactAgainstEvidence(touching.map, touching.shape, cache, *left, stretches).has_value());
    // Without evidence (an older document) nothing is checked.
    CHECK_FALSE(naming::detail::exactAgainstEvidence(touching.map, touching.shape, cache, *left, withoutEvidence(ref))
                    .has_value());
}

TEST_CASE("An Exact answer that stops short of where the pick ran, the rest on the same line under another name, is "
          "not Exact",
          "[naming][evidence]")
{
    // The completeness check where nothing moved (T-081a holdout5 1:2: a hole met the face beside a picked edge and
    // put a vertex on it 0.18 mm from its end; the stretch beyond kept no name of the pick). A block 51 mm long has a V
    // groove whose apex touches its bottom at x 50: the front bottom edge is now 0..50 (`left`) and 50..51 (`right`,
    // on the sliver solid beyond the apex), on one line between planes that are the same on both sides. The edge was
    // picked on the plain block, 0..51: every point of its territory is on `left` (nothing moved), and `right` lies
    // inside where it was picked. The pick came apart into the two, and that set is provably all of it (a picked line
    // segment is exactly its box's stretch of its line): Split {left, right}.
    const Body touching = grooved(5, false, 51);
    const auto left = lineEdge(touching.shape, Vec3{0, 5, 0}, Vec3{50, 5, 0});
    const auto right = lineEdge(touching.shape, Vec3{50, 5, 0}, Vec3{51, 5, 0});
    REQUIRE(left);
    REQUIRE(right);
    const naming::detail::TerritoryCache cache(touching.shape);
    auto pickOn = [](double length) {
        const Body plain = extrude(kBlock, 0, rectangle(0, 5, length, 65, 1, 1), 20);
        const auto edge = lineEdge(plain.shape, Vec3{0, 5, 0}, Vec3{length, 5, 0});
        REQUIRE(edge);
        return resolverFor(plain)->makeRef(kBody, *edge);
    };
    const TopoRef whole = pickOn(51);
    REQUIRE(whole.territory.known());
    const naming::detail::Probe onLeft(touching.shape, *left);
    REQUIRE(onLeft.count(whole.territory) == whole.territory.points.size());
    const auto split = naming::detail::exactAgainstEvidence(touching.map, touching.shape, cache, *left, whole);
    REQUIRE(split.has_value());
    CHECK(split->state == ResolveState::Split);
    std::vector<SubId> both{*left, *right};
    std::sort(both.begin(), both.end());
    CHECK(split->elements == both);

    // Picked 0..50.5: `right` reaches into where it was picked but also beyond it (the edge grew past the vertex), so
    // the set is not provably the pick: Broken.
    const TopoRef shorter = pickOn(50.5);
    const auto broken = naming::detail::exactAgainstEvidence(touching.map, touching.shape, cache, *left, shorter);
    REQUIRE(broken.has_value());
    CHECK(broken->state == ResolveState::Broken);
    CHECK(broken->reason.find("runs on") != std::string::npos);

    // Picked 0..50: `right` only touches where it was picked, at the vertex; `left` is all of the pick.
    const TopoRef exact = pickOn(50);
    CHECK_FALSE(
        naming::detail::completeAgainstEvidence(touching.map, touching.shape, cache, {*left}, exact).has_value());
    CHECK_FALSE(naming::detail::exactAgainstEvidence(touching.map, touching.shape, cache, *left, exact).has_value());
    // Without evidence (an older document) nothing is checked.
    CHECK_FALSE(naming::detail::exactAgainstEvidence(touching.map, touching.shape, cache, *left, withoutEvidence(whole))
                    .has_value());
}

namespace {

// `body` with the line edge from a to b chamfered by `size` (feature kBelow).
Body chamfered(const Body& body, double size, Vec3 a, Vec3 b)
{
    const auto edge = lineEdge(body.shape, a, b);
    REQUIRE(edge);
    JobContext ctx;
    const std::array<SubId, 1> edges{*edge};
    auto result = take(kernel::chamfer(body.shape, edges, size, ctx));
    const std::array<const ElementMap*, 1> maps{&body.map};
    const NamingInput in{kBelow, "chamfer", maps, &result.history, &result.shape, {}};
    return Body{result.shape, take(propagate(in)).map};
}

// A block 76.2 x 50.8 x 31.75 with its two right-hand edges along y chamfered (0.762 at the top, 0.82 at the bottom),
// cut by a rectangle 2hw x 2hh, turned about y, in the plane through o = (75.972, 50.7999992, 16.514) facing +y, swept
// 27.909 mm towards -y (T-081a holdout5 1:7, the import's other chamfers left out). The random harness put the plane
// through a mesh sample of the back face (single precision), 7.6e-7 mm inside it: the cut leaves a slab of material
// that thin, and where the cut runs out through the right face the slab's end is a face of the right face's plane,
// 30 mm long and 7.6e-7 mm wide (kernel::boolean, with or without its same-domain unification).
Body backCut(double hw, double hh)
{
    Body b = extrude(kBlock, 0, rectangle(0, 0, 76.2, 50.8, 1, 1), 31.75);
    b = chamfered(b, 0.762, Vec3{76.2, 0, 31.75}, Vec3{76.2, 50.8, 31.75});
    b = chamfered(b, 0.82, Vec3{76.2, 0, 0}, Vec3{76.2, 50.8, 0});
    const kernel::Plane plane{Vec3{75.9721684, 50.7999992, 16.5142589}, Vec3{0, 1, 0},
                              Vec3{0.94812155, 0, -0.317908046}};
    JobContext ctx;
    auto sweep =
        take(kernel::extrude(kernel::Profile{plane, {rectangle(-hw, -hh, hw, hh, 41, 5)}}, -27.9085221, 0.0, ctx));
    const NamingInput toolIn{kPocket, "extrude", {}, nullptr, &sweep.op.shape, sweep.origins};
    const Body tool{sweep.op.shape, take(propagate(toolIn)).map};
    auto result = take(kernel::boolean(kernel::BoolOp::Cut, b.shape, tool.shape, ctx));
    const std::array<const ElementMap*, 2> maps{&b.map, &tool.map};
    const std::array<const kernel::Shape*, 2> shapes{&b.shape, &tool.shape};
    const NamingInput in{kPocket, "extrude", maps, &result.history, &result.shape, {}, shapes};
    return Body{result.shape, take(propagate(in)).map};
}

} // namespace

TEST_CASE("An Exact answer that is one arc of a picked circle now in several, on the same surfaces, is Split into all "
          "of them",
          "[naming][evidence]")
{
    // The completeness check on a circle (as holdout5 1:2 on an arc): the rim of a hole on the bottom face, picked
    // whole, is split by a V groove whose apex touches the bottom along x 50: its arcs on either side lie on the same
    // circle between the same plane and cylinder. An answer that is one of them (the names cannot carry the rim through
    // to that, so the rule is driven directly) leaves points of the rim's territory on the others: the pick came
    // apart, and the arcs together are all of it.
    const Body plain = holed();
    const std::vector<SubId> rim = rimEdges(plain.shape);
    REQUIRE(rim.size() == 1);
    const TopoRef ref = resolverFor(plain)->makeRef(kBody, rim.front());
    REQUIRE(ref.territory.known());
    const Body split = grooveCut(holed(), 5, false);
    const std::vector<SubId> arcs = rimEdges(split.shape);
    REQUIRE(arcs.size() >= 2);
    const naming::detail::TerritoryCache cache(split.shape);
    const auto out = naming::detail::exactAgainstEvidence(split.map, split.shape, cache, arcs.front(), ref);
    REQUIRE(out.has_value());
    INFO(out->reason);
    CHECK(out->state == ResolveState::Split);
    CHECK(out->elements == arcs);
    // The same with the groove apart (a flat bottom 11 mm wide): the rim lost its stretch across the groove, and
    // the arcs left are still all of it.
    const Body apart = grooveCut(holed(), 5, true);
    const std::vector<SubId> left = rimEdges(apart.shape);
    REQUIRE(left.size() >= 2);
    const naming::detail::TerritoryCache apartCache(apart.shape);
    const auto pieces = naming::detail::exactAgainstEvidence(apart.map, apart.shape, apartCache, left.front(), ref);
    REQUIRE(pieces.has_value());
    CHECK(pieces->state == ResolveState::Split);
    CHECK(pieces->elements == left);
}

TEST_CASE("A piece of zero area is never part of an answer", "[naming][evidence]")
{
    // T-081a holdout5 1:7. The right face (x 76.2) is picked with the cut as first made: it runs out through the right
    // face only where it is narrow, and the face stays whole. The cut made larger takes all of the right face from y
    // 22.9 to the back, except the slab's end, a face of zero area (zeroMeasure: 7.6e-7 mm wide): the right face's
    // name comes apart into the big piece and that one. The answer is the big piece alone: Split (the name is gone),
    // which its own evidence does not object to. kernel::boolean now merges a face that close (boolean.cpp SLIVERS):
    // the cuts are made with that off, so the slab is still there.
    const kernel::testing::SliverMergeOff keepSlab;
    const Body picked = backCut(9.135638, 13.318216);
    const auto right = faceAt(picked.shape, Vec3{76.2, 5, 15}, Vec3{1, 0, 0});
    REQUIRE(right);
    const TopoRef ref = resolverFor(picked)->makeRef(kBody, *right);
    REQUIRE(ref.territory.known());

    const Body larger = backCut(10.555159, 15.387637);
    const auto piece = faceAt(larger.shape, Vec3{76.2, 5, 15}, Vec3{1, 0, 0});
    REQUIRE(piece);
    std::vector<SubId> zero; // faces of the right face's plane with (next to) no area
    for (std::uint32_t i = 0; i < larger.shape.count(SubKind::Face); ++i) {
        const auto info = take(kernel::surfaceInfo(larger.shape, SubId{SubKind::Face, i}));
        if (info.type == kernel::SurfaceType::Plane && std::abs(info.axis.x) > 0.999 && info.area < 1e-3 &&
            std::abs(info.centroid.x - 76.2) < 1e-3)
            zero.push_back(SubId{SubKind::Face, i});
    }
    REQUIRE(zero.size() == 1);
    CHECK(naming::detail::zeroMeasure(larger.shape, zero.front(), ref.hint));
    CHECK_FALSE(naming::detail::zeroMeasure(larger.shape, *piece, ref.hint));
    const Resolved r = resolverFor(larger)->resolve(ref);
    CHECK(r.state == ResolveState::Split);
    CHECK(r.elements == std::vector<SubId>{*piece});
    CHECK(r.usable());
    CHECK_FALSE(r.postableWithoutReview());
}

TEST_CASE("With the input shapes, an input face the history reports deleted but the result absorbed lends its name",
          "[naming][evidence]")
{
    // A block and a second block beside it, their tops coplanar: the union has one top face. The history is edited to
    // report the second block's top Deleted (a boolean that unified the two without saying so): with the input shapes,
    // propagate() sees that the result's top covers it and gives it that face's name as well; without, it does not.
    const Body a = extrude(kBlock, 0, rectangle(0, 0, 100, 60, 1, 1), 20);
    const Body b = extrude(kBelow, 0, rectangle(100, 0, 150, 60, 11, 2), 20);
    JobContext ctx;
    auto fused = take(kernel::boolean(kernel::BoolOp::Union, a.shape, b.shape, ctx));
    const auto toolTop = faceAt(b.shape, Vec3{125, 30, 20}, Vec3{0, 0, 1});
    const auto top = faceAt(fused.shape, Vec3{125, 30, 20}, Vec3{0, 0, 1});
    REQUIRE(toolTop);
    REQUIRE(top);
    bool edited = false;
    for (kernel::HistoryEntry& e : fused.history.entries)
        if (e.inputIndex == 1 && e.source == *toolTop && e.relation == kernel::HistoryEntry::Relation::Modified) {
            e.relation = kernel::HistoryEntry::Relation::Deleted;
            e.results.clear();
            edited = true;
        }
    REQUIRE(edited);
    const std::array<const ElementMap*, 2> maps{&a.map, &b.map};
    const std::array<const kernel::Shape*, 2> shapes{&a.shape, &b.shape};
    const ElementMap with =
        take(propagate(NamingInput{kPocket, "extrude", maps, &fused.history, &fused.shape, {}, shapes})).map;
    const ElementMap without =
        take(propagate(NamingInput{kPocket, "extrude", maps, &fused.history, &fused.shape, {}})).map;
    const Hash256 toolName = b.map.namesOf(*toolTop).front();
    const std::span<const Hash256> withNames = with.namesOf(*top);
    const std::span<const Hash256> withoutNames = without.namesOf(*top);
    CHECK(std::find(withNames.begin(), withNames.end(), toolName) != withNames.end());
    CHECK(std::find(withoutNames.begin(), withoutNames.end(), toolName) == withoutNames.end());
    // The second block's top, picked before the union, resolves Merged to the whole top.
    const TopoRef ref = makeResolver({ResolverBody{kBody, b.map, b.shape}})->makeRef(kBody, *toolTop);
    const Resolved r = makeResolver({ResolverBody{kBody, with, fused.shape}})->resolve(ref);
    CHECK(r.state == ResolveState::Merged);
    CHECK(r.elements == std::vector<SubId>{*top});
    // Input shapes that do not match the maps are refused.
    const std::array<const kernel::Shape*, 1> one{&a.shape};
    CHECK_FALSE(propagate(NamingInput{kPocket, "extrude", maps, &fused.history, &fused.shape, {}, one}).has_value());
}
