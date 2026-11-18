// Uuid, Id, Error and Result.
#include <catch2/catch_test_macros.hpp>

#include <ray/core.h>

#include <set>
#include <string>

TEST_CASE("Uuid generate makes random version 4 ids", "[uuid]")
{
    std::set<ray::Uuid> seen;
    for (int i = 0; i < 1000; ++i) {
        const ray::Uuid id = ray::Uuid::generate();
        CHECK((id.bytes[6] & 0xF0) == 0x40);
        CHECK((id.bytes[8] & 0xC0) == 0x80);
        seen.insert(id);
    }
    CHECK(seen.size() == 1000);
    CHECK(seen.count(ray::Uuid{}) == 0);
}

TEST_CASE("Uuid string round trip", "[uuid]")
{
    const ray::Uuid known = ray::Uuid::fromString("123e4567-e89b-12d3-a456-426614174000");
    const ray::Uuid expected{
        {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x12, 0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00}};
    CHECK(known == expected);
    CHECK(known.toString() == "123e4567-e89b-12d3-a456-426614174000");

    // Upper case is accepted; output is always lower case.
    CHECK(ray::Uuid::fromString("123E4567-E89B-12D3-A456-426614174000") == expected);

    for (int i = 0; i < 100; ++i) {
        const ray::Uuid id = ray::Uuid::generate();
        const std::string text = id.toString();
        CHECK(text.size() == 36);
        CHECK(ray::Uuid::fromString(text) == id);
    }
    CHECK(ray::Uuid{}.toString() == "00000000-0000-0000-0000-000000000000");
    CHECK(ray::Uuid::fromString("ffffffff-ffff-ffff-ffff-ffffffffffff").toString() ==
          "ffffffff-ffff-ffff-ffff-ffffffffffff");
}

TEST_CASE("Uuid fromString rejects malformed text with the nil Uuid", "[uuid]")
{
    const char* const bad[] = {
        "",
        "123e4567-e89b-12d3-a456-42661417400",   // too short
        "123e4567-e89b-12d3-a456-4266141740000", // too long
        "123e4567e89b12d3a456426614174000",      // no hyphens
        "123e4567-e89b-12d3-a456_426614174000",  // wrong separator
        "123e456-7e89b-12d3-a456-426614174000",  // hyphen in the wrong place
        "123e4567-e89b-12d3-a456-42661417400g",  // not hex
        "{23e4567-e89b-12d3-a456-42661417400}",  // braces
        " 23e4567-e89b-12d3-a456-426614174000",  // leading space
        "123e4567-e89b-12d3-a456-42661417400 ",  // trailing space
        "123e4567-e89b-12d3-a456-4266141740-0",  // extra hyphen
        "+23e4567-e89b-12d3-a456-426614174000",  // sign
    };
    for (const char* text : bad) {
        INFO(text);
        CHECK(ray::Uuid::fromString(text) == ray::Uuid{});
    }
    // An embedded NUL is not a hex digit.
    std::string withNul = "123e4567-e89b-12d3-a456-426614174000";
    withNul[34] = '\0';
    CHECK(ray::Uuid::fromString(withNul) == ray::Uuid{});
}

TEST_CASE("Id compares by its Uuid", "[uuid]")
{
    const ray::Uuid a = ray::Uuid::fromString("00000000-0000-0000-0000-000000000001");
    const ray::Uuid b = ray::Uuid::fromString("00000000-0000-0000-0000-000000000002");
    const ray::FeatureId fa{a};
    const ray::FeatureId fb{b};
    CHECK(fa < fb);
    CHECK(fa == ray::FeatureId{a});
    CHECK(fa != fb);
}

TEST_CASE("Result carries a value or an Error", "[result]")
{
    const ray::Result<int> ok = 3;
    REQUIRE(ok.has_value());
    CHECK(*ok == 3);
    const ray::Result<int> failed = tl::unexpected(ray::Error{ray::ErrorCode::NotFound, "missing", {"inner", "outer"}});
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == ray::ErrorCode::NotFound);
    CHECK(failed.error().context.size() == 2);
    const ray::Status status = std::monostate{};
    CHECK(status.has_value());
}
