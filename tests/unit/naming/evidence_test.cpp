// Pick-time evidence in TopoRef (naming.h Territory, PickNeighbour), its JSON form, and Resolved::usable() /
// postableWithoutReview(), on small parts built with the kernel's extrude and boolean.
#include <catch2/catch_test_macros.hpp>

#include "naming_test_util.h"

#include <naming_testing.h>
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
    ref.outline = PickOutline{};
    ref.samples = naming::PickSamples{}; // (round 8: an older document has no samples either)
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
        CHECK(back.outline.chords == ref.outline.chords);
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
    CHECK(old.find("outline") == std::string::npos);
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
    CHECK(member(replaced("\"chords\":[[", "\"chords\":[[1,2,3,4,5,6],[")) == "outline.chords");
    CHECK(member(replaced("\"outline\":{", "\"outline\":{\"slack\":1,")) == "outline.slack");
    CHECK(member(replaced("{\"ambiguous\"", "{\"extra\":1,\"ambiguous\"")) == "extra");
    CHECK(member(replaced("\"area\":", "\"area\":1.5,\"x\":")) == "hint.x");
    CHECK(member("{\"body\":\"00000000-0000-0000-0000-000000000000\",\"kind\":\"face\",\"names\":[]}") == "body");
    CHECK_FALSE(topoRefFromJson("{\"body\":").has_value());
    CHECK_FALSE(topoRefFromJson("[]").has_value());
}

TEST_CASE("A planar face's reference records its outline, within its cap, the same on every platform",
          "[naming][evidence]")
{
    // A 100 x 60 x 20 block with a 10 mm round hole through it (T-081b round 2: TopoRef::outline).
    Body b = extrude(kBlock, 0, rectangle(0, 0, 100, 60, 1, 1), 20);
    kernel::ProfileLoop hole{{kernel::ProfileCurve{kernel::ProfileCurve::Kind::Circle, {{30, 30}}, 5.0, true, 31}}, 4};
    b = boolean(b, kPocket, kernel::BoolOp::Cut, -1, std::move(hole), 22);
    const auto top = faceAt(b.shape, Vec3{5, 5, 20}, Vec3{0, 0, 1});
    REQUIRE(top);
    const TopoRef ref = resolverFor(b)->makeRef(kBody, *top);
    REQUIRE(ref.outline.known());
    CHECK(ref.outline.chords.size() <= kMaxOutlineChords);
    // Four lines (one chord each, no slack) and the hole's circle in the rest of the chords, each within its slack.
    std::size_t lines = 0;
    for (const auto& c : ref.outline.chords)
        lines += c[6] == 0 ? 1 : 0;
    CHECK(lines == 4);
    CHECK(ref.outline.chords.size() == 4 + (kMaxOutlineChords - 4));
    for (const auto& c : ref.outline.chords) {
        CHECK(c[2] == 200000); // z 20 mm in quanta (1e-4 mm)
        CHECK(c[5] == 200000);
        CHECK(c[6] < 100); // under 0.01 mm from the circle
    }
    // The same bits on every platform: a hash of the JSON form, compared with the value Windows and Linux both give.
    const std::string text = toJson(ref);
    const std::size_t at = text.find("\"outline\":");
    REQUIRE(at != std::string::npos);
    const std::string outline = text.substr(at, text.find("]]}", at) + 3 - at);
    Hasher h;
    h.str(outline);
    INFO(outline);
    CHECK(h.finish().hex().substr(0, 16) == "28887bde1e5e317a");
    // Edges and vertices, and faces that are not planar, have none.
    for (const SubId& s : test::allElements(b.shape)) {
        const TopoRef r = resolverFor(b)->makeRef(kBody, s);
        if (s.kind != SubKind::Face)
            CHECK_FALSE(r.outline.known());
        else if (take(kernel::surfaceInfo(b.shape, s)).type != kernel::SurfaceType::Plane)
            CHECK_FALSE(r.outline.known());
    }
}

