// The RCTP toolpath format, version 2 (cam.h "Move: the RCTP record", PLAN.md 3.6): the Move layout, the bytes
// Toolpath::serialize writes, the round trip, and what deserialize refuses.
#include <ray/cam.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstring>
#include <type_traits>

using namespace ray;
using namespace ray::cam;

namespace {

// The documented layout: no implicit padding, every field where cam.h says.
static_assert(sizeof(Move) == 112);
static_assert(std::is_trivially_copyable_v<Move> && std::is_standard_layout_v<Move>);
static_assert(std::has_unique_object_representations_v<Move>, "Move has implicit padding");
static_assert(offsetof(Move, kind) == 0 && offsetof(Move, role) == 1 && offsetof(Move, plane) == 2 &&
              offsetof(Move, compensation) == 3 && offsetof(Move, cycleReturn) == 4 &&
              offsetof(Move, cycleLeftHand) == 5 && offsetof(Move, pad0) == 6);
static_assert(offsetof(Move, feedMicronPerMin) == 8 && offsetof(Move, cycleDwellMs) == 12 &&
              offsetof(Move, threadPitchNm) == 16 && offsetof(Move, pad1) == 20);
static_assert(offsetof(Move, x) == 24 && offsetof(Move, y) == 32 && offsetof(Move, z) == 40 &&
              offsetof(Move, i) == 48 && offsetof(Move, j) == 56 && offsetof(Move, k) == 64);
static_assert(offsetof(Move, cycleRetract) == 72 && offsetof(Move, cycleTop) == 80 &&
              offsetof(Move, cycleClearance) == 88 && offsetof(Move, cyclePeck) == 96 &&
              offsetof(Move, cyclePeckClearance) == 104);

// Every field set, to a value that shows its byte order.
Move everyField(std::int64_t seed)
{
    Move m{};
    m.kind = MoveKind::CycleTap;
    m.role = MoveRole::Plunge;
    m.plane = ArcPlane::YZ;
    m.compensation = 2;
    m.cycleReturn = CycleReturn::Initial;
    m.cycleLeftHand = 1;
    m.feedMicronPerMin = 0x01020304 + static_cast<std::int32_t>(seed);
    m.cycleDwellMs = 250;
    m.threadPitchNm = 1'250'000;
    m.x = 0x0102030405060708 + seed;
    m.y = -1;
    m.z = -12'000'000;
    m.i = 4;
    m.j = 5;
    m.k = -6;
    m.cycleRetract = 3'000'000;
    m.cycleTop = 0;
    m.cycleClearance = 25'000'000;
    m.cyclePeck = 7;
    m.cyclePeckClearance = 300'000;
    return m;
}

Toolpath sample()
{
    Toolpath tp{};
    tp.operation.value = Uuid::fromString("0c000000-0000-4000-8000-000000000001");
    for (std::size_t n = 0; n < tp.inputHash.bytes.size(); ++n) {
        tp.inputHash.bytes[n] = static_cast<std::uint8_t>(n * 7);
    }
    tp.startX = 1;
    tp.startY = -2;
    tp.startZ = 50'000'000;
    tp.moves = {everyField(0), everyField(1)};
    Move rapid{};
    rapid.kind = MoveKind::Rapid;
    rapid.role = MoveRole::Retract;
    rapid.z = 50'000'000;
    tp.moves.push_back(rapid);
    return tp;
}

std::uint64_t readLe(const std::vector<std::byte>& b, std::size_t at, int bytes)
{
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b[at + static_cast<std::size_t>(i)])) << (8 * i);
    }
    return v;
}

} // namespace

