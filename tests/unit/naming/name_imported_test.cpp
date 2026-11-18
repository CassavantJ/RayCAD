// nameImported: names for history-free bodies (task T-007). Determinism, independence from sub-shape order,
// symmetric parts, the sibling rules, and the runtime on a body with a few thousand faces.
#include <catch2/catch_test_macros.hpp>

#include "naming_test_util.h"

#include <import_naming.h>
#include <shape_testing.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>
#include <set>
#include <vector>

using namespace ray;
using namespace ray::naming;
using ray::naming::test::take;

namespace {

std::vector<Vec2> grid(int n, double pitch, double offset)
{
    std::vector<Vec2> out;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            out.push_back(Vec2{offset + pitch * i, offset + pitch * j});
    return out;
}

kernel::Shape holePlate4x4()
{
    const std::vector<Vec2> holes = grid(4, 20.0, 20.0);
    return take(kernel::testing::makePlateWithHoles(Vec3{100, 100, 10}, 4.0, holes));
}

kernel::Shape cube()
{
    return take(kernel::makeBox(Vec3{0, 0, 0}, Vec3{10, 10, 10}));
}

kernel::Shape sphere()
{
    return take(kernel::testing::makeSphere(Vec3{1, 2, 3}, 10.0));
}

// Visiting orders for the test hook.
detail::VisitOrder reversedOrder()
{
    return [](kernel::SubKind, std::uint32_t count) {
        std::vector<std::uint32_t> v(count);
        std::iota(v.begin(), v.end(), std::uint32_t{0});
        std::reverse(v.begin(), v.end());
        return v;
    };
}
detail::VisitOrder shuffledOrder(std::uint32_t seed)
{
    return [seed](kernel::SubKind kind, std::uint32_t count) {
        std::vector<std::uint32_t> v(count);
        std::iota(v.begin(), v.end(), std::uint32_t{0});
        std::mt19937 random(seed + static_cast<std::uint32_t>(kind));
        std::shuffle(v.begin(), v.end(), random);
        return v;
    };
}

struct Census {
    std::size_t elements = 0, ambiguous = 0, shared = 0;
};

// Every element has a name; primary names are distinct unless the element is ambiguous and shares its name.
Census census(const kernel::Shape& shape, const ElementMap& map)
{
    Census c;
    std::set<Hash256> seen;
    for (const kernel::SubId sub : test::allElements(shape)) {
        ++c.elements;
        const auto names = map.namesOf(sub);
        REQUIRE(names.size() == 1);
        const TopoName* record = map.record(names[0]);
        REQUIRE(record != nullptr);
        if (record->ambiguous())
            ++c.ambiguous;
        if (!seen.insert(names[0]).second) {
            ++c.shared;
            CHECK(record->ambiguous()); // only an ambiguous name can be shared
        }
        if (map.find(names[0]).has_value())
            CHECK(*map.find(names[0]) == sub);
    }
    return c;
}

// Facts for the synthetic tests: faces only (no edges, no vertices) unless added.
detail::BodyFacts faceFacts(const std::vector<Fingerprint>& fingerprints,
                            const std::vector<std::vector<std::uint32_t>>& neighbours)
{
    detail::BodyFacts facts;
    facts.faceCount = static_cast<std::uint32_t>(fingerprints.size());
    for (std::uint32_t i = 0; i < facts.faceCount; ++i)
        facts.faces.push_back(detail::FaceFacts{i, fingerprints[i], neighbours[i]});
    return facts;
}

Fingerprint planeAt(std::int64_t x, std::int64_t area = 1'000'000)
{
    Fingerprint f = detail::emptyFingerprint();
    f.surface = kernel::SurfaceType::Plane;
    f.qCx = x;
    f.qArea = area;
    f.qAxisZ = 1'000'000;
    f.neighbourCount = 1;
    return f;
}

} // namespace

TEST_CASE("Naming the same body twice gives identical element-map bytes", "[naming][import]")
{
    const FeatureId feature = test::featureId(7);
    for (const kernel::Shape& shape : {holePlate4x4(), cube(), sphere()}) {
        const auto first = nameImported(feature, shape).serialize();
        const auto second = nameImported(feature, shape).serialize();
        CHECK(first == second);
        // A copy of the shape made through the kernel's serialisation names identically (SubIds survive).
        const kernel::Shape again = take(kernel::deserialize(kernel::serialize(shape)));
        CHECK(nameImported(feature, again).serialize() == first);
    }
    // The feature is part of every name.
    const kernel::Shape box = cube();
    CHECK(nameImported(test::featureId(7), box).serialize() != nameImported(test::featureId(8), box).serialize());
}