TEST_CASE("A planar face with more arcs than the outline cap allows records none, and resolves as before",
          "[naming][evidence]")
{
    // 25 round holes: 4 lines + 25 x 4 chords > kMaxOutlineChords.
    Body b = extrude(kBlock, 0, rectangle(0, 0, 100, 100, 1, 1), 10);
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j)
            b = boolean(b, test::featureId(static_cast<std::uint8_t>(100 + 5 * i + j)), kernel::BoolOp::Cut, -1,
                        kernel::ProfileLoop{
                            {kernel::ProfileCurve{
                                kernel::ProfileCurve::Kind::Circle, {{10.0 + 20 * i, 10.0 + 20 * j}}, 2.0, true, 40}},
                            5},
                        12);
    const auto top = faceAt(b.shape, Vec3{1, 1, 10}, Vec3{0, 0, 1});
    REQUIRE(top);
    const TopoRef ref = resolverFor(b)->makeRef(kBody, *top);
    CHECK_FALSE(ref.outline.known());
    CHECK(ref.territory.known());
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
    // By its names the reference leads to the right piece: without evidence (an older document) it is suggested there
    // (T-081b round 9: a reference without samples is Exact only by generator identity, and a piece is not a whole
    // generator element): Repaired, not usable until confirmed.
    const Resolved old = resolverFor(moved)->resolve(withoutEvidence(ref));
    CHECK(old.state == ResolveState::Repaired);
    CHECK_FALSE(old.usable());
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

namespace {

// A pocket 10 x 20, 5 deep, cut into a 100 x 60 x 20 block with its sketch frame at (x0, 20, 20) (feature kPocket),
// its placement given to propagate() (T-081b round 4) or not.
Body placedPocket(double x0, bool placed)
{
    const Body block = extrude(kBlock, 0, rectangle(0, 0, 100, 60, 1, 1), 20);
    JobContext ctx;
    const kernel::Plane plane{Vec3{x0, 20, 20}, Vec3{0, 0, 1}, Vec3{1, 0, 0}};
    auto sweep = take(kernel::extrude(kernel::Profile{plane, {rectangle(0, 0, 10, 20, 41, 5)}}, -5, 0.0, ctx));
    const Xform frame{{1, 0, 0, x0, 0, 1, 0, 20, 0, 0, 1, 20}};
    NamingInput toolIn{kPocket, "extrude", {}, nullptr, &sweep.op.shape, sweep.origins};
    if (placed)
        toolIn.placement = frame;
    const ElementMap toolMap = take(propagate(toolIn)).map;
    auto result = take(kernel::boolean(kernel::BoolOp::Cut, block.shape, sweep.op.shape, ctx));
    const std::array<const ElementMap*, 2> maps{&block.map, &toolMap};
    const std::array<const kernel::Shape*, 2> shapes{&block.shape, &sweep.op.shape};
    NamingInput in{kPocket, "extrude", maps, &result.history, &result.shape, {}, shapes};
    if (placed)
        in.placement = frame;
    return Body{result.shape, take(propagate(in)).map};
}

} // namespace

