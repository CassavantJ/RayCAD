// ElementMap, TopoName and Fingerprint: lookups, the tolerance compare, and the serialised form (task T-007).
#include <catch2/catch_test_macros.hpp>

#include "naming_test_util.h"

#include <naming_internal.h>
#include <shape_testing.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>
#include <span>
#include <vector>

using namespace ray;
using namespace ray::naming;
using ray::naming::test::take;

namespace {

Fingerprint sampleFingerprint()
{
    Fingerprint f = detail::emptyFingerprint();
    f.surface = kernel::SurfaceType::Plane;
    f.qArea = 1'000'000;
    f.qCx = 50'000;
    f.qCy = -20'000;
    f.qCz = 100'000;
    f.qRadius = 0;
    f.qAxisX = 0;
    f.qAxisY = 0;
    f.qAxisZ = 1'000'000;
    f.neighbourCount = 4;
    return f;
}

std::vector<std::byte> reseal(std::vector<std::byte> bytes)
{
    REQUIRE(bytes.size() >= 32);
    const std::span<const std::byte> body(bytes.data(), bytes.size() - 32);
    Hasher hasher;
    hasher.bytes(body);
    const Hash256 sum = hasher.finish();
    std::memcpy(bytes.data() + body.size(), sum.bytes.data(), 32);
    return bytes;
}

ElementMap plateMap()
{
    const std::array<Vec2, 2> holes{Vec2{20, 20}, Vec2{60, 30}};
    const kernel::Shape plate = take(kernel::testing::makePlateWithHoles(Vec3{100, 60, 10}, 5.0, holes));
    return nameImported(test::featureId(1), plate);
}

} // namespace

TEST_CASE("Fingerprint::matches compares with a tolerance", "[naming][fingerprint]")
{
    const Fingerprint a = sampleFingerprint();
    CHECK(a.matches(a));

    Fingerprint b = a;
    b.qCx += 9; // 0.9 um
    CHECK(a.matches(b));
    b.qCy += 4; // sqrt(81 + 16) < 10 quanta = 1e-3 mm
    CHECK(a.matches(b));
    b = a;
    b.qCz -= 11; // 1.1 um
    CHECK_FALSE(a.matches(b));

    b = a;
    b.qArea += 1; // a value that flipped at a rounding boundary
    CHECK(a.matches(b));
    b.qArea = a.qArea + 3; // 3 quanta on 1e6: more than 1e-7 relative and more than the 2-quantum floor
    CHECK_FALSE(a.matches(b));
    Fingerprint big = a;
    big.qArea = 10'000'000'000; // 1e6 mm^2: 1e-7 relative is 1000 quanta
    Fingerprint bigger = big;
    bigger.qArea += 900;
    CHECK(big.matches(bigger));
    bigger.qArea += 200;
    CHECK_FALSE(big.matches(bigger));

    b = a;
    b.qRadius += 11;
    CHECK_FALSE(a.matches(b));
    b = a;
    b.qAxisZ -= 11;
    CHECK_FALSE(a.matches(b));
    b = a;
    b.surface = kernel::SurfaceType::Cylinder;
    CHECK_FALSE(a.matches(b));
    b = a;
    b.neighbourCount = 5;
    CHECK_FALSE(a.matches(b));

    // Far-apart values do not overflow.
    b = a;
    b.qCx = std::numeric_limits<std::int64_t>::max() / 4;
    Fingerprint c = a;
    c.qCx = -(std::numeric_limits<std::int64_t>::max() / 4);
    CHECK_FALSE(b.matches(c));
}