TEST_CASE("Names do not depend on the order in which elements are visited", "[naming][import][order]")
{
    const FeatureId feature = test::featureId(3);
    for (const kernel::Shape& shape : {holePlate4x4(), cube(), sphere()}) {
        const auto canonical = nameImported(feature, shape).serialize();
        CHECK(detail::nameImported(feature, shape, reversedOrder()).serialize() == canonical);
        for (std::uint32_t seed = 1; seed <= 5; ++seed)
            CHECK(detail::nameImported(feature, shape, shuffledOrder(seed)).serialize() == canonical);
    }
}

TEST_CASE("Names do not depend on the kernel's sub-shape order", "[naming][import][order]")
{
    const FeatureId feature = test::featureId(4);
    for (const kernel::Shape& shape : {holePlate4x4(), cube(), sphere()}) {
        const ElementMap map = nameImported(feature, shape);
        const auto expected = test::nameGeometry(shape, map);
        bool someOrderDiffers = false;
        for (std::uint64_t seed = 1; seed <= 4; ++seed) {
            const kernel::Shape shuffled = take(kernel::testing::reorderFaces(shape, seed));
            REQUIRE(shuffled.count(kernel::SubKind::Face) == shape.count(kernel::SubKind::Face));
            for (const kernel::SubId sub : test::allElements(shape))
                if (test::geometryOf(shape, sub) != test::geometryOf(shuffled, sub))
                    someOrderDiffers = true;
            const ElementMap other = nameImported(feature, shuffled);
            // Same names, each on the element with the same geometry.
            CHECK(test::nameGeometry(shuffled, other) == expected);
        }
        if (shape.count(kernel::SubKind::Face) > 1)
            CHECK(someOrderDiffers); // the hook really changed the SubId order
    }
}

TEST_CASE("Symmetric parts: every element gets a distinct name or is flagged ambiguous", "[naming][import][symmetry]")
{
    const FeatureId feature = test::featureId(5);

    SECTION("a cube")
    {
        const kernel::Shape shape = cube();
        const Census c = census(shape, nameImported(feature, shape));
        CHECK(c.elements == 8 + 12 + 6);
        CHECK(c.ambiguous == 0); // every face has its own centroid, every edge and vertex its own faces
        CHECK(c.shared == 0);
    }
    SECTION("a plate with a 4 x 4 grid of identical holes")
    {
        const kernel::Shape shape = holePlate4x4();
        CHECK(shape.count(kernel::SubKind::Face) == 6 + 16);
        const ElementMap map = nameImported(feature, shape);
        const Census c = census(shape, map);
        CHECK(c.ambiguous == 0);
        CHECK(c.shared == 0);
        // The 16 hole walls are congruent but sit in different places: 16 names, 16 families of one.
        std::set<Hash256> wallFamilies;
        for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
            const kernel::SubId face{kernel::SubKind::Face, i};
            if (take(kernel::surfaceInfo(shape, face)).type != kernel::SurfaceType::Cylinder)
                continue;
            const TopoName* record = map.record(map.namesOf(face)[0]);
            REQUIRE(record != nullptr);
            CHECK(record->siblingCount == 1);
            wallFamilies.insert(record->family);
        }
        CHECK(wallFamilies.size() == 16);
    }
    SECTION("a sphere: its edges and vertices can only be told apart by ordinal")
    {
        const kernel::Shape shape = sphere();
        const ElementMap map = nameImported(feature, shape);
        const Census c = census(shape, map);
        CHECK(c.shared == 0);
        REQUIRE(shape.count(kernel::SubKind::Face) == 1);
        CHECK_FALSE(map.record(map.namesOf(kernel::SubId{kernel::SubKind::Face, 0})[0])->ambiguous());
        const std::size_t edges = shape.count(kernel::SubKind::Edge);
        const std::size_t vertices = shape.count(kernel::SubKind::Vertex);
        REQUIRE(edges > 1);
        REQUIRE(vertices > 1);
        CHECK(c.ambiguous == edges + vertices);
        for (std::uint32_t i = 0; i < edges; ++i) {
            const TopoName* record = map.record(map.namesOf(kernel::SubId{kernel::SubKind::Edge, i})[0]);
            CHECK(record->siblingCount == edges);
            CHECK(record->ordinal >= 1);
            CHECK(record->ordinal <= edges);
        }
    }
}

