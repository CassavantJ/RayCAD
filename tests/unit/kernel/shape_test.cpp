// Shape, canonical SubId order, serialisation and content hash (task T-004).
#include <catch2/catch_test_macros.hpp>

#include "kernel_test_util.h"

#include <shape_testing.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace ray;
using namespace ray::kernel;
using ray::kernel::test::box;
using ray::kernel::test::orderFingerprint;
using ray::kernel::test::take;

namespace {

Shape cylinder()
{
    return take(makeCylinder(Vec3{1, 2, 3}, Vec3{0, 0, 1}, 5.0, 12.0));
}

// What every SubId means, without Catch2 assertions (safe on worker threads). Empty on any error.
std::vector<unsigned char> fingerprintNoAssert(const Shape& shape)
{
    std::vector<unsigned char> out;
    auto fail = [&] {
        out.clear();
        return out;
    };
    for (std::uint32_t i = 0; i < shape.count(SubKind::Vertex); ++i) {
        auto p = vertexPoint(shape, SubId{SubKind::Vertex, i});
        if (!p)
            return fail();
        test::appendBits(out, *p);
    }
    for (std::uint32_t i = 0; i < shape.count(SubKind::Edge); ++i) {
        auto c = curveInfo(shape, SubId{SubKind::Edge, i});
        if (!c)
            return fail();
        test::appendBits(out, c->start);
        test::appendBits(out, c->end);
        test::appendBits(out, c->length);
    }
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
        auto s = surfaceInfo(shape, SubId{SubKind::Face, i});
        if (!s)
            return fail();
        test::appendBits(out, s->centroid);
        test::appendBits(out, s->area);
        for (const SubKind kind : {SubKind::Vertex, SubKind::Edge, SubKind::Face, SubKind::Solid})
            for (const SubId id : adjacent(shape, SubId{SubKind::Face, i}, kind))
                test::appendBits(out, static_cast<double>(id.index));
    }
    return out;
}

struct Built {
    std::vector<unsigned char> fingerprint;
    std::vector<std::byte> bytes;
    Hash256 hash;
};

// Builds the three test solids and describes them. Empty fingerprint on any error. No Catch2 assertions.
std::array<Built, 3> buildAndDescribe()
{
    std::array<Built, 3> out;
    std::array<Result<Shape>, 3> shapes{makeBox(Vec3{0, 0, 0}, Vec3{10, 20, 30}),
                                        makeCylinder(Vec3{1, 2, 3}, Vec3{0, 0, 1}, 5.0, 12.0), testing::makeCutBody()};
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        if (!shapes[i])
            continue;
        out[i].fingerprint = fingerprintNoAssert(*shapes[i]);
        out[i].bytes = serialize(*shapes[i]);
        out[i].hash = shapes[i]->contentHash();
    }
    return out;
}

// Runs body(0) .. body(count - 1) on `count` threads that all start at once, and joins them.
template <class F>
void runOnThreads(int count, F body)
{
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(count));
    for (int t = 0; t < count; ++t)
        threads.emplace_back([&body, t] { body(t); });
    for (std::thread& thread : threads)
        thread.join();
}

void requireRoundTrip(const Shape& shape)
{
    const std::vector<std::byte> first = serialize(shape);
    REQUIRE_FALSE(first.empty());
    const Shape back = take(deserialize(first));
    const std::vector<std::byte> second = serialize(back);
    REQUIRE(second == first);
    REQUIRE(back.contentHash() == shape.contentHash());
    for (const SubKind kind : {SubKind::Vertex, SubKind::Edge, SubKind::Face, SubKind::Solid})
        REQUIRE(back.count(kind) == shape.count(kind));
    REQUIRE(orderFingerprint(back) == orderFingerprint(shape));
}

} // namespace