TEST_CASE("A feature's placement is carried from map to map, recorded by a reference, and followed when it moves",
          "[naming][evidence]")
{
    // T-081b round 4: NamingInput::placement, ElementMap::placement, TopoRef::placements.
    const Body b = placedPocket(30, true);
    const FeaturePlacement* p = b.map.placement(kPocket);
    REQUIRE(p != nullptr);
    CHECK(samePlacement(*p, placementOf(kPocket, Xform{{1, 0, 0, 30, 0, 1, 0, 20, 0, 0, 1, 20}})));
    CHECK_FALSE(samePlacement(*p, placementOf(kPocket, Xform{{1, 0, 0, 30.01, 0, 1, 0, 20, 0, 0, 1, 20}})));
    CHECK(b.map.placement(kBlock) == nullptr);
    // The map file: version 2 with the placement, read back the same; a map without one is version 1, as before.
    const std::vector<std::byte> bytes = b.map.serialize();
    CHECK(std::to_integer<int>(bytes[4]) == 2);
    const ElementMap back = take(ElementMap::deserialize(bytes));
    REQUIRE(back.placement(kPocket) != nullptr);
    CHECK(back.placement(kPocket)->q == p->q);
    CHECK(back.serialize() == bytes);
    CHECK(std::to_integer<int>(placedPocket(30, false).map.serialize()[4]) == 1);
    // The pocket's floor: its reference records the pocket's placement, and its JSON form keeps it.
    const auto floor = faceAt(b.shape, Vec3{35, 30, 15}, Vec3{0, 0, 1});
    REQUIRE(floor);
    const TopoRef ref = resolverFor(b)->makeRef(kBody, *floor);
    REQUIRE(ref.placements.size() == 1);
    CHECK(ref.placements[0].feature == kPocket);
    const std::string text = toJson(ref);
    CHECK(text.find("\"placements\":[{\"feature\":\"") != std::string::npos);
    const TopoRef read = take(topoRefFromJson(text));
    REQUIRE(read.placements.size() == 1);
    CHECK(read.placements[0].q == ref.placements[0].q);
    CHECK(toJson(read) == text);
    CHECK(toJson(withoutEvidence(ref)).find("placements") != std::string::npos); // not evidence: written whenever known
    // The pocket moved to x 95: clipped by the block's end, its floor half as large and nowhere where it was. Carried
    // by
    // the pocket's own move it lies wholly inside the picked floor: Exact. Without the placement the move is guessed
    // (the floor neither in place nor over its place along its normal): Broken. (T-081b round 9: the floor, the
    // pocket's cap, is a whole generator element, but it still lies on the picked plane, so its samples decide, not
    // generator identity.)
    const Body moved = placedPocket(95, true);
    const auto movedFloor = faceAt(moved.shape, Vec3{97, 30, 15}, Vec3{0, 0, 1});
    REQUIRE(movedFloor);
    CHECK(ref.samples.whole);
    const Resolved r = resolverFor(moved)->resolve(ref);
    CHECK(r.state == ResolveState::Exact);
    CHECK_FALSE(r.identity);
    CHECK(r.elements == std::vector<SubId>{*movedFloor});
    TopoRef unplaced = ref;
    unplaced.placements.clear();
    unplaced.samples = naming::PickSamples{}; // a reference made before placements has no samples (round 8) either
    CHECK(resolverFor(moved)->resolve(unplaced).state == ResolveState::Broken);
    // Sorted by feature, each once: a damaged list is refused, naming the member.
    std::string twice = text;
    const std::size_t at = twice.find("\"placements\":[");
    const std::size_t end = twice.find("]}]", at) + 2;
    const std::string one = twice.substr(at + 14, end - at - 14);
    twice.insert(end, "," + one);
    const auto refused = topoRefFromJson(twice);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().context.front() == "placements");
}

// ---------------------------------------------------------------- T-081b round 9
namespace {

const FeatureId kImport = test::featureId(64);
const FeatureId kMove = test::featureId(65);

// The quadrilateral of setup_document_test.cpp: (0, 0), (40, y2), (40, 20), (0, 20), line 1 from the first point to the
// second; extruded `height` with its placement (naming::sweepPlacement), as the document extrudes it.
Body placedPrism(double y2, double height)
{
    JobContext ctx;
    const kernel::Plane plane{Vec3{0, 0, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0}};
    const std::vector<kernel::ProfileLoop> loops{kernel::ProfileLoop{
        {line({0, 0}, {40, y2}, 1), line({40, y2}, {40, 20}, 2), line({40, 20}, {0, 20}, 3), line({0, 20}, {0, 0}, 4)},
        9}};
    auto sweep = take(kernel::extrude(kernel::Profile{plane, loops}, height, 0.0, ctx));
    NamingInput in{kBlock, "extrude", {}, nullptr, &sweep.op.shape, sweep.origins};
    in.placement = sweepPlacement(plane, std::span<const kernel::ProfileLoop>(loops));
    return Body{sweep.op.shape, take(propagate(in)).map};
}

// An imported box 0..10 x 0..20 x 0..5 (feature kImport), moved by `by` with a body placement (kMove) or not.
Body movedImport(Vec3 by, bool bodyPlacement)
{
    const kernel::Shape box = take(kernel::makeBox(Vec3{0, 0, 0}, Vec3{10, 20, 5}));
    const ElementMap map = nameImported(kImport, box);
    const Xform move{{1, 0, 0, by.x, 0, 1, 0, by.y, 0, 0, 1, by.z}};
    auto moved = take(kernel::transformed(box, move));
    const std::array<const ElementMap*, 1> maps{&map};
    NamingInput in{kMove, "transform_body", maps, &moved.history, &moved.shape, {}, {}};
    if (bodyPlacement)
        in.bodyPlacement = move;
    return Body{moved.shape, take(propagate(in)).map};
}

} // namespace

