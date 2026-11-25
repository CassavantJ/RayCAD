// rematchImported (task T-084): which names a new revision of an imported body keeps, and what a reference to each old
// element resolves to afterwards. Shapes are built with the kernel; the revision pairs judged by the independent oracle
// are in tests/naming/oracle/rematch_pairs_test.cpp.
#include "naming_test_util.h"

#include "rematch.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>

namespace {

using namespace ray;
using namespace ray::naming;
using kernel::SubId;
using kernel::SubKind;
using naming::test::take;

const FeatureId kImport = naming::test::featureId(7);
const BodyId kBody = naming::test::bodyId(3);

kernel::Shape box(Vec3 a, Vec3 b)
{
    return take(kernel::makeBox(a, b));
}
kernel::Shape cut(const kernel::Shape& s, const kernel::Shape& tool)
{
    JobContext ctx;
    return take(kernel::boolean(kernel::BoolOp::Cut, s, tool, ctx)).shape;
}
kernel::Shape hole(const kernel::Shape& s, double x, double y, double r)
{
    return cut(s, take(kernel::makeCylinder({x, y, -1}, {0, 0, 1}, r, 30)));
}
kernel::Shape moved(const kernel::Shape& s, double dx, double dy, double dz)
{
    Xform x = Xform::identity();
    x.m[3] = dx;
    x.m[7] = dy;
    x.m[11] = dz;
    return take(kernel::transformed(s, x)).shape;
}

// A face of `s` by type and centroid.
SubId faceAt(const kernel::Shape& s, kernel::SurfaceType type, Vec3 c, double tol = 1e-6)
{
    for (std::uint32_t i = 0; i < s.count(SubKind::Face); ++i) {
        const auto info = kernel::surfaceInfo(s, {SubKind::Face, i});
        if (info && info->type == type && std::abs(info->centroid.x - c.x) < tol &&
            std::abs(info->centroid.y - c.y) < tol && std::abs(info->centroid.z - c.z) < tol)
            return {SubKind::Face, i};
    }
    FAIL("no face of that type at that centroid");
    return {};
}
// The face of `s` on the plane z = height facing `up`.
SubId planeZ(const kernel::Shape& s, double height)
{
    for (std::uint32_t i = 0; i < s.count(SubKind::Face); ++i) {
        const auto info = kernel::surfaceInfo(s, {SubKind::Face, i});
        if (info && info->type == kernel::SurfaceType::Plane && std::abs(info->bounds.min.z - height) < 1e-6 &&
            std::abs(info->bounds.max.z - height) < 1e-6)
            return {SubKind::Face, i};
    }
    FAIL("no plane at that height");
    return {};
}
SubId cylinderAt(const kernel::Shape& s, double x, double y)
{
    for (std::uint32_t i = 0; i < s.count(SubKind::Face); ++i) {
        const auto info = kernel::surfaceInfo(s, {SubKind::Face, i});
        if (info && info->type == kernel::SurfaceType::Cylinder && std::abs(info->centroid.x - x) < 1e-6 &&
            std::abs(info->centroid.y - y) < 1e-6)
            return {SubKind::Face, i};
    }
    FAIL("no cylinder there");
    return {};
}

struct Replaced {
    kernel::Shape before, after;
    ElementMap old;
    RematchResult result;
    std::unique_ptr<IResolver> oldResolver, newResolver;

    Replaced(kernel::Shape a, kernel::Shape b, bool withShape = true) : before(std::move(a)), after(std::move(b))
    {
        old = nameImported(kImport, before);
        REQUIRE(old.size() > 0);
        result = withShape ? rematchImported(old, before, kImport, after) : rematchImported(old, kImport, after);
        REQUIRE(result.map.size() ==
                after.count(SubKind::Face) + after.count(SubKind::Edge) + after.count(SubKind::Vertex));
        oldResolver = makeResolver({ResolverBody{kBody, old, before}});
        newResolver = makeResolver({ResolverBody{kBody, result.map, after}});
    }
    Resolved resolve(SubId pick) const { return newResolver->resolve(oldResolver->makeRef(kBody, pick)); }
    bool unchanged(SubId pick, SubId expected) const
    {
        const Resolved r = resolve(pick);
        return r.state == ResolveState::Exact && r.elements == std::vector<SubId>{expected};
    }
    // Every old name, for checking that new names never reuse one.
    std::set<Hash256> oldNames() const
    {
        std::set<Hash256> out;
        for (const SubId s : naming::test::allElements(before))
            for (const Hash256& n : old.namesOf(s))
                out.insert(n);
        return out;
    }
};

kernel::Shape plate()
{
    kernel::Shape s = box({0, 0, 0}, {80, 50, 10});
    s = hole(s, 15, 25, 3);
    s = hole(s, 40, 25, 4);
    return s;
}

} // namespace