TEST_CASE("Siblings are told apart by their surrounding faces before any ordinal", "[naming][import][siblings]")
{
    const FeatureId feature = test::featureId(9);
    // Faces 0..3 have one and the same fingerprint; each touches a different one of faces 4..7.
    const Fingerprint same = planeAt(0);
    detail::BodyFacts facts = faceFacts({same, same, same, same, planeAt(10), planeAt(20), planeAt(30), planeAt(40)},
                                        {{4}, {5}, {6}, {7}, {0}, {1}, {2}, {3}});
    const ElementMap map = take(detail::assignNames(feature, facts));
    std::set<Hash256> names;
    Hash256 family{};
    for (std::uint32_t i = 0; i < 4; ++i) {
        const Hash256 name = map.namesOf(kernel::SubId{kernel::SubKind::Face, i})[0];
        names.insert(name);
        const TopoName* record = map.record(name);
        REQUIRE(record != nullptr);
        CHECK_FALSE(record->ambiguous());
        CHECK(record->ordinal == 0);
        CHECK(record->siblingCount == 4);
        CHECK(record->discriminator.size() == 1);
        if (i == 0)
            family = record->family;
        CHECK(record->family == family);
        CHECK(map.find(name) == kernel::SubId{kernel::SubKind::Face, i});
    }
    CHECK(names.size() == 4);
    CHECK(map.family(family).size() == 4);

    // Visiting order does not matter here either.
    detail::BodyFacts reversed = facts;
    std::reverse(reversed.faces.begin(), reversed.faces.end());
    CHECK(take(detail::assignNames(feature, reversed)).serialize() == map.serialize());
}

TEST_CASE("Siblings with identical surroundings get ordinals by fingerprint", "[naming][import][siblings]")
{
    const FeatureId feature = test::featureId(10);
    // Three edges on the same two faces and without vertices: one family, empty discriminators.
    detail::BodyFacts facts = faceFacts({planeAt(0), planeAt(100)}, {{1}, {0}});
    facts.edgeCount = 3;
    auto edge = [](std::uint32_t index, std::int64_t x) {
        detail::EdgeFacts e;
        e.index = index;
        e.fingerprint = planeAt(x, 5000);
        e.fingerprint.surface = static_cast<kernel::SurfaceType>(kernel::CurveType::Line);
        e.faces = {0, 1};
        return e;
    };
    facts.edges = {edge(0, 300), edge(1, 100), edge(2, 200)};
    const ElementMap map = take(detail::assignNames(feature, facts));
    // Ordinal follows the centroid (x = 100, 200, 300), not the SubId.
    const std::array<std::uint32_t, 3> expectedOrdinal{3, 1, 2};
    for (std::uint32_t i = 0; i < 3; ++i) {
        const TopoName* record = map.record(map.namesOf(kernel::SubId{kernel::SubKind::Edge, i})[0]);
        REQUIRE(record != nullptr);
        CHECK(record->ambiguous());
        CHECK(record->ordinal == expectedOrdinal[i]);
        CHECK(record->siblingCount == 3);
        CHECK(record->discriminator.empty());
    }
    std::reverse(facts.edges.begin(), facts.edges.end());
    CHECK(take(detail::assignNames(feature, facts)).serialize() == map.serialize());
}

TEST_CASE("A name issued alone differs from one issued as one of n", "[naming][import][siblings]")
{
    const FeatureId feature = test::featureId(11);
    auto withEdges = [](std::vector<std::int64_t> xs) {
        detail::BodyFacts facts = faceFacts({planeAt(0), planeAt(100)}, {{1}, {0}});
        facts.edgeCount = static_cast<std::uint32_t>(xs.size());
        for (std::uint32_t i = 0; i < xs.size(); ++i) {
            detail::EdgeFacts e;
            e.index = i;
            e.fingerprint = planeAt(xs[i], 5000);
            e.faces = {0, 1};
            facts.edges.push_back(e);
        }
        return facts;
    };
    const ElementMap one = take(detail::assignNames(feature, withEdges({100})));
    const ElementMap two = take(detail::assignNames(feature, withEdges({100, 200})));
    const Hash256 alone = one.namesOf(kernel::SubId{kernel::SubKind::Edge, 0})[0];
    CHECK_FALSE(one.record(alone)->ambiguous());
    for (std::uint32_t i = 0; i < 2; ++i) {
        const Hash256 sibling = two.namesOf(kernel::SubId{kernel::SubKind::Edge, i})[0];
        CHECK(sibling != alone);
        CHECK_FALSE(two.find(alone).has_value());
        CHECK(two.record(sibling)->family == one.record(alone)->family);
    }
    CHECK(two.family(one.record(alone)->family).size() == 2);
}