TEST_CASE("Generator identity (round 9): a whole generator element its generator reshaped is the element its name "
          "leads to",
          "[naming][evidence][identity]")
{
    // setup_document_test.cpp's WCS edge: the top edge over sketch line 1, then the line turned (its second point up
    // to y 10) and the prism made taller (10 -> 15). The placement (the centre of the profile's box) did not change, so
    // the samples stay where they were, and nothing of them lies on the edge or the face now: round 8 said Broken for
    // the edge and kept the face by a guess (the plane moved along its normal). Both are whole generator elements,
    // their surfaces reshaped by their own generator: Exact by identity.
    const Body before = placedPrism(0, 10);
    const auto edge = lineEdge(before.shape, Vec3{0, 0, 10}, Vec3{40, 0, 10});
    const auto top = faceAt(before.shape, Vec3{20, 10, 10}, Vec3{0, 0, 1});
    const auto kept = lineEdge(before.shape, Vec3{0, 20, 0}, Vec3{0, 0, 0});
    REQUIRE(edge);
    REQUIRE(top);
    REQUIRE(kept);
    const auto pick = resolverFor(before);
    const TopoRef edgeRef = pick->makeRef(kBody, *edge);
    const TopoRef topRef = pick->makeRef(kBody, *top);
    const TopoRef keptRef = pick->makeRef(kBody, *kept);
    CHECK(edgeRef.samples.version == kPickSamplesVersion);
    CHECK(edgeRef.samples.whole);
    CHECK(topRef.samples.whole);
    CHECK(keptRef.samples.whole);
    // The flag is evidence: the JSON form keeps it (version 2; since round 10 the form is version 3), and a version-1
    // form still loads (as version 1).
    const std::string text = toJson(edgeRef);
    CHECK(text.find("\"version\":3,\"whole\":true}") != std::string::npos);
    CHECK(take(topoRefFromJson(text)).samples.whole);
    CHECK(toJson(take(topoRefFromJson(text))) == text);

    const Body after = placedPrism(10, 15);
    const auto edgeNow = lineEdge(after.shape, Vec3{0, 0, 15}, Vec3{40, 10, 15});
    const auto topNow = faceAt(after.shape, Vec3{20, 12, 15}, Vec3{0, 0, 1});
    const auto keptNow = lineEdge(after.shape, Vec3{0, 20, 0}, Vec3{0, 0, 0});
    REQUIRE(edgeNow);
    REQUIRE(topNow);
    REQUIRE(keptNow);
    const auto now = resolverFor(after);
    Resolved r = now->resolve(edgeRef);
    CHECK(r.state == ResolveState::Exact);
    CHECK(r.identity);
    CHECK(r.elements == std::vector<SubId>{*edgeNow});
    CHECK(r.postableWithoutReview());
    r = now->resolve(topRef);
    CHECK(r.state == ResolveState::Exact);
    CHECK(r.identity);
    CHECK(r.elements == std::vector<SubId>{*topNow});
    // On its own surfaces still (the bottom edge over line 4): decided by its samples, which it holds. Exact, not by
    // identity.
    r = now->resolve(keptRef);
    CHECK(r.state == ResolveState::Exact);
    CHECK_FALSE(r.identity);
    CHECK(r.elements == std::vector<SubId>{*keptNow});
    // A reference without samples (an older document): the name alone, pure and held by one element, decides.
    r = now->resolve(withoutEvidence(edgeRef));
    CHECK(r.state == ResolveState::Exact);
    CHECK(r.identity);

    // The mutation check: identity off, the edge is Broken (round 8) and the face only a suggestion (the guess).
    naming::testing::setGeneratorIdentityEnabled(false);
    const Resolved offEdge = now->resolve(edgeRef);
    const Resolved offTop = now->resolve(topRef);
    const Resolved offOld = now->resolve(withoutEvidence(edgeRef));
    naming::testing::setGeneratorIdentityEnabled(true);
    CHECK(offEdge.state == ResolveState::Broken);
    CHECK(offTop.state == ResolveState::Repaired);
    CHECK_FALSE(offTop.usable());
    CHECK(offOld.state != ResolveState::Exact);
}