TEST_CASE("rematch: the same geometry keeps every name; a part moved by noise below the tolerance too", "[rematch]")
{
    const kernel::Shape a = plate();
    for (const kernel::Shape& b : {a, moved(a, 4e-4, -3e-4, 2e-4)}) {
        const Replaced r(a, b);
        CHECK(r.result.unmatched.empty());
        CHECK(r.result.lineage.empty());
        for (const SubId s : naming::test::allElements(a)) {
            INFO("element " << static_cast<int>(s.kind) << ":" << s.index);
            const Resolved got = r.resolve(s);
            CHECK(got.state == ResolveState::Exact);
            CHECK(got.elements == std::vector<SubId>{s});
        }
    }
}

TEST_CASE("rematch: a part moved beyond the tolerance is not matched (no transform is assumed)", "[rematch]")
{
    const kernel::Shape a = plate();
    const Replaced r(a, moved(a, 0.01, 0, 0));
    for (const SubId s : naming::test::allElements(a)) {
        const Resolved got = r.resolve(s);
        CHECK(got.state != ResolveState::Exact);
        CHECK_FALSE((got.usable() && got.state == ResolveState::Repaired));
    }
}

TEST_CASE("rematch: matching does not depend on the order of faces", "[rematch]")
{
    // The same plate built by cutting the holes the other way round.
    kernel::Shape b = box({0, 0, 0}, {80, 50, 10});
    b = hole(b, 40, 25, 4);
    b = hole(b, 15, 25, 3);
    const kernel::Shape a = plate();
    const Replaced r(a, b);
    for (const SubId s : naming::test::allElements(a)) {
        const Resolved got = r.resolve(s);
        REQUIRE(got.state == ResolveState::Exact);
        REQUIRE(got.elements.size() == 1);
        CHECK(naming::test::sameGeometry(naming::test::geometryOf(a, s), naming::test::geometryOf(b, got.elements[0]),
                                         1e-6));
    }
}