TEST_CASE("Elements that tie on everything share a name that find() refuses", "[naming][import][siblings]")
{
    const FeatureId feature = test::featureId(12);
    const Fingerprint same = planeAt(0);
    const detail::BodyFacts facts = faceFacts({same, same, planeAt(50)}, {{2}, {2}, {0, 1}});
    const ElementMap map = take(detail::assignNames(feature, facts));
    const Hash256 a = map.namesOf(kernel::SubId{kernel::SubKind::Face, 0})[0];
    const Hash256 b = map.namesOf(kernel::SubId{kernel::SubKind::Face, 1})[0];
    CHECK(a == b);
    CHECK(map.record(a)->ambiguous());
    CHECK_FALSE(map.find(a).has_value());
    const Hash256 c = map.namesOf(kernel::SubId{kernel::SubKind::Face, 2})[0];
    CHECK(map.find(c) == kernel::SubId{kernel::SubKind::Face, 2});
    // It survives a round trip.
    const ElementMap back = take(ElementMap::deserialize(map.serialize()));
    CHECK_FALSE(back.find(a).has_value());
    CHECK(back.serialize() == map.serialize());
}

TEST_CASE("Inconsistent facts and bad visiting orders are errors and never exceptions", "[naming][import]")
{
    const FeatureId feature = test::featureId(13);
    detail::BodyFacts facts = faceFacts({planeAt(0), planeAt(1)}, {{1}, {0}});
    facts.faces[1].neighbours = {5};
    CHECK_FALSE(detail::assignNames(feature, facts).has_value());
    facts = faceFacts({planeAt(0), planeAt(1)}, {{1}, {0}});
    facts.faces[1].index = 0;
    CHECK_FALSE(detail::assignNames(feature, facts).has_value());

    const kernel::Shape shape = cube();
    const detail::VisitOrder notAPermutation = [](kernel::SubKind, std::uint32_t count) {
        return std::vector<std::uint32_t>(count, 0);
    };
    CHECK_FALSE(detail::gatherFacts(shape, notAPermutation).has_value());
    ElementMap map;
    CHECK_NOTHROW(map = detail::nameImported(feature, shape, notAPermutation));
    CHECK(map.size() == 0);
    CHECK_NOTHROW(map = nameImported(feature, kernel::Shape{}));
    CHECK(map.size() == 0);
}

TEST_CASE("Runtime of nameImported on a body with a few thousand faces", "[naming][import][perf]")
{
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };

    const kernel::Shape star = take(kernel::testing::makeStarPrism(1500, 200.0, 180.0, 25.0));
    REQUIRE(star.count(kernel::SubKind::Face) == 3002);
    const FeatureId feature = test::featureId(14);

    const auto t0 = Clock::now();
    const ElementMap map = nameImported(feature, star);
    const auto t1 = Clock::now();
    const std::vector<std::byte> bytes = map.serialize();
    const auto t2 = Clock::now();
    const ElementMap back = take(ElementMap::deserialize(bytes));
    const auto t3 = Clock::now();

    const Census c = census(star, map);
    CHECK(c.elements == map.size());
    CHECK(c.shared == 0);
    CHECK(back.serialize() == bytes);
    CHECK(detail::nameImported(feature, star, shuffledOrder(99)).serialize() == bytes);

    std::printf("[naming perf] star prism: %u faces, %u edges, %u vertices, %zu ambiguous\n",
                star.count(kernel::SubKind::Face), star.count(kernel::SubKind::Edge),
                star.count(kernel::SubKind::Vertex), c.ambiguous);
    std::printf("[naming perf] nameImported %.1f ms, serialize %.1f ms (%zu bytes), deserialize %.1f ms\n", ms(t1 - t0),
                ms(t2 - t1), bytes.size(), ms(t3 - t2));
}
