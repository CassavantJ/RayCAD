// ray::Hasher and Hash256: official BLAKE3 test vectors and the typed field encodings.
#include <catch2/catch_test_macros.hpp>

#include <ray/core.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

// The input of the official vectors: bytes 0, 1, ..., 250, 0, 1, ... repeated.
std::vector<std::byte> vectorInput(std::size_t length)
{
    std::vector<std::byte> input(length);
    for (std::size_t i = 0; i < length; ++i) {
        input[i] = static_cast<std::byte>(i % 251);
    }
    return input;
}

ray::Hash256 hashOf(std::span<const std::byte> data)
{
    return ray::Hasher().bytes(data).finish();
}

template <std::size_t N>
std::span<const std::byte> asBytes(const std::uint8_t (&values)[N])
{
    return std::as_bytes(std::span<const std::uint8_t>(values, N));
}

} // namespace

TEST_CASE("BLAKE3 official test vectors", "[hash]")
{
    // From test_vectors/test_vectors.json of the BLAKE3 reference repository: input length and the first
    // 32 bytes of the "hash" output.
    const std::pair<std::size_t, const char*> vectors[] = {
        {0, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
        {1, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
        {63, "e9bc37a594daad83be9470df7f7b3798297c3d834ce80ba85d6e207627b7db7b"},
        {64, "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98"},
        {65, "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee"},
        {1023, "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
        {1024, "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
        {1025, "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
        {2049, "5f4d72f40d7a5f82b15ca2b2e44b1de3c2ef86c426c95c1af0b6879522563030"},
        {8193, "bab6c09cb8ce8cf459261398d2e7aef35700bf488116ceb94a36d0f5f1b7bc3b"},
        {31744, "62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47"},
        {102400, "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085"},
    };
    for (const auto& [length, expected] : vectors) {
        INFO("input length " << length);
        const std::vector<std::byte> input = vectorInput(length);
        CHECK(hashOf(input).hex() == expected);

        // The same input fed in uneven pieces gives the same hash.
        ray::Hasher pieces;
        std::size_t at = 0;
        for (std::size_t step = 1; at < length; step = step * 3 + 1) {
            const std::size_t n = std::min(step, length - at);
            pieces.bytes(std::span<const std::byte>(input).subspan(at, n));
            at += n;
        }
        CHECK(pieces.finish().hex() == expected);
    }
}

TEST_CASE("Hash256 hex and comparison", "[hash]")
{
    ray::Hash256 zero;
    CHECK(zero.hex() == std::string(64, '0'));
    ray::Hash256 h;
    h.bytes[0] = 0xAB;
    h.bytes[31] = 0x0F;
    CHECK(h.hex() == "ab" + std::string(60, '0') + "0f");
    CHECK(zero < h);
    CHECK(h == h);
}

TEST_CASE("Hasher field encodings", "[hash]")
{
    SECTION("f64 hashes the little-endian bit pattern and normalises -0.0")
    {
        const std::uint8_t one[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F}; // 1.0
        CHECK(ray::Hasher().f64(1.0).finish() == hashOf(asBytes(one)));
        CHECK(ray::Hasher().f64(-0.0).finish() == ray::Hasher().f64(0.0).finish());
        const std::uint8_t zero[8] = {};
        CHECK(ray::Hasher().f64(-0.0).finish() == hashOf(asBytes(zero)));
        CHECK(ray::Hasher().f64(1.0).finish() != ray::Hasher().f64(-1.0).finish());
    }
    SECTION("i64 hashes 8 little-endian bytes")
    {
        const std::uint8_t le[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
        CHECK(ray::Hasher().i64(0x0102030405060708).finish() == hashOf(asBytes(le)));
        const std::uint8_t minusOne[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        CHECK(ray::Hasher().i64(-1).finish() == hashOf(asBytes(minusOne)));
    }
    SECTION("str hashes an 8-byte length, then the bytes")
    {
        const std::uint8_t ab[] = {0x02, 0, 0, 0, 0, 0, 0, 0, 'a', 'b'};
        CHECK(ray::Hasher().str("ab").finish() == hashOf(asBytes(ab)));
        CHECK(ray::Hasher().str("ab").str("c").finish() != ray::Hasher().str("a").str("bc").finish());
        CHECK(ray::Hasher().str("").finish() != ray::Hasher().finish());
    }
}

TEST_CASE("Hasher finish, chaining and moves", "[hash]")
{
    ray::Hasher h;
    h.str("part one");
    const ray::Hash256 first = h.finish();
    CHECK(h.finish() == first); // finish does not reset
    h.i64(42);
    CHECK(h.finish() != first);
    CHECK(h.finish() == ray::Hasher().str("part one").i64(42).finish());

    ray::Hasher moved = std::move(h);
    CHECK(moved.finish() == ray::Hasher().str("part one").i64(42).finish());
    // A moved-from Hasher starts again from empty input.
    CHECK(h.finish() == ray::Hasher().finish()); // NOLINT(bugprone-use-after-move)

    ray::Hasher assigned;
    assigned = std::move(moved);
    CHECK(assigned.finish() == ray::Hasher().str("part one").i64(42).finish());
}
