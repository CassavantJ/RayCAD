// Pick-time evidence in TopoRef (naming.h Territory, PickNeighbour), its JSON form, and Resolved::usable() /
// postableWithoutReview(), on small parts built with the kernel's extrude and boolean.
#include <catch2/catch_test_macros.hpp>

#include "naming_test_util.h"

#include <territory.h>

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