TEST_CASE("sub-shape counts of the primitives", "[kernel][shape]")
{
    const Shape b = box(Vec3{0, 0, 0}, Vec3{10, 20, 30});
    REQUIRE_FALSE(b.isNull());
    CHECK(b.count(SubKind::Vertex) == 8);
    CHECK(b.count(SubKind::Edge) == 12);
    CHECK(b.count(SubKind::Face) == 6);
    CHECK(b.count(SubKind::Solid) == 1);

    // A cylinder: two cap circles and a seam line; two planar caps and the lateral face.
    const Shape c = cylinder();
    CHECK(c.count(SubKind::Vertex) == 2);
    CHECK(c.count(SubKind::Edge) == 3);
    CHECK(c.count(SubKind::Face) == 3);
    CHECK(c.count(SubKind::Solid) == 1);

    const Shape face = take(testing::makePlanarFace());
    CHECK(face.count(SubKind::Vertex) == 4);
    CHECK(face.count(SubKind::Edge) == 4);
    CHECK(face.count(SubKind::Face) == 1);
    CHECK(face.count(SubKind::Solid) == 0);

    // The same TShape at two locations counts twice.
    const Shape twice = take(testing::makeLocatedCompound());
    CHECK(twice.count(SubKind::Vertex) == 16);
    CHECK(twice.count(SubKind::Face) == 12);
    CHECK(twice.count(SubKind::Solid) == 2);

    CHECK(b.count(static_cast<SubKind>(7)) == 0);
}

TEST_CASE("a null shape", "[kernel][shape]")
{
    const Shape null;
    REQUIRE(null.isNull());
    for (const SubKind kind : {SubKind::Vertex, SubKind::Edge, SubKind::Face, SubKind::Solid})
        CHECK(null.count(kind) == 0);
    REQUIRE_NOTHROW(null.contentHash());
    CHECK(null.contentHash() == Shape{}.contentHash());
    CHECK(null.contentHash() != box(Vec3{0, 0, 0}, Vec3{1, 1, 1}).contentHash());
    CHECK(serialize(null).empty());
}

TEST_CASE("serialise, deserialise, serialise is byte-identical", "[kernel][shape][serialize]")
{
    SECTION("box")
    {
        requireRoundTrip(box(Vec3{-1, 2, -3}, Vec3{10, 20, 30}));
    }
    SECTION("cylinder")
    {
        requireRoundTrip(cylinder());
        requireRoundTrip(take(makeCylinder(Vec3{0, 0, 0}, Vec3{1, 1, 1}, 2.5, 7.0)));
    }
    SECTION("boolean cut built through OCCT directly")
    {
        const Shape cut = take(testing::makeCutBody());
        CHECK(cut.count(SubKind::Solid) == 1);
        CHECK(cut.count(SubKind::Face) > 6);
        requireRoundTrip(cut);
    }
    SECTION("compound with a located copy")
    {
        requireRoundTrip(take(testing::makeLocatedCompound()));
    }
    SECTION("lone face")
    {
        requireRoundTrip(take(testing::makePlanarFace()));
    }
    SECTION("twice through")
    {
        const Shape once = take(deserialize(serialize(take(testing::makeCutBody()))));
        const Shape twice = take(deserialize(serialize(once)));
        REQUIRE(serialize(twice) == serialize(once));
    }
}

TEST_CASE("serialisation has no timestamps, pointers or mesh and is the same for equal shapes",
          "[kernel][shape][serialize]")
{
    // Two independent builds give the same bytes (no pointer or address reaches them).
    const Shape a = box(Vec3{0, 0, 0}, Vec3{10, 20, 30});
    const Shape b = box(Vec3{0, 0, 0}, Vec3{10, 20, 30});
    REQUIRE(serialize(a) == serialize(b));
    REQUIRE(serialize(take(testing::makeCutBody())) == serialize(take(testing::makeCutBody())));

    // Meshing a shape and adding it to a compound changes OCCT's in-place flags and adds triangulations;
    // none of that may reach the bytes.
    const Shape meshed = box(Vec3{0, 0, 0}, Vec3{10, 20, 30});
    const std::vector<std::byte> before = serialize(meshed);
    REQUIRE(testing::meshAndShare(meshed).has_value());
    CHECK(serialize(meshed) == before);
    CHECK(before == serialize(a));

    const Shape cut = take(testing::makeCutBody());
    const std::vector<std::byte> cutBefore = serialize(cut);
    REQUIRE(testing::meshAndShare(cut).has_value());
    CHECK(serialize(cut) == cutBefore);
}