TEST_CASE("Generator identity (round 9): a piece of a split face never is; its ground is known at pick time",
          "[naming][evidence][identity]")
{
    // slotted(): the slot splits the block's top face in two; the right piece is picked. Its one name is a split
    // piece's: not a whole generator element, at pick time or later, whatever its reference says.
    const Body picked = slotted(40);
    const auto right = faceAt(picked.shape, Vec3{80, 30, 20}, Vec3{0, 0, 1});
    const auto front = faceAt(picked.shape, Vec3{5, 0, 10}, Vec3{0, -1, 0});
    REQUIRE(right);
    REQUIRE(front);
    const TopoRef ref = resolverFor(picked)->makeRef(kBody, *right);
    REQUIRE(ref.names.size() == 1);
    const TopoName* piece = picked.map.record(ref.names[0]);
    REQUIRE(piece != nullptr);
    REQUIRE(piece->role == "split");
    CHECK_FALSE(ref.samples.whole);
    CHECK(resolverFor(picked)->makeRef(kBody, *front).samples.whole); // the block's side, notched, is whole
    // Its ground: the block's, up the split record's parent, which no element holds any more (the map keeps it as an
    // ancestor record).
    for (const Hash256& parent : piece->parents) {
        CHECK(picked.map.record(parent) == nullptr);
        CHECK(naming::detail::recordOrAncestor(picked.map, parent) != nullptr);
    }
    REQUIRE(!ref.samples.carriers.empty());
    REQUIRE(ref.samples.carriers[0].ground >= 0);
    CHECK(ref.samples.grounds[static_cast<std::size_t>(ref.samples.carriers[0].ground)].feature == kBlock);
    naming::testing::setAncestorsEnabled(false);
    const TopoRef without = resolverFor(picked)->makeRef(kBody, *right);
    naming::testing::setAncestorsEnabled(true);
    CHECK(without.samples.carriers[0].ground == -1); // the mutation: only the records the map holds

    // The slot moved past its own width: the piece's name is still on the right piece, which holds part of the pick
    // and part of it is now the left piece's (an Exact answer part of whose territory is another element): Broken. A
    // reference forged to say the piece was whole is still no identity (the name is a split piece's now too), and
    // neither is one without samples (Repaired, a suggestion).
    const Body moved = slotted(62);
    TopoRef forged = ref;
    forged.samples.whole = true;
    for (const TopoRef* r : std::array<const TopoRef*, 2>{&ref, &forged}) {
        const Resolved res = resolverFor(moved)->resolve(*r);
        CHECK(res.state == ResolveState::Broken);
        CHECK_FALSE(res.identity);
    }
    const Resolved old = resolverFor(moved)->resolve(withoutEvidence(ref));
    CHECK(old.state == ResolveState::Repaired);
    CHECK_FALSE(old.identity);
}

