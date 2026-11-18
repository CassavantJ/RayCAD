// IResolver: Exact and Broken (task T-007). A reference never resolves to a different element.
#include <catch2/catch_test_macros.hpp>

#include "naming_test_util.h"

#include <import_naming.h>
#include <resolver.h>
#include <shape_testing.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace ray;
using namespace ray::naming;
using ray::naming::test::take;

namespace {

const FeatureId kImport = test::featureId(21);
const BodyId kBody = test::bodyId(1);

std::vector<Vec2> grid4x4()
{
    std::vector<Vec2> out;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out.push_back(Vec2{20.0 + 20.0 * i, 20.0 + 20.0 * j});
    return out;
}

kernel::Shape plate(const std::vector<Vec2>& holes)
{
    return take(kernel::testing::makePlateWithHoles(Vec3{100, 100, 10}, 4.0, holes));
}

std::unique_ptr<IResolver> resolverFor(const kernel::Shape& shape, BodyId body = kBody)
{
    return makeResolver({ResolverBody{body, nameImported(kImport, shape), shape}});
}

Fingerprint lineAt(std::int64_t x)
{
    Fingerprint f = detail::emptyFingerprint();
    f.surface = static_cast<kernel::SurfaceType>(kernel::CurveType::Line);
    f.qCx = x;
    f.qArea = 50'000;
    f.qAxisX = 1'000'000;
    f.neighbourCount = 2;
    return f;
}

// Two faces and three edges on both of them, the middle one at `middleX`: the edges are siblings with identical
// discriminators, so their names carry ordinals.
ElementMap siblingEdges(std::int64_t middleX)
{
    detail::BodyFacts facts;
    facts.faceCount = 2;
    Fingerprint a = detail::emptyFingerprint();
    a.surface = kernel::SurfaceType::Plane;
    a.qCz = 0;
    a.qAxisZ = 1'000'000;
    Fingerprint b = a;
    b.qCz = 100'000;
    facts.faces = {detail::FaceFacts{0, a, {1}}, detail::FaceFacts{1, b, {0}}};
    facts.edgeCount = 3;
    const std::array<std::int64_t, 3> xs{0, middleX, 100'000};
    for (std::uint32_t i = 0; i < 3; ++i) {
        detail::EdgeFacts e;
        e.index = i;
        e.fingerprint = lineAt(xs[i]);
        e.faces = {0, 1};
        facts.edges.push_back(e);
    }
    return take(detail::assignNames(kImport, facts));
}

} // namespace

TEST_CASE("Every element of an unchanged body resolves Exact to itself", "[naming][resolver]")
{
    const std::vector<Vec2> holes = grid4x4();
    for (const kernel::Shape& shape : {plate(holes), take(kernel::makeBox(Vec3{0, 0, 0}, Vec3{10, 10, 10})),
                                       take(kernel::testing::makeSphere(Vec3{0, 0, 0}, 5.0))}) {
        const ElementMap map = nameImported(kImport, shape);
        const auto resolver = makeResolver({ResolverBody{kBody, map, shape}});
        for (const kernel::SubId sub : test::allElements(shape)) {
            const TopoRef ref = resolver->makeRef(kBody, sub);
            REQUIRE(ref.names.size() == 1);
            CHECK(ref.body == kBody);
            CHECK(ref.kind == sub.kind);
            CHECK(ref.families.size() == 1);
            CHECK(ref.ambiguous.size() == 1);
            CHECK(ref.families[0] == map.record(ref.names[0])->family);
            CHECK(ref.ambiguous[0] == map.record(ref.names[0])->ambiguous());
            CHECK(ref.splitParents.empty());
            CHECK(ref.hint == map.fingerprintOf(sub));
            CHECK_FALSE(ref.repairConfirmed);
            CHECK(ref.neighbourNames.size() == kernel::adjacent(shape, sub, kernel::SubKind::Face).size());
            const Resolved r = resolver->resolve(ref);
            CHECK(r.state == ResolveState::Exact);
            CHECK(r.usable());
            REQUIRE(r.elements.size() == 1);
            CHECK(r.elements[0] == sub);
        }
    }
}

TEST_CASE("References survive a change of the kernel's sub-shape order", "[naming][resolver][order]")
{
    const kernel::Shape before = plate(grid4x4());
    const kernel::Shape after = take(kernel::testing::reorderFaces(before, 17));
    const auto pick = resolverFor(before);
    const auto now = resolverFor(after);
    for (const kernel::SubId sub : test::allElements(before)) {
        const Resolved r = now->resolve(pick->makeRef(kBody, sub));
        REQUIRE(r.state == ResolveState::Exact);
        REQUIRE(r.elements.size() == 1);
        CHECK(test::geometryOf(after, r.elements[0]) == test::geometryOf(before, sub));
    }
}

TEST_CASE("A reference to a missing name resolves Broken and never to another element", "[naming][resolver]")
{
    const std::vector<Vec2> holes = grid4x4();
    const kernel::Shape before = plate(holes);
    std::vector<Vec2> moved = holes;
    moved[5].x += 5.0; // one hole moves
    moved.pop_back();  // another one is gone
    const kernel::Shape after = plate(moved);

    const auto pick = resolverFor(before);
    const auto now = resolverFor(after);
    std::size_t exact = 0, broken = 0;
    for (const kernel::SubId sub : test::allElements(before)) {
        Resolved r{ResolveState::Broken, {}, {}};
        CHECK_NOTHROW(r = now->resolve(pick->makeRef(kBody, sub)));
        if (r.state == ResolveState::Exact) {
            ++exact;
            REQUIRE(r.elements.size() == 1);
            // The oracle is the kernel's own geometry, not the naming code: the element found must be the
            // element that was picked.
            INFO("element " << static_cast<int>(sub.kind) << ":" << sub.index);
            CHECK(test::sameGeometry(test::geometryOf(after, r.elements[0]), test::geometryOf(before, sub), 1e-9));
        } else {
            CHECK(r.state == ResolveState::Broken);
            CHECK(r.elements.empty());
            CHECK_FALSE(r.usable());
            CHECK_FALSE(r.reason.empty());
            ++broken;
        }
    }
    // The walls (and seams) of the 14 untouched holes and the four sides keep their names; everything on the
    // top and bottom faces, whose centroids moved, is lost.
    CHECK(exact >= 14 * 2 + 4);
    CHECK(broken > 0);
    std::printf("[naming] hole moved and hole removed: %zu Exact, %zu Broken of %zu references\n", exact, broken,
                exact + broken);

    // The moved hole's wall in particular.
    for (std::uint32_t i = 0; i < before.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId face{kernel::SubKind::Face, i};
        const kernel::SurfaceInfo info = take(kernel::surfaceInfo(before, face));
        if (info.type == kernel::SurfaceType::Cylinder && std::abs(info.origin.x - holes[5].x) < 1e-6 &&
            std::abs(info.origin.y - holes[5].y) < 1e-6) {
            const Resolved r = now->resolve(pick->makeRef(kBody, face));
            CHECK(r.state == ResolveState::Broken);
        }
    }
}

TEST_CASE("An ambiguous name is trusted only while its fingerprint matches", "[naming][resolver][ambiguous]")
{
    SECTION("sibling edges told apart only by ordinal")
    {
        const ElementMap atPick = siblingEdges(50'000);
        const auto pick = makeResolver({ResolverBody{kBody, atPick, kernel::Shape{}}});
        const TopoRef ref = pick->makeRef(kBody, kernel::SubId{kernel::SubKind::Edge, 1});
        REQUIRE(ref.names.size() == 1);
        REQUIRE(ref.ambiguous[0]);

        // The middle edge moves by 0.5 um (inside the 1e-3 mm tolerance): same ordinal, same name, Exact.
        const auto small = makeResolver({ResolverBody{kBody, siblingEdges(50'005), kernel::Shape{}}});
        Resolved r = small->resolve(ref);
        CHECK(r.state == ResolveState::Exact);
        CHECK(r.elements == std::vector<kernel::SubId>{kernel::SubId{kernel::SubKind::Edge, 1}});

        // It moves by 5 um: the name still exists (same ordinal), but it is no longer trusted.
        const ElementMap later = siblingEdges(50'050);
        REQUIRE(later.find(ref.names[0]).has_value());
        const auto moved = makeResolver({ResolverBody{kBody, later, kernel::Shape{}}});
        r = moved->resolve(ref);
        CHECK(r.state == ResolveState::Broken);
        CHECK(r.elements.empty());
        CHECK_FALSE(r.usable());
        CHECK_FALSE(r.reason.empty());

        // It moves past its neighbour: the ordinals swap. The old name now names the other edge, whose
        // fingerprint does not match: Broken, not the wrong edge.
        const ElementMap swapped = siblingEdges(150'000);
        const auto other = makeResolver({ResolverBody{kBody, swapped, kernel::Shape{}}});
        r = other->resolve(ref);
        CHECK(r.state == ResolveState::Broken);
    }
    SECTION("a sphere's pole vertex")
    {
        const kernel::Shape shape = take(kernel::testing::makeSphere(Vec3{0, 0, 0}, 5.0));
        const auto resolver = resolverFor(shape);
        TopoRef ref = resolver->makeRef(kBody, kernel::SubId{kernel::SubKind::Vertex, 0});
        REQUIRE(ref.ambiguous.size() == 1);
        REQUIRE(ref.ambiguous[0]);
        CHECK(resolver->resolve(ref).state == ResolveState::Exact);
        ref.hint.qCz += 5; // 0.5 um: tolerance
        CHECK(resolver->resolve(ref).state == ResolveState::Exact);
        ref.hint.qCz += 50; // 5.5 um
        CHECK(resolver->resolve(ref).state == ResolveState::Broken);
        // The flag stored in the reference counts too, even if the record says otherwise.
        ref.ambiguous[0] = false;
        CHECK(resolver->resolve(ref).state == ResolveState::Broken);
    }
    SECTION("an unambiguous name does not depend on the hint")
    {
        const kernel::Shape box = take(kernel::makeBox(Vec3{0, 0, 0}, Vec3{10, 10, 10}));
        const auto resolver = resolverFor(box);
        TopoRef ref = resolver->makeRef(kBody, kernel::SubId{kernel::SubKind::Face, 0});
        REQUIRE_FALSE(ref.ambiguous[0]);
        ref.hint.qCx += 1'000'000;
        CHECK(resolver->resolve(ref).state == ResolveState::Exact);
    }
}

TEST_CASE("Malformed and unresolvable references are Broken and never exceptions", "[naming][resolver]")
{
    const kernel::Shape box = take(kernel::makeBox(Vec3{0, 0, 0}, Vec3{10, 10, 10}));
    const auto resolver = resolverFor(box);
    const TopoRef good = resolver->makeRef(kBody, kernel::SubId{kernel::SubKind::Face, 2});
    REQUIRE(resolver->resolve(good).state == ResolveState::Exact);

    auto expectBroken = [&](const TopoRef& ref) {
        Resolved r{ResolveState::Broken, {}, {}};
        CHECK_NOTHROW(r = resolver->resolve(ref));
        CHECK(r.state == ResolveState::Broken);
        CHECK(r.elements.empty());
        CHECK_FALSE(r.usable());
        CHECK_FALSE(r.reason.empty());
    };

    TopoRef ref = good;
    ref.body = test::bodyId(99);
    expectBroken(ref); // unknown body

    expectBroken(TopoRef{}); // nothing at all

    ref = good;
    ref.names.clear();
    expectBroken(ref);

    ref = good;
    ref.families.push_back(Hash256{});
    expectBroken(ref); // parallel vectors disagree

    ref = good;
    ref.kind = kernel::SubKind::Edge;
    expectBroken(ref); // the name is a face's

    ref = good;
    ref.names.push_back(Hash256{});
    ref.families.push_back(Hash256{});
    ref.ambiguous.push_back(false);
    expectBroken(ref); // part of the picked element is gone

    ref = good;
    const TopoRef otherFace = resolver->makeRef(kBody, kernel::SubId{kernel::SubKind::Face, 3});
    ref.names.push_back(otherFace.names[0]);
    ref.families.push_back(otherFace.families[0]);
    ref.ambiguous.push_back(false);
    expectBroken(ref); // its names now lead to two elements

    // makeRef on something that is not there gives a reference that resolves Broken.
    TopoRef none{};
    CHECK_NOTHROW(none = resolver->makeRef(kBody, kernel::SubId{kernel::SubKind::Face, 999}));
    CHECK(none.names.empty());
    expectBroken(none);
    CHECK_NOTHROW(none = resolver->makeRef(kBody, kernel::SubId{kernel::SubKind::Solid, 0}));
    expectBroken(none);
    CHECK_NOTHROW(none = resolver->makeRef(test::bodyId(42), kernel::SubId{kernel::SubKind::Face, 0}));
    expectBroken(none);

    // A body whose map is empty (nameImported failed) resolves nothing.
    const auto empty = makeResolver({ResolverBody{kBody, ElementMap{}, kernel::Shape{}}});
    CHECK(empty->resolve(good).state == ResolveState::Broken);
}

TEST_CASE("Elements that cannot be told apart are never resolved", "[naming][resolver][ambiguous]")
{
    detail::BodyFacts facts;
    facts.faceCount = 3;
    Fingerprint same = detail::emptyFingerprint();
    same.surface = kernel::SurfaceType::Plane;
    same.qAxisZ = 1'000'000;
    Fingerprint other = same;
    other.qCx = 70'000;
    facts.faces = {detail::FaceFacts{0, same, {2}}, detail::FaceFacts{1, same, {2}},
                   detail::FaceFacts{2, other, {0, 1}}};
    const ElementMap map = take(detail::assignNames(kImport, facts));
    const auto resolver = makeResolver({ResolverBody{kBody, map, kernel::Shape{}}});
    const TopoRef ref = resolver->makeRef(kBody, kernel::SubId{kernel::SubKind::Face, 0});
    REQUIRE(ref.names.size() == 1);
    const Resolved r = resolver->resolve(ref);
    CHECK(r.state == ResolveState::Broken);
    CHECK(r.elements.empty());
}

TEST_CASE("Two bodies: a reference resolves only in its own body", "[naming][resolver]")
{
    const kernel::Shape a = take(kernel::makeBox(Vec3{0, 0, 0}, Vec3{10, 10, 10}));
    const kernel::Shape b = take(kernel::makeBox(Vec3{0, 0, 0}, Vec3{10, 10, 10}));
    const BodyId idA = test::bodyId(1);
    const BodyId idB = test::bodyId(2);
    // Same import feature id on purpose: the names are identical, the body decides.
    const auto resolver =
        makeResolver({ResolverBody{idB, nameImported(kImport, b), b}, ResolverBody{idA, nameImported(kImport, a), a}});
    const TopoRef ref = resolver->makeRef(idA, kernel::SubId{kernel::SubKind::Edge, 4});
    TopoRef moved = ref;
    moved.body = idB;
    CHECK(resolver->resolve(ref).state == ResolveState::Exact);
    CHECK(resolver->resolve(moved).state == ResolveState::Exact); // the same name in body B
    CHECK(resolver->resolve(moved).elements == resolver->resolve(ref).elements);
    moved.body = test::bodyId(3);
    CHECK(resolver->resolve(moved).state == ResolveState::Broken);
}

TEST_CASE("Runtime of makeRef and resolve on a body with a few thousand faces", "[naming][resolver][perf]")
{
    using Clock = std::chrono::steady_clock;
    const kernel::Shape star = take(kernel::testing::makeStarPrism(1500, 200.0, 180.0, 25.0));
    const auto resolver = resolverFor(star);
    const auto elements = test::allElements(star);
    std::vector<TopoRef> refs;
    refs.reserve(elements.size());
    const auto t0 = Clock::now();
    for (const kernel::SubId sub : elements)
        refs.push_back(resolver->makeRef(kBody, sub));
    const auto t1 = Clock::now();
    std::size_t exact = 0;
    for (const TopoRef& ref : refs)
        exact += resolver->resolve(ref).state == ResolveState::Exact ? 1 : 0;
    const auto t2 = Clock::now();
    CHECK(exact == elements.size());
    auto ms = [](Clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };
    std::printf("[naming perf] %zu references: makeRef %.1f ms, resolve %.1f ms\n", elements.size(), ms(t1 - t0),
                ms(t2 - t1));
}