TEST_CASE("serialize writes the documented RCTP bytes", "[cam][toolpath][format]")
{
    const Toolpath tp = sample();
    const std::vector<std::byte> b = tp.serialize();
    REQUIRE(b.size() == 88 + 112 * 3);
    CHECK(std::memcmp(b.data(), "RCTP", 4) == 0);
    CHECK(readLe(b, 4, 4) == kToolpathFormatVersion);
    CHECK(kToolpathFormatVersion == 2);
    CHECK(std::memcmp(b.data() + 8, tp.operation.value.bytes.data(), 16) == 0);
    CHECK(std::memcmp(b.data() + 24, tp.inputHash.bytes.data(), 32) == 0);
    CHECK(readLe(b, 56, 8) == 3);
    CHECK(static_cast<std::int64_t>(readLe(b, 72, 8)) == -2);
    CHECK(readLe(b, 80, 8) == 50'000'000);
    // The first record, field by field at cam.h's offsets.
    const std::size_t r = 88;
    CHECK(readLe(b, r + 0, 1) == static_cast<std::uint8_t>(MoveKind::CycleTap));
    CHECK(readLe(b, r + 1, 1) == static_cast<std::uint8_t>(MoveRole::Plunge));
    CHECK(readLe(b, r + 2, 1) == 2);
    CHECK(readLe(b, r + 3, 1) == 2);
    CHECK(readLe(b, r + 4, 1) == 1);
    CHECK(readLe(b, r + 5, 1) == 1);
    CHECK(readLe(b, r + 6, 2) == 0);
    CHECK(readLe(b, r + 8, 4) == 0x01020304);
    CHECK(readLe(b, r + 12, 4) == 250);
    CHECK(readLe(b, r + 16, 4) == 1'250'000);
    CHECK(readLe(b, r + 20, 4) == 0);
    CHECK(readLe(b, r + 24, 8) == 0x0102030405060708);
    CHECK(static_cast<std::int64_t>(readLe(b, r + 32, 8)) == -1);
    CHECK(static_cast<std::int64_t>(readLe(b, r + 40, 8)) == -12'000'000);
    CHECK(readLe(b, r + 72, 8) == 3'000'000);
    CHECK(readLe(b, r + 80, 8) == 0);
    CHECK(readLe(b, r + 88, 8) == 25'000'000);
    CHECK(readLe(b, r + 96, 8) == 7);
    CHECK(readLe(b, r + 104, 8) == 300'000);
    // On a little-endian machine the record is the in-memory Move, byte for byte.
    if constexpr (std::endian::native == std::endian::little) {
        for (std::size_t n = 0; n < tp.moves.size(); ++n) {
            CHECK(std::memcmp(b.data() + 88 + 112 * n, &tp.moves[n], 112) == 0);
        }
    }
}

TEST_CASE("deserialize gives back what serialize wrote; hash() is BLAKE3 of it", "[cam][toolpath][format]")
{
    Toolpath tp = sample();
    tp.warnings = {"not stored"};
    tp.stats.cutLengthMm = 12.5; // not stored either
    const std::vector<std::byte> b = tp.serialize();
    const auto back = Toolpath::deserialize(b);
    REQUIRE(back.has_value());
    CHECK(back->operation == tp.operation);
    CHECK(back->inputHash == tp.inputHash);
    CHECK((back->startX == 1 && back->startY == -2 && back->startZ == 50'000'000));
    REQUIRE(back->moves.size() == 3);
    CHECK(std::memcmp(back->moves.data(), tp.moves.data(), 3 * sizeof(Move)) == 0);
    CHECK(back->warnings.empty());
    CHECK(back->stats.cutLengthMm == 0);
    CHECK(back->serialize() == b);
    Hasher h;
    h.bytes(b);
    CHECK(tp.hash() == h.finish());
    // Each cycle field is part of the hash.
    for (int field = 0; field < 6; ++field) {
        Toolpath other = tp;
        Move& m = other.moves[0];
        switch (field) {
        case 0:
            m.cycleTop += 1;
            break;
        case 1:
            m.cycleClearance += 1;
            break;
        case 2:
            m.cycleReturn = CycleReturn::RPlane;
            break;
        case 3:
            m.cyclePeckClearance += 1;
            break;
        case 4:
            m.cycleLeftHand = 0;
            break;
        default:
            m.cyclePeck += 1;
            break;
        }
        INFO("field " << field);
        CHECK(other.hash() != tp.hash());
    }
    // An empty toolpath is the header alone.
    CHECK(Toolpath{}.serialize().size() == 88);
    CHECK(Toolpath::deserialize(Toolpath{}.serialize()).has_value());
}

TEST_CASE("deserialize refuses what is not a version-2 RCTP toolpath", "[cam][toolpath][format]")
{
    const std::vector<std::byte> good = sample().serialize();
    const auto refused = [](std::vector<std::byte> b, ErrorCode code) {
        const auto r = Toolpath::deserialize(b);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == code);
    };
    auto b = good;
    b[0] = std::byte{'X'};
    refused(b, ErrorCode::FormatError);
    refused(std::vector<std::byte>(good.begin(), good.begin() + 40), ErrorCode::FormatError);
    refused(std::vector<std::byte>(good.begin(), good.end() - 1), ErrorCode::FormatError); // size and count disagree
    b = good;
    b.push_back(std::byte{0});
    refused(b, ErrorCode::FormatError);
    b = good;
    b[56] = std::byte{0xFF}; // a count far beyond the bytes
    b[63] = std::byte{0x7F};
    refused(b, ErrorCode::FormatError);
    b = good;
    b[4] = std::byte{1}; // version 1 was never written
    refused(b, ErrorCode::FormatError);
    b[4] = std::byte{3};
    refused(b, ErrorCode::FormatTooNew);
    // Values outside their range, pads that are not 0.
    for (const auto& [offset, value] : std::vector<std::pair<std::size_t, std::uint8_t>>{
             {0, 12}, {1, 8}, {2, 3}, {3, 3}, {4, 2}, {5, 2}, {6, 1}, {7, 1}, {20, 1}, {23, 0x80}}) {
        INFO("offset " << offset << " value " << int(value));
        b = good;
        b[88 + 112 + offset] = std::byte{value};
        refused(b, ErrorCode::FormatError);
    }
}