TEST_CASE("Body moves (round 9): a body moved rigidly carries what was picked on it", "[naming][evidence][body]")
{
    // An imported box (no feature places its faces) moved by 30 along x with a body placement (transform_body's): the
    // map records the move with the features it carried; a face picked before the move is followed by it. Without the
    // body placement only a guess (a rigid copy somewhere else) is left: a suggestion.
    const Body still = movedImport(Vec3{0, 0, 0}, false);
    const auto top = faceAt(still.shape, Vec3{5, 10, 5}, Vec3{0, 0, 1});
    REQUIRE(top);
    const TopoRef ref = resolverFor(still)->makeRef(kBody, *top);
    CHECK_FALSE(ref.samples.whole); // an imported face's name is made from its geometry
    REQUIRE(ref.samples.carriers.size() == 1);
    REQUIRE(ref.samples.carriers[0].ground >= 0);
    CHECK(ref.samples.grounds[0].feature == kImport);

    const Body moved = movedImport(Vec3{30, 0, 0}, true);
    const std::vector<const FeaturePlacement*> moves = naming::detail::bodyMovesOf(moved.map, kImport);
    REQUIRE(moves.size() == 1);
    CHECK(samePlacement(*moves[0], placementOf(kMove, Xform{{1, 0, 0, 30, 0, 1, 0, 0, 0, 0, 1, 0}})));
    const auto movedTop = faceAt(moved.shape, Vec3{35, 10, 5}, Vec3{0, 0, 1});
    REQUIRE(movedTop);
    Resolved r = resolverFor(moved)->resolve(ref);
    CHECK(r.state == ResolveState::Exact);
    CHECK_FALSE(r.identity);
    CHECK(r.elements == std::vector<SubId>{*movedTop});
    // Picked after the move, the move then edited (30 -> 50): followed from the one to the other.
    const TopoRef afterMove = resolverFor(moved)->makeRef(kBody, *movedTop);
    const Body further = movedImport(Vec3{50, 0, 0}, true);
    const auto furtherTop = faceAt(further.shape, Vec3{55, 10, 5}, Vec3{0, 0, 1});
    REQUIRE(furtherTop);
    r = resolverFor(further)->resolve(afterMove);
    CHECK(r.state == ResolveState::Exact);
    CHECK(r.elements == std::vector<SubId>{*furtherTop});
    // Without the body placement, and the mutation check (body moves off): a suggestion, never Exact.
    CHECK(resolverFor(movedImport(Vec3{30, 0, 0}, false))->resolve(ref).state == ResolveState::Repaired);
    naming::testing::setBodyMovesEnabled(false);
    const Resolved off = resolverFor(moved)->resolve(ref);
    naming::testing::setBodyMovesEnabled(true);
    CHECK(off.state != ResolveState::Exact);
}

// ---------------------------------------------------------------- T-081b round 10
namespace {

const FeatureId kPush = test::featureId(66);
const FeatureId kBoss = test::featureId(67);

// An imported box 0..10 x 0..20 x 0..5 (kImport), its top face pushed out by `distance` (kernel::offsetFaces, feature
// kPush), the push's own move given to naming as the document gives it (naming::offsetFaceMove) or not.
Body pushedImport(double distance, bool moves)
{
    const kernel::Shape box = take(kernel::makeBox(Vec3{0, 0, 0}, Vec3{10, 20, 5}));
    const ElementMap map = nameImported(kImport, box);
    const auto top = faceAt(box, Vec3{5, 10, 5}, Vec3{0, 0, 1});
    REQUIRE(top);
    JobContext ctx;
    const std::array<SubId, 1> faces{*top};
    auto pushed = take(kernel::offsetFaces(box, faces, distance, ctx));
    const std::array<const ElementMap*, 1> maps{&map};
    const std::array<const kernel::Shape*, 1> shapes{&box};
    NamingInput in{kPush, "offset_face", maps, &pushed.history, &pushed.shape, {}, shapes};
    std::vector<FaceMove> list;
    if (moves)
        if (const auto m = offsetFaceMove(box, *top, distance))
            list.push_back(*m);
    in.faceMoves = list;
    return Body{pushed.shape, take(propagate(in)).map};
}

} // namespace