TEST_CASE("contentHash is equal for equal shapes and different for different ones", "[kernel][shape][hash]")
{
    const Shape a = box(Vec3{0, 0, 0}, Vec3{10, 20, 30});
    const Shape b = box(Vec3{0, 0, 0}, Vec3{10, 20, 30});
    const Shape c = box(Vec3{0, 0, 0}, Vec3{10, 20, 30.000001});
    const Shape d = box(Vec3{1, 0, 0}, Vec3{11, 20, 30});
    CHECK(a.contentHash() == b.contentHash());
    CHECK(a.contentHash() == a.contentHash());
    CHECK(a.contentHash() != c.contentHash());
    CHECK(a.contentHash() != d.contentHash());
    CHECK(a.contentHash() != cylinder().contentHash());
    CHECK(cylinder().contentHash() == cylinder().contentHash());
    CHECK(take(testing::makeCutBody()).contentHash() == take(testing::makeCutBody()).contentHash());
    CHECK(take(testing::makeCutBody()).contentHash() != a.contentHash());
    CHECK(a.contentHash() != Hash256{});
}

TEST_CASE("canonical SubId order of a box is fixed", "[kernel][shape][order]")
{
    // Golden values: the order is part of the kernel's determinism guarantee (identical across runs and
    // operating systems). A change here changes every SubId of every box and must be deliberate.
    const Shape b = box(Vec3{0, 0, 0}, Vec3{10, 20, 30});
    std::vector<Vec3> vertices;
    for (std::uint32_t i = 0; i < b.count(SubKind::Vertex); ++i)
        vertices.push_back(take(vertexPoint(b, SubId{SubKind::Vertex, i})));
    std::vector<Vec3> faceCentroids;
    for (std::uint32_t i = 0; i < b.count(SubKind::Face); ++i)
        faceCentroids.push_back(take(surfaceInfo(b, SubId{SubKind::Face, i})).centroid);
    std::vector<Vec3> edgeMidpoints;
    for (std::uint32_t i = 0; i < b.count(SubKind::Edge); ++i) {
        const CurveInfo info = take(curveInfo(b, SubId{SubKind::Edge, i}));
        edgeMidpoints.push_back(
            Vec3{(info.start.x + info.end.x) / 2, (info.start.y + info.end.y) / 2, (info.start.z + info.end.z) / 2});
    }
    auto same = [](const std::vector<Vec3>& actual, const std::vector<Vec3>& expected) {
        if (actual.size() != expected.size())
            return false;
        for (std::size_t i = 0; i < actual.size(); ++i)
            if (!test::close(actual[i], expected[i], 1e-12))
                return false;
        return true;
    };
    CHECK(same(vertices,
               {{0, 0, 30}, {0, 0, 0}, {0, 20, 30}, {0, 20, 0}, {10, 0, 30}, {10, 0, 0}, {10, 20, 30}, {10, 20, 0}}));
    CHECK(same(edgeMidpoints, {{0, 0, 15},
                               {0, 10, 30},
                               {0, 20, 15},
                               {0, 10, 0},
                               {10, 0, 15},
                               {10, 10, 30},
                               {10, 20, 15},
                               {10, 10, 0},
                               {5, 0, 0},
                               {5, 0, 30},
                               {5, 20, 0},
                               {5, 20, 30}}));
    CHECK(same(faceCentroids, {{0, 10, 15}, {10, 10, 15}, {5, 0, 15}, {5, 20, 15}, {5, 10, 0}, {5, 10, 30}}));
}