TEST_CASE("rematch: each kind of revision", "[rematch]")
{
    const kernel::Shape a = plate();
    const SubId small = cylinderAt(a, 15, 25), big = cylinderAt(a, 40, 25), top = planeZ(a, 10), bottom = planeZ(a, 0);
    const SubId front = faceAt(a, kernel::SurfaceType::Plane, {40, 0, 5});

    SECTION("a hole added: its wall is new, the faces it runs through changed, the rest unchanged")
    {
        const kernel::Shape b = hole(a, 65, 25, 2);
        const Replaced r(a, b);
        CHECK(r.unchanged(small, cylinderAt(b, 15, 25)));
        CHECK(r.unchanged(big, cylinderAt(b, 40, 25)));
        CHECK(r.unchanged(front, faceAt(b, kernel::SurfaceType::Plane, {40, 0, 5})));
        for (const SubId f : {top, bottom}) {
            const Resolved got = r.resolve(f);
            CHECK(got.state == ResolveState::Merged); // review: not Exact
            CHECK(got.elements == std::vector<SubId>{planeZ(b, f == top ? 10 : 0)});
            CHECK_FALSE(got.postableWithoutReview());
        }
        // The new wall holds no old name.
        const std::set<Hash256> old = r.oldNames();
        for (const Hash256& n : r.result.map.namesOf(cylinderAt(b, 65, 25)))
            CHECK(old.count(n) == 0);
    }
    SECTION("a hole removed: references to its wall are broken")
    {
        kernel::Shape b = box({0, 0, 0}, {80, 50, 10});
        b = hole(b, 15, 25, 3);
        const Replaced r(a, b);
        CHECK(r.resolve(big).state == ResolveState::Broken);
        CHECK(r.unchanged(small, cylinderAt(b, 15, 25)));
        CHECK(r.resolve(top).state == ResolveState::Merged);
        bool deleted = false;
        for (const LineageEvent& e : r.result.lineage)
            deleted = deleted || (e.kind == LineageEvent::Kind::Deleted && e.from == r.old.namesOf(big).front());
        CHECK(deleted);
    }
    SECTION("a hole moved or resized: the old wall is not matched to the new one")
    {
        for (const auto& [x, radius] : {std::pair{42.0, 4.0}, std::pair{40.0, 4.5}}) {
            kernel::Shape b = box({0, 0, 0}, {80, 50, 10});
            b = hole(b, 15, 25, 3);
            b = hole(b, x, 25, radius);
            const Replaced r(a, b);
            const Resolved got = r.resolve(big);
            CHECK(got.state == ResolveState::Broken);
            CHECK(r.unchanged(small, cylinderAt(b, 15, 25)));
        }
    }
    SECTION("the part made longer: the end face is new, the faces along it changed")
    {
        kernel::Shape b = box({0, 0, 0}, {90, 50, 10});
        b = hole(b, 15, 25, 3);
        b = hole(b, 40, 25, 4);
        const Replaced r(a, b);
        const SubId end = faceAt(a, kernel::SurfaceType::Plane, {80, 25, 5});
        CHECK(r.resolve(end).state == ResolveState::Broken);
        const Resolved f = r.resolve(front);
        CHECK(f.state == ResolveState::Merged);
        CHECK(f.elements == std::vector<SubId>{faceAt(b, kernel::SurfaceType::Plane, {45, 0, 5})});
        CHECK(r.unchanged(small, cylinderAt(b, 15, 25)));
    }
    SECTION("a slot across the top: the top face is in two pieces, named from it; a reference is never Exact")
    {
        const kernel::Shape b = cut(a, box({60, -1, 5}, {64, 51, 11}));
        const Replaced r(a, b);
        // The pieces carry "split" names made from the top face's names.
        int pieces = 0;
        for (std::uint32_t i = 0; i < b.count(SubKind::Face); ++i)
            for (const Hash256& n : r.result.map.namesOf({SubKind::Face, i}))
                if (const TopoName* t = r.result.map.record(n); t != nullptr && t->role == "split") {
                    CHECK(t->parents == std::vector<Hash256>(r.old.namesOf(top).begin(), r.old.namesOf(top).end()));
                    ++pieces;
                }
        CHECK(pieces == 2);
        // The resolver answers Split with both pieces, or Broken when the pick-time evidence objects (the pieces meet
        // the new slot's walls, faces of the same import).
        const Resolved got = r.resolve(top);
        CHECK((got.state == ResolveState::Split || got.state == ResolveState::Broken));
        if (got.state == ResolveState::Split)
            CHECK(got.elements.size() == 2);
        CHECK(r.unchanged(small, cylinderAt(b, 15, 25)));
    }
}

TEST_CASE("rematch: symmetric traps are told apart by position, never by shape alone", "[rematch]")
{
    SECTION("two holes of the same size: each keeps its own name; swapped sizes match nothing")
    {
        kernel::Shape a = box({0, 0, 0}, {80, 50, 10});
        a = hole(a, 20, 25, 3);
        a = hole(a, 60, 25, 4);
        kernel::Shape b = box({0, 0, 0}, {80, 50, 10});
        b = hole(b, 20, 25, 4);
        b = hole(b, 60, 25, 3);
        const Replaced r(a, b);
        // Local shape alone would match the 3 mm hole at x 20 to the 3 mm hole now at x 60.
        CHECK(r.resolve(cylinderAt(a, 20, 25)).state == ResolveState::Broken);
        CHECK(r.resolve(cylinderAt(a, 60, 25)).state == ResolveState::Broken);
    }
    SECTION("a square pocket turned 45 degrees: its floor has the same imported name, but is not the same face")
    {
        const kernel::Shape blank = box({-40, -40, 0}, {40, 40, 20});
        const kernel::Shape a = cut(blank, box({-10, -10, 10}, {10, 10, 21}));
        Xform turn = Xform::identity(); // 45 degrees about Z
        const double c = std::sqrt(0.5);
        turn.m[0] = c;
        turn.m[1] = -c;
        turn.m[4] = c;
        turn.m[5] = c;
        const kernel::Shape tool = take(kernel::transformed(box({-10, -10, 10}, {10, 10, 21}), turn)).shape;
        const kernel::Shape b = cut(blank, tool);
        const SubId floorA = planeZ(a, 10), floorB = planeZ(b, 10);
        // Congruent in place: the same fingerprint, hence the same name from nameImported alone.
        CHECK(nameImported(kImport, a).namesOf(floorA).front() == nameImported(kImport, b).namesOf(floorB).front());
        const Replaced r(a, b);
        const Resolved got = r.resolve(floorA);
        CHECK(got.state != ResolveState::Exact);
        // Nor without the old shape (fingerprint and neighbours).
        const Replaced f(a, b, false);
        CHECK(f.resolve(floorA).state != ResolveState::Exact);
    }
}