TEST_CASE("Face moves (round 10): a face a direct edit pushed is followed by the edit's own move",
          "[naming][evidence][facemoves]")
{
    // The top face of an imported box (no feature places it) pushed out by 3 (offset_face): picked before the push, it
    // is followed by the push's own move (NamingInput::faceMoves), Exact. Without the move given, or with face moves
    // switched off (the mutation check), only the guess is left (the face moved along its normal): a suggestion.
    const kernel::Shape box = take(kernel::makeBox(Vec3{0, 0, 0}, Vec3{10, 20, 5}));
    const Body before{box, nameImported(kImport, box)};
    const auto top = faceAt(box, Vec3{5, 10, 5}, Vec3{0, 0, 1});
    REQUIRE(top);
    const TopoRef ref = resolverFor(before)->makeRef(kBody, *top);
    REQUIRE(ref.samples.version == kPickSamplesVersion);
    REQUIRE(ref.samples.carrierNames.size() == ref.samples.carriers.size());
    CHECK(ref.samples.carriers[0].moved == -1); // nothing had moved it at pick time

    const auto move = offsetFaceMove(box, *top, 3.0);
    REQUIRE(move);
    CHECK(move->move.m[11] == 3.0); // along the outward normal, +z
    const Body pushed = pushedImport(3.0, true);
    const auto pushedTop = faceAt(pushed.shape, Vec3{5, 10, 8}, Vec3{0, 0, 1});
    REQUIRE(pushedTop);
    Resolved r = resolverFor(pushed)->resolve(ref);
    CHECK(r.state == ResolveState::Exact);
    CHECK(r.elements == std::vector<SubId>{*pushedTop});
    // Picked after the push, the push then edited (3 -> 5): followed from the one to the other (the pick records the
    // move that had carried it, PickSamples::faceMoves).
    const TopoRef afterPush = resolverFor(pushed)->makeRef(kBody, *pushedTop);
    REQUIRE(afterPush.samples.carriers[0].moved >= 0);
    const std::string text = toJson(afterPush);
    CHECK(toJson(take(topoRefFromJson(text))) == text);
    const Body further = pushedImport(5.0, true);
    const auto furtherTop = faceAt(further.shape, Vec3{5, 10, 10}, Vec3{0, 0, 1});
    REQUIRE(furtherTop);
    r = resolverFor(further)->resolve(afterPush);
    CHECK(r.state == ResolveState::Exact);
    CHECK(r.elements == std::vector<SubId>{*furtherTop});
    // Without the move given, and the mutation check (face moves off): a suggestion, never Exact.
    CHECK(resolverFor(pushedImport(3.0, false))->resolve(ref).state != ResolveState::Exact);
    naming::testing::setFaceMovesEnabled(false);
    const Resolved off = resolverFor(pushed)->resolve(ref);
    naming::testing::setFaceMovesEnabled(true);
    CHECK(off.state != ResolveState::Exact);
    // A version-2 reference (written before round 10) still loads, as version 2, and resolves as before: its face
    // moves are not known.
    TopoRef v2 = ref;
    v2.samples.version = 2;
    const std::string v2text = toJson(v2);
    CHECK(v2text.find("carrierNames") == std::string::npos);
    const TopoRef back = take(topoRefFromJson(v2text));
    CHECK(back.samples.version == 2);
    CHECK(back.samples.carrierNames.empty());
    CHECK(resolverFor(pushed)->resolve(back).state != ResolveState::Exact);
}