TEST_CASE("Quantisation rounds to the 1e-4 mm grid and clamps", "[naming][fingerprint]")
{
    CHECK(detail::quantise(1.0, detail::kLengthQuantaPerMm) == 10'000);
    CHECK(detail::quantise(-2.5, detail::kLengthQuantaPerMm) == -25'000);
    CHECK(detail::quantise(0.00012, detail::kLengthQuantaPerMm) == 1);
    CHECK(detail::quantise(0.00016, detail::kLengthQuantaPerMm) == 2);
    CHECK(detail::quantise(std::numeric_limits<double>::quiet_NaN(), detail::kLengthQuantaPerMm) == 0);
    CHECK(detail::quantise(1e300, detail::kLengthQuantaPerMm) == (std::int64_t{1} << 60));
    CHECK(detail::quantise(-std::numeric_limits<double>::infinity(), detail::kLengthQuantaPerMm) ==
          -(std::int64_t{1} << 60));
    CHECK(detail::quantise(0.7071067811865476, detail::kAxisQuanta) == 707'107);
}

TEST_CASE("An empty ElementMap answers every query safely", "[naming][map]")
{
    const ElementMap empty;
    CHECK(empty.size() == 0);
    CHECK_FALSE(empty.find(Hash256{}).has_value());
    CHECK(empty.namesOf(kernel::SubId{kernel::SubKind::Face, 0}).empty());
    CHECK(empty.record(Hash256{}) == nullptr);
    CHECK(empty.family(Hash256{}).empty());
    CHECK(empty.fingerprintOf(kernel::SubId{kernel::SubKind::Face, 0}).surface == kernel::SurfaceType::Other);

    const std::vector<std::byte> bytes = empty.serialize();
    CHECK_FALSE(bytes.empty());
    const ElementMap back = take(ElementMap::deserialize(bytes));
    CHECK(back.size() == 0);
    CHECK(back.serialize() == bytes);
}

TEST_CASE("ElementMap lookups agree with each other", "[naming][map]")
{
    const std::array<Vec2, 2> holes{Vec2{20, 20}, Vec2{60, 30}};
    const kernel::Shape plate = take(kernel::testing::makePlateWithHoles(Vec3{100, 60, 10}, 5.0, holes));
    const FeatureId feature = test::featureId(1);
    const ElementMap map = nameImported(feature, plate);
    const auto elements = test::allElements(plate);
    REQUIRE(map.size() == elements.size());

    std::set<Hash256> seen;
    for (const kernel::SubId sub : elements) {
        const auto names = map.namesOf(sub);
        REQUIRE(names.size() == 1);
        CHECK(seen.insert(names[0]).second);
        REQUIRE(map.find(names[0]).has_value());
        CHECK(*map.find(names[0]) == sub);
        const TopoName* record = map.record(names[0]);
        REQUIRE(record != nullptr);
        CHECK(record->id == names[0]);
        CHECK(record->kind == sub.kind);
        CHECK(record->origin == feature);
        CHECK(record->id == detail::nameId(*record));
        CHECK(record->family == detail::familyId(record->kind, record->origin, record->role, record->parents));
        const std::string expectedRole = sub.kind == kernel::SubKind::Face   ? "import.face"
                                         : sub.kind == kernel::SubKind::Edge ? "import.edge"
                                                                             : "import.vertex";
        CHECK(record->role == expectedRole);
        const auto members = map.family(record->family);
        CHECK(std::find(members.begin(), members.end(), names[0]) != members.end());
        CHECK(members.size() == record->siblingCount);
        CHECK(std::is_sorted(members.begin(), members.end()));
        CHECK(static_cast<std::size_t>(map.fingerprintOf(sub).neighbourCount) ==
              kernel::adjacent(plate, sub, kernel::SubKind::Face).size());
    }
    CHECK_FALSE(map.find(Hash256{}).has_value());
    CHECK(map.namesOf(kernel::SubId{kernel::SubKind::Face, 9999}).empty());
    CHECK(map.namesOf(kernel::SubId{kernel::SubKind::Solid, 0}).empty());
}

TEST_CASE("ElementMap serialisation round-trips byte for byte", "[naming][map][serialize]")
{
    const ElementMap map = plateMap();
    const std::vector<std::byte> bytes = map.serialize();
    const ElementMap back = take(ElementMap::deserialize(bytes));
    CHECK(back.serialize() == bytes);
    CHECK(back.size() == map.size());
    for (const auto& element : map.impl->elements) {
        const auto names = back.namesOf(element.sub);
        REQUIRE(names.size() == element.names.size());
        CHECK(std::equal(names.begin(), names.end(), element.names.begin()));
        CHECK(back.fingerprintOf(element.sub) == element.fingerprint);
        for (const Hash256& name : element.names) {
            const TopoName* a = map.record(name);
            const TopoName* b = back.record(name);
            REQUIRE(a != nullptr);
            REQUIRE(b != nullptr);
            CHECK(a->role == b->role);
            CHECK(a->parents == b->parents);
            CHECK(a->discriminator == b->discriminator);
            CHECK(a->ordinal == b->ordinal);
            CHECK(a->siblingCount == b->siblingCount);
            CHECK(a->family == b->family);
            CHECK(back.find(name) == map.find(name));
        }
    }
    // Copies share the table and serialise identically.
    const ElementMap copy = back;
    CHECK(copy.serialize() == bytes);
}

TEST_CASE("ElementMap::deserialize rejects damaged bytes with an error", "[naming][map][serialize]")
{
    const std::vector<std::byte> good = plateMap().serialize();
    REQUIRE(good.size() > 200);

    SECTION("every truncation")
    {
        for (std::size_t n = 0; n < good.size(); ++n) {
            const auto result = ElementMap::deserialize(std::span<const std::byte>(good.data(), n));
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == ErrorCode::FormatError);
        }
    }
    SECTION("every single flipped byte")
    {
        for (std::size_t i = 0; i < good.size(); ++i) {
            std::vector<std::byte> bad = good;
            bad[i] ^= std::byte{0x5A};
            const auto result = ElementMap::deserialize(bad);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == ErrorCode::FormatError);
        }
    }
    SECTION("an extra byte")
    {
        std::vector<std::byte> bad = good;
        bad.push_back(std::byte{0});
        CHECK_FALSE(ElementMap::deserialize(bad).has_value());
        std::vector<std::byte> inside = good;
        inside.insert(inside.end() - 32, std::byte{0});
        const auto result = ElementMap::deserialize(reseal(inside));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::FormatError);
    }
    SECTION("well-sealed bytes that break the rules")
    {
        auto expectFormatError = [](const std::vector<std::byte>& bytes) {
            const auto result = ElementMap::deserialize(reseal(bytes));
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == ErrorCode::FormatError);
            CHECK_FALSE(result.error().message.empty());
        };
        constexpr std::size_t firstElement = 12; // magic, version, element count
        constexpr std::size_t firstName = firstElement + 1 + 4 + 1 + 64 + 2 + 4;
        std::vector<std::byte> bad = good;
        bad[firstElement] = std::byte{7}; // unknown kind
        expectFormatError(bad);
        bad = good;
        bad[firstName] ^= std::byte{1}; // a name with no record
        expectFormatError(bad);
        bad = good;
        bad[good.size() - 33] ^= std::byte{1}; // last byte of the last record's family
        expectFormatError(bad);
        bad = good;
        bad[firstElement + 1] = std::byte{0xFF}; // element index out of order
        bad[firstElement + 2] = std::byte{0xFF};
        expectFormatError(bad);
        bad = good;
        bad[8] = std::byte{0xFF}; // element count far larger than the bytes
        bad[9] = std::byte{0xFF};
        bad[10] = std::byte{0xFF};
        expectFormatError(bad);
        bad = good;
        bad[0] = std::byte{'X'};
        expectFormatError(bad);
    }
    SECTION("a newer version")
    {
        std::vector<std::byte> newer = good;
        newer[4] = std::byte{2};
        const auto result = ElementMap::deserialize(reseal(newer));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::FormatTooNew);
    }
    SECTION("garbage never throws")
    {
        std::vector<std::byte> noise(4096);
        std::uint32_t state = 12345;
        for (std::byte& b : noise) {
            state = state * 1664525u + 1013904223u;
            b = static_cast<std::byte>(state >> 24);
        }
        // A valid header in front of the noise, so the parser gets past the magic and version.
        const std::vector<std::byte> header(good.begin(), good.begin() + 8);
        for (std::size_t n = 0; n + 32 <= noise.size(); n += 97) {
            CHECK_NOTHROW(ElementMap::deserialize(std::span<const std::byte>(noise.data(), n)));
            std::vector<std::byte> sealed = header;
            sealed.insert(sealed.end(), noise.begin(), noise.begin() + static_cast<std::ptrdiff_t>(n + 32));
            Result<ElementMap> result = ElementMap{};
            CHECK_NOTHROW(result = ElementMap::deserialize(reseal(sealed)));
            CHECK_FALSE(result.has_value());
        }
    }
}