TEST_CASE("rematch: the map is valid, deterministic, and new names never reuse an old one", "[rematch]")
{
    const kernel::Shape a = plate();
    const kernel::Shape b = hole(cut(a, box({60, -1, 6}, {81, 51, 11})), 70, 10, 2);
    const Replaced r(a, b);
    const Replaced again(a, b);
    CHECK(r.result.map.serialize() == again.result.map.serialize());
    const auto back = ElementMap::deserialize(r.result.map.serialize());
    REQUIRE(back);
    CHECK(back->serialize() == r.result.map.serialize());
    // An old name is either on an unchanged element (alone with other old names) or carried with a new name of its
    // own; no element holds only new names that equal old ones.
    const std::set<Hash256> old = r.oldNames();
    for (const SubId s : naming::test::allElements(b)) {
        const auto names = r.result.map.namesOf(s);
        REQUIRE(!names.empty());
    }
    // unmatched lists every old name not on an unchanged element.
    CHECK(std::is_sorted(r.result.unmatched.begin(), r.result.unmatched.end()));
    for (const Hash256& n : r.result.unmatched)
        CHECK(old.count(n) == 1);
}

TEST_CASE("rematch without the old shape: fingerprints and neighbours, nothing reported changed", "[rematch]")
{
    const kernel::Shape a = plate();
    SECTION("the same geometry in another order")
    {
        kernel::Shape b = box({0, 0, 0}, {80, 50, 10});
        b = hole(b, 40, 25, 4);
        b = hole(b, 15, 25, 3);
        const Replaced r(a, b, false);
        for (const SubId s : naming::test::allElements(a))
            CHECK(r.resolve(s).state == ResolveState::Exact);
    }
    SECTION("a pocket deepened: its faces are not Exact; a face whose neighbourhood is unchanged is")
    {
        const kernel::Shape blank = box({0, 0, 0}, {80, 50, 20});
        const kernel::Shape p = cut(blank, box({30, 15, 12}, {50, 35, 21}));
        const kernel::Shape q = cut(blank, box({30, 15, 8}, {50, 35, 21}));
        const Replaced r(p, q, false);
        CHECK(r.resolve(planeZ(p, 12)).state != ResolveState::Exact);
        CHECK(r.unchanged(faceAt(p, kernel::SurfaceType::Plane, {40, 0, 10}),
                          faceAt(q, kernel::SurfaceType::Plane, {40, 0, 10})));
        for (const LineageEvent& e : r.result.lineage)
            CHECK(e.kind == LineageEvent::Kind::Deleted);
    }
    SECTION("a hole added: the faces it runs through are not Exact, the others are")
    {
        const kernel::Shape b = hole(a, 65, 25, 2);
        const Replaced r(a, b, false);
        CHECK(r.resolve(planeZ(a, 10)).state != ResolveState::Exact);
        CHECK(r.resolve(planeZ(a, 0)).state != ResolveState::Exact);
        CHECK(r.unchanged(cylinderAt(a, 15, 25), cylinderAt(b, 15, 25)));
        CHECK(r.unchanged(faceAt(a, kernel::SurfaceType::Plane, {40, 0, 5}),
                          faceAt(b, kernel::SurfaceType::Plane, {40, 0, 5})));
    }
}