TEST_CASE("Derived identity (round 10): the one edge between the faces it is named from, each still its own",
          "[naming][evidence][identity]")
{
    // An imported box 0..40 x 0..20 x 0..10 with a boss joined on its top (sketch on z 10: (5, 5), (35, 5), (35, 15),
    // (5, 15); line 1 the front). The edge where the boss's front side meets the box's top is named from those two
    // faces (nb.edge), and was the only edge between them: PickSamples::between. Not a whole generator element (the top
    // is an imported face). The boss's front line turned (its second point to (35, 7)): the placement (the centre of
    // the profile's box) did not change, so the samples stay where they were; the turned edge holds none of them (it
    // shares its first point only): Broken by its samples. By derived identity: the boss side is a whole generator
    // element, the top holds the pick (the old edge's samples lie on it), and one edge lies between them: Exact. The
    // mutation check: derived identity off, not Exact.
    const auto build = [](double y2) {
        const kernel::Shape box = take(kernel::makeBox(Vec3{0, 0, 0}, Vec3{40, 20, 10}));
        const Body base{box, nameImported(kImport, box)};
        JobContext ctx;
        const kernel::Plane plane{Vec3{0, 0, 10}, Vec3{0, 0, 1}, Vec3{1, 0, 0}};
        const std::vector<kernel::ProfileLoop> loops{
            kernel::ProfileLoop{{line({5, 5}, {35, y2}, 1), line({35, y2}, {35, 15}, 2), line({35, 15}, {5, 15}, 3),
                                 line({5, 15}, {5, 5}, 4)},
                                1}};
        auto sweep = take(kernel::extrude(kernel::Profile{plane, loops}, 5, 0.0, ctx));
        NamingInput toolIn{kBoss, "extrude", {}, nullptr, &sweep.op.shape, sweep.origins};
        toolIn.placement = sweepPlacement(plane, std::span<const kernel::ProfileLoop>(loops));
        const Body tool{sweep.op.shape, take(propagate(toolIn)).map};
        auto joined = take(kernel::boolean(kernel::BoolOp::Union, base.shape, tool.shape, ctx));
        const std::array<const ElementMap*, 2> maps{&base.map, &tool.map};
        const std::array<const kernel::Shape*, 2> shapes{&base.shape, &tool.shape};
        NamingInput in{kBoss, "extrude", maps, &joined.history, &joined.shape, {}, shapes};
        in.placement = toolIn.placement;
        return Body{joined.shape, take(propagate(in)).map};
    };
    const Body before = build(5);
    const auto edge = lineEdge(before.shape, Vec3{5, 5, 10}, Vec3{35, 5, 10});
    REQUIRE(edge);
    const TopoRef ref = resolverFor(before)->makeRef(kBody, *edge);
    REQUIRE(ref.names.size() == 1);
    const TopoName* name = before.map.record(ref.names[0]);
    REQUIRE(name != nullptr);
    CHECK(name->role == "nb.edge");
    CHECK_FALSE(ref.samples.whole);
    CHECK(ref.samples.between);
    CHECK(toJson(ref).find("\"between\":true") != std::string::npos);
    CHECK(take(topoRefFromJson(toJson(ref))).samples.between);

    const Body after = build(7);
    const auto turned = lineEdge(after.shape, Vec3{5, 5, 10}, Vec3{35, 7, 10});
    REQUIRE(turned);
    const Resolved r = resolverFor(after)->resolve(ref);
    CHECK(r.state == ResolveState::Exact);
    CHECK(r.identity);
    CHECK(r.derived);
    CHECK(r.elements == std::vector<SubId>{*turned});
    naming::testing::setDerivedIdentityEnabled(false);
    const Resolved off = resolverFor(after)->resolve(ref);
    naming::testing::setDerivedIdentityEnabled(true);
    CHECK(off.state != ResolveState::Exact);
    // Its name's faces must still be theirs: the same reference, the edge's name forged to say it was not alone
    // between them, is not Exact.
    TopoRef notAlone = ref;
    notAlone.samples.between = false;
    CHECK(resolverFor(after)->resolve(notAlone).state != ResolveState::Exact);
}