TEST_CASE("SubId order is identical across repeated builds and threads", "[kernel][shape][order]")
{
    const std::array<Built, 3> reference = buildAndDescribe();
    for (const Built& built : reference) {
        REQUIRE_FALSE(built.fingerprint.empty());
        REQUIRE_FALSE(built.bytes.empty());
    }

    SECTION("repeated builds")
    {
        for (int run = 0; run < 3; ++run) {
            const std::array<Built, 3> again = buildAndDescribe();
            for (std::size_t i = 0; i < again.size(); ++i) {
                CHECK(again[i].fingerprint == reference[i].fingerprint);
                CHECK(again[i].bytes == reference[i].bytes);
                CHECK(again[i].hash == reference[i].hash);
            }
        }
    }

    SECTION("built and queried on 8 threads at once")
    {
        constexpr int kThreads = 8;
        std::vector<std::array<Built, 3>> results(kThreads);
        runOnThreads(kThreads, [&results](int t) {
            for (int run = 0; run < 2; ++run)
                results[static_cast<std::size_t>(t)] = buildAndDescribe();
        });
        for (const auto& perThread : results)
            for (std::size_t i = 0; i < perThread.size(); ++i) {
                CHECK(perThread[i].fingerprint == reference[i].fingerprint);
                CHECK(perThread[i].bytes == reference[i].bytes);
            }
    }

    SECTION("one shared shape queried from 8 threads at once")
    {
        // The adjacency maps and the content hash of a Shape are built lazily; first use races here.
        const Shape shared = take(testing::makeCutBody());
        constexpr int kThreads = 8;
        std::vector<std::vector<unsigned char>> prints(kThreads);
        std::vector<Hash256> hashes(kThreads);
        runOnThreads(kThreads, [&](int t) {
            hashes[static_cast<std::size_t>(t)] = shared.contentHash();
            prints[static_cast<std::size_t>(t)] = fingerprintNoAssert(shared);
        });
        for (int t = 0; t < kThreads; ++t) {
            CHECK(prints[static_cast<std::size_t>(t)] == reference[2].fingerprint);
            CHECK(hashes[static_cast<std::size_t>(t)] == reference[2].hash);
        }
    }
}

