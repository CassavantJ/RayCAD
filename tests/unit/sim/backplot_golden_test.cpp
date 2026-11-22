// Determinism of the backplot (task T-037): a fixed toolpath of every move kind gives the same bytes on every run,
// thread and platform. The golden hash below was made on Windows (MSVC) and checked on Linux (GCC); a change to it
// is a change of the backplot's output and needs a reason. (Remade for the canned-cycle settlement of cam.h: the
// cycles now carry their clearance, return level and peck clearance.)
#include "backplot_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstring>
#include <thread>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

// Every move kind, in all planes and directions, with awkward numbers: helices, a spiral from a rounded centre, a
// 2 m radius and a 0.05 mm radius, and each canned cycle.
cam::Toolpath goldenToolpath()
{
    std::vector<Move> m;
    m.push_back(rapid(12.345678, -3.21, 25.0, MoveRole::Approach));
    m.push_back(line(12.345678, -3.21, 1.5, 300.0, MoveRole::Plunge));
    m.push_back(line(19.876543, -3.21, 1.5, 1234.5, MoveRole::LeadIn));
    m.push_back(arc(true, ArcPlane::XY, 12.345678, 4.320865, 1.5, 12.345678, -3.21, 1.5));     // a quarter
    m.push_back(arc(false, ArcPlane::XY, 12.345678, 4.320865, -0.75, 12.345678, -3.21, -0.4)); // full helix turn
    m.push_back(arc(true, ArcPlane::XY, 4.815162, -3.209997, -0.75, 12.345678, -3.21, 0.0));   // spiral
    m.push_back(arc(true, ArcPlane::XZ, 7.0, -3.21, 3.0, 7.0, -3.21, 0.5, 800.0, MoveRole::Ramp));
    m.push_back(arc(false, ArcPlane::YZ, 7.0, 1.0, 3.0, 0.0, -1.105, 3.0, 800.0, MoveRole::LeadOut));
    m.push_back(arc(true, ArcPlane::XY, 7.0, 1.1, 3.0, 7.0, 1.05, 0.0, 600.0)); // 0.05 mm radius half circle
    m.push_back(arc(false, ArcPlane::XY, 4007.0, 1.1, 3.0, 2007.0, 1.1, 0.0, 600.0, MoveRole::Link)); // 2 m radius
    m.push_back(blankMove(MoveKind::Dwell, MoveRole::Cut));
    m.push_back(rapid(40.0, 40.0, 30.0));
    // Cycles (cam.h): top 0, clearance 20 (below the tool, so the first comes down to it), G98 and G99 mixed, a peck
    // clearance of the move's own on the chip break, a left-hand tap.
    const auto g98 = cam::CycleReturn::Initial;
    const auto g99 = cam::CycleReturn::RPlane;
    m.push_back(levels(cycle(MoveKind::CycleDrill, 40.0, 40.0, -6.1, 2.0, 0.0, 150.0, 250), 0.0, 20.0, g99));
    m.push_back(levels(cycle(MoveKind::CyclePeck, 50.0, 40.0, -17.3, 2.0, 3.7, 140.0), 0.0, 20.0, g98));
    Move chipBreak = levels(cycle(MoveKind::CycleChipBreak, 60.0, 40.0, -17.3, 2.0, 2.2, 140.0), 0.0, 20.0, g99);
    chipBreak.cyclePeckClearance = nm(0.35);
    m.push_back(chipBreak);
    Move tap = levels(cycle(MoveKind::CycleTap, 70.0, 40.0, -12.0, 5.0, 0.0, 1270.0), 0.0, 20.0, g98);
    tap.cycleLeftHand = 1;
    m.push_back(tap);
    m.push_back(levels(cycle(MoveKind::CycleBore, 80.0, 40.0, -12.0, 2.0, 0.0, 90.0, 400), 0.0, 20.0, g99));
    m.push_back(levels(cycle(MoveKind::CycleReam, 90.0, 40.0, -12.0, 2.0, 0.0, 90.0), 0.0, 20.0, g98));
    m.push_back(blankMove(MoveKind::CycleCancel, MoveRole::Retract));
    m.push_back(rapid(90.0, 40.0, 50.0, MoveRole::Retract));
    return toolpath(0.0, 0.0, 50.0, m);
}