TEST_CASE("deserialise rejects corrupt and truncated input", "[kernel][shape][serialize]")
{
    const std::vector<std::byte> good = serialize(take(testing::makeCutBody()));
    REQUIRE(good.size() > 100);
    REQUIRE(deserialize(good).has_value());

    auto expectError = [](std::span<const std::byte> bytes, std::initializer_list<ErrorCode> allowed) {
        Result<Shape> result = Shape{};
        REQUIRE_NOTHROW(result = deserialize(bytes));
        REQUIRE_FALSE(result.has_value());
        INFO(result.error().message);
        CHECK(std::find(allowed.begin(), allowed.end(), result.error().code) != allowed.end());
        CHECK(result.error().message.find("structured exception") == std::string::npos);
    };

    SECTION("empty and tiny")
    {
        expectError({}, {ErrorCode::FormatError});
        expectError(std::span(good).first(10), {ErrorCode::FormatError});
    }
    SECTION("truncated at every length class")
    {
        for (const std::size_t keep :
             {std::size_t{16}, std::size_t{47}, std::size_t{48}, good.size() / 2, good.size() - 33, good.size() - 1})
            expectError(std::span(good).first(keep), {ErrorCode::FormatError});
    }
    SECTION("extra bytes at the end")
    {
        std::vector<std::byte> longer = good;
        longer.push_back(std::byte{0});
        expectError(longer, {ErrorCode::FormatError});
    }
    SECTION("wrong magic")
    {
        std::vector<std::byte> bad = good;
        bad[0] = std::byte{'X'};
        expectError(bad, {ErrorCode::FormatError});
    }
    SECTION("format version")
    {
        std::vector<std::byte> bad = good;
        bad[7] = std::byte{2};
        expectError(bad, {ErrorCode::FormatTooNew});
        bad[7] = std::byte{0};
        expectError(bad, {ErrorCode::FormatError});
    }
    SECTION("any flipped byte is caught by the checksum")
    {
        for (std::size_t at = 8; at < good.size(); at += 37) {
            std::vector<std::byte> bad = good;
            bad[at] ^= std::byte{0x5A};
            expectError(bad, {ErrorCode::FormatError});
        }
    }
    SECTION("damaged payload behind a valid checksum")
    {
        const std::vector<std::byte> payload = testing::payloadOf(good);
        REQUIRE_FALSE(payload.empty());
        REQUIRE(deserialize(testing::seal(payload)).has_value());

        std::vector<std::byte> header = payload; // wrong BinTools header line
        header[10] = std::byte{'#'};
        expectError(testing::seal(header), {ErrorCode::FormatError, ErrorCode::KernelFailure});

        // Only the header line: the location table is missing.
        const std::size_t headerLength = 1 + std::string("Open CASCADE Topology V4, (c) Open Cascade").size() + 1;
        expectError(testing::seal(std::span(payload).first(headerLength)),
                    {ErrorCode::FormatError, ErrorCode::KernelFailure});

        std::vector<std::byte> root = payload; // root reference points past the shape table
        for (std::size_t i = root.size() - 8; i < root.size() - 4; ++i)
            root[i] = std::byte{0x7F};
        expectError(testing::seal(root), {ErrorCode::FormatError, ErrorCode::KernelFailure});

        std::vector<std::byte> orientation = payload; // invalid root orientation
        orientation[orientation.size() - 9] = std::byte{9};
        expectError(testing::seal(orientation), {ErrorCode::FormatError, ErrorCode::KernelFailure});

        std::vector<std::byte> trailing = payload; // payload longer than the shape
        trailing.push_back(std::byte{0});
        expectError(testing::seal(trailing), {ErrorCode::FormatError, ErrorCode::KernelFailure});
    }
}

TEST_CASE("makeBox and makeCylinder reject degenerate input", "[kernel][shape]")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    auto invalid = [](const Result<Shape>& result) {
        return !result.has_value() && result.error().code == ErrorCode::InvalidArgument;
    };
    CHECK(invalid(makeBox(Vec3{0, 0, 0}, Vec3{0, 1, 1})));
    CHECK(invalid(makeBox(Vec3{0, 0, 0}, Vec3{1, 0, 1})));
    CHECK(invalid(makeBox(Vec3{0, 0, 0}, Vec3{1, 1, 0})));
    CHECK(invalid(makeBox(Vec3{0, 0, 0}, Vec3{1, 1, 1e-9})));
    CHECK(invalid(makeBox(Vec3{1, 1, 1}, Vec3{0, 2, 2})));
    CHECK(invalid(makeBox(Vec3{0, 0, 0}, Vec3{nan, 1, 1})));
    CHECK(invalid(makeBox(Vec3{0, -inf, 0}, Vec3{1, 1, 1})));

    CHECK(invalid(makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 1}, 0.0, 1.0)));
    CHECK(invalid(makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 1}, -1.0, 1.0)));
    CHECK(invalid(makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 1}, 1.0, 0.0)));
    CHECK(invalid(makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 1}, 1.0, -2.0)));
    CHECK(invalid(makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 0}, 1.0, 1.0)));
    CHECK(invalid(makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 1e-12}, 1.0, 1.0)));
    CHECK(invalid(makeCylinder(Vec3{nan, 0, 0}, Vec3{0, 0, 1}, 1.0, 1.0)));
    CHECK(invalid(makeCylinder(Vec3{0, 0, 0}, Vec3{0, 0, 1}, inf, 1.0)));

    CHECK(makeBox(Vec3{0, 0, 0}, Vec3{1e-3, 1e-3, 1e-3}).has_value());
    CHECK(makeCylinder(Vec3{0, 0, 0}, Vec3{3, -4, 12}, 0.5, 1e4).has_value());
}