// A model frame turned 30 degrees about Z and shifted (literal cosines, so the input does not depend on a libm).
const Xform kModelFromWcs{
    {0.8660254037844386, -0.5, 0.0, 101.25, 0.5, 0.8660254037844386, 0.0, -48.5, 0.0, 0.0, 1.0, -17.0}};

void putU32(std::vector<std::byte>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}

// Little-endian, field by field.
std::vector<std::byte> bytesOf(const Backplot& bp)
{
    std::vector<std::byte> out;
    putU32(out, static_cast<std::uint32_t>(bp.lineVertices.size()));
    for (const BackplotVertex& v : bp.lineVertices) {
        putU32(out, std::bit_cast<std::uint32_t>(v.x));
        putU32(out, std::bit_cast<std::uint32_t>(v.y));
        putU32(out, std::bit_cast<std::uint32_t>(v.z));
        putU32(out, static_cast<std::uint32_t>(v.role) | (static_cast<std::uint32_t>(v.kind) << 8) |
                        (static_cast<std::uint32_t>(v.pad) << 16));
        putU32(out, std::bit_cast<std::uint32_t>(v.feed));
        putU32(out, v.moveIndex);
    }
    putU32(out, static_cast<std::uint32_t>(bp.moveFirstVertex.size()));
    for (std::uint32_t f : bp.moveFirstVertex) {
        putU32(out, f);
    }
    return out;
}

std::string hashOf(const Backplot& bp)
{
    const std::vector<std::byte> b = bytesOf(bp);
    Hasher h;
    h.bytes(b);
    return h.finish().hex();
}

} // namespace

TEST_CASE("the backplot of a fixed toolpath matches its golden hash", "[sim][backplot][golden]")
{
    const cam::Toolpath tp = goldenToolpath();
    const Backplot bp = buildBackplot(tp, kModelFromWcs, 0.002);
    checkLayout(tp, bp);
    CHECK(bp.lineVertices.size() == 1451);
    CHECK(hashOf(bp) == "015710dbde3cea32e5cf7807cc003f88a53cae5477475ceffff401c888b92ee5");
    // A few values written out, so a mismatch says where.
    CHECK(std::bit_cast<std::uint32_t>(bp.lineVertices[0].x) == std::bit_cast<std::uint32_t>(101.25f));
}

TEST_CASE("the backplot is the same on every run and thread", "[sim][backplot][golden]")
{
    const cam::Toolpath tp = goldenToolpath();
    const std::vector<std::byte> first = bytesOf(buildBackplot(tp, kModelFromWcs, 0.002));
    std::vector<std::vector<std::byte>> results(4);
    {
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < results.size(); ++i) {
            threads.emplace_back([&, i] { results[i] = bytesOf(buildBackplot(tp, kModelFromWcs, 0.002)); });
        }
        for (std::thread& t : threads) {
            t.join();
        }
    }
    for (const auto& r : results) {
        CHECK(r == first);
    }
    // In-memory layout too: 24 bytes per vertex, no padding, pad zeroed (the renderer uploads it as it is).
    STATIC_REQUIRE(sizeof(BackplotVertex) == 24);
    const Backplot bp = buildBackplot(tp, kModelFromWcs, 0.002);
    const Backplot again = buildBackplot(tp, kModelFromWcs, 0.002);
    REQUIRE(bp.lineVertices.size() == again.lineVertices.size());
    CHECK(std::memcmp(bp.lineVertices.data(), again.lineVertices.data(), bp.lineVertices.size() * 24) == 0);
}
