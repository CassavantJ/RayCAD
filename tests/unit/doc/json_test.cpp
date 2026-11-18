// Canonical JSON (src/doc/json.h and ray::doc::canonicalJson).
#include "json.h"

#include <ray/document.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <random>
#include <string>
#include <vector>

using ray::ErrorCode;
using ray::doc::canonicalJson;

namespace {

const char* const kSamples[] = {
    "null",
    "true",
    "[]",
    "{}",
    R"({"b":1,"a":[true,false,null],"c":{"y":"x","x":-0}})",
    R"( { "z" : [ 1 , 2.50 , -3e2 , { } , [ ] ] , "a" : "\u00e9\ud83d\ude00\/" } )",
    R"({"numbers":[0,-0,0.1,1e-7,1e21,123456789,1.5e300,5e-324]})",
    R"({"deep":{"a":{"b":{"c":{"d":[[[["x"]]]]}}}},"empty":""})",
    R"(["\b\f\n\r\t\"\\","\u0001\u001f\u007f",""])",
};

} // namespace

TEST_CASE("canonical JSON sorts keys and drops whitespace", "[doc][json]")
{
    CHECK(canonicalJson(R"({"b":1,"a":[true,false,null],"c":{"y":"x","x":-0}})") ==
          R"({"a":[true,false,null],"b":1,"c":{"x":0,"y":"x"}})");
    CHECK(canonicalJson(" \t\r\n[ 1 ,\n 2 ]\n") == "[1,2]");
    CHECK(canonicalJson("\xEF\xBB\xBF{}") == "{}"); // a leading byte order mark is ignored
    CHECK(canonicalJson("\"x\"") == "\"x\"");
    CHECK(canonicalJson("12") == "12");
}

TEST_CASE("canonical JSON is idempotent", "[doc][json]")
{
    for (const char* sample : kSamples) {
        INFO(sample);
        const std::string once = canonicalJson(sample);
        REQUIRE_FALSE(once.empty());
        CHECK(canonicalJson(once) == once);
    }
}

TEST_CASE("canonical JSON does not depend on key order", "[doc][json]")
{
    std::vector<std::string> members = {R"("width":"4 in")", R"("count":3)", R"("a":{"y":1,"x":2})", R"("Z":null)",
                                        R"("_":[1,{"q":0,"p":1}])"};
    std::sort(members.begin(), members.end());
    std::string expected;
    int permutations = 0;
    do {
        std::string text = "{";
        for (std::size_t i = 0; i < members.size(); ++i) {
            text += (i == 0 ? "" : " , ") + members[i];
        }
        text += "}";
        const std::string canonical = canonicalJson(text);
        if (expected.empty()) {
            expected = canonical;
        }
        CHECK(canonical == expected);
        ++permutations;
    } while (std::next_permutation(members.begin(), members.end()));
    CHECK(permutations == 120);
    CHECK(expected == R"({"Z":null,"_":[1,{"p":1,"q":0}],"a":{"x":2,"y":1},"count":3,"width":"4 in"})");
}

TEST_CASE("strings are escaped as RFC 8785 says", "[doc][json]")
{
    // Short escapes for the five named controls, \u00xx (lower-case hex) for the other controls, everything
    // else literally, including '/', DEL and non-ASCII.
    CHECK(canonicalJson(R"(["\u0008\u0009\u000a\u000c\u000d"])") == R"(["\b\t\n\f\r"])");
    CHECK(canonicalJson(R"(["\u0000\u001F\u000B"])") == R"(["\u0000\u001f\u000b"])");
    CHECK(canonicalJson(R"(["\"\\\/"])") == R"(["\"\\/"])");
    CHECK(canonicalJson(R"(["\u007f"])") == "[\"\x7f\"]");
    CHECK(canonicalJson(R"(["\u00e9\u20ac\ud83d\ude00"])") == "[\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"]");
    CHECK(canonicalJson("[\"\xC3\xA9\"]") == "[\"\xC3\xA9\"]");
}

TEST_CASE("keys sort by UTF-16 code units", "[doc][json]")
{
    // The example of RFC 8785 section 3.2.3.
    const std::string input =
        R"({"\u20ac":"Euro Sign","\r":"Carriage Return","\ufb33":"Hebrew Letter Dalet With Dagesh","1":"One",)"
        R"("\ud83d\ude00":"Emoji: Grinning Face","\u0080":"Control","\u00f6":"Latin Small Letter O With Diaeresis"})";
    const std::string expected = "{\"\\r\":\"Carriage Return\",\"1\":\"One\",\"\xC2\x80\":\"Control\","
                                 "\"\xC3\xB6\":\"Latin Small Letter O With Diaeresis\",\"\xE2\x82\xAC\":\"Euro Sign\","
                                 "\"\xF0\x9F\x98\x80\":\"Emoji: Grinning Face\","
                                 "\"\xEF\xAC\xB3\":\"Hebrew Letter Dalet With Dagesh\"}";
    CHECK(canonicalJson(input) == expected);
}

TEST_CASE("malformed JSON is a FormatError", "[doc][json]")
{
    const std::string deep(300, '[');
    const std::string deepClosed = deep + std::string(300, ']');
    const std::string cases[] = {
        "",
        " ",
        "{",
        "}",
        "[1,]",
        "[,1]",
        R"({"a":1,})",
        R"({"a" 1})",
        R"({a:1})",
        R"({"a":1 "b":2})",
        R"({"a":1,"a":2})",
        "01",
        "1.",
        ".5",
        "+1",
        "1e",
        "-",
        "NaN",
        "Infinity",
        "tru",
        "nul",
        "[1] 2",
        "[1e400]",
        R"("\x")",
        R"("\u12")",
        R"("\u12g4")",
        R"("\ud800")",
        R"("\udc00")",
        R"("\ud800\u0041")",
        "\"a\x01\"",
        "\"unterminated",
        "\"\xC0\xAF\"",         // overlong '/'
        "\"\xED\xA0\x80\"",     // encoded surrogate
        "\"\xF4\x90\x80\x80\"", // above U+10FFFF
        "\"\xE2\x82\"",         // truncated sequence
        "\"\x80\"",             // lone continuation byte
        "\xEF\xBB",             // truncated byte order mark
        deepClosed,
        std::string("[\"a\"\0]", 6), // NUL outside a string
    };
    for (const std::string& text : cases) {
        INFO(text);
        const auto parsed = ray::doc::json::parse(text);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == ErrorCode::FormatError);
        CHECK(parsed.error().message.find("Invalid JSON at byte") == 0);
        CHECK(canonicalJson(text).empty());
    }
    // 256 levels of nesting are fine.
    CHECK(canonicalJson(std::string(256, '[') + std::string(256, ']')) ==
          std::string(256, '[') + std::string(256, ']'));
}

TEST_CASE("values made in code are written canonically", "[doc][json]")
{
    using ray::doc::json::Value;
    Value::Object members;
    members.push_back({"b", Value::number(-0.0)});
    members.push_back({"a", Value::string("x\x01y\xFFz")}); // invalid UTF-8 is written as U+FFFD
    members.push_back({"b", Value::boolean(true)});         // duplicate key: the first one is kept
    members.push_back({"c", Value::number(std::numeric_limits<double>::infinity())});
    const Value value = Value::object(std::move(members));
    CHECK(ray::doc::json::write(value) == "{\"a\":\"x\\u0001y\xEF\xBF\xBDz\",\"b\":0,\"c\":null}");
    REQUIRE(value.find("a") != nullptr);
    CHECK(value.find("missing") == nullptr);
}

TEST_CASE("fuzzed JSON never crashes and errors are FormatError", "[doc][json]")
{
    const std::string base = canonicalJson(kSamples[5]) + canonicalJson(kSamples[4]);
    const std::string seed = R"({"a":[1,2.5,-3e-7,{"k":"\u00e9\ud83d\ude00"}],"b":{"c":null,"d":true,"e":"x\ny"}})";
    std::mt19937 rng(8u);
    auto check = [](const std::string& text) {
        const auto parsed = ray::doc::json::parse(text);
        if (parsed) {
            const std::string once = ray::doc::json::write(*parsed);
            CHECK(canonicalJson(once) == once);
        } else {
            CHECK(parsed.error().code == ErrorCode::FormatError);
        }
    };
    for (const std::string& text : {seed, base}) {
        for (std::size_t n = 0; n <= text.size(); ++n) {
            check(text.substr(0, n));
        }
        for (int i = 0; i < 4000; ++i) {
            std::string mutated = text;
            const int flips = 1 + static_cast<int>(rng() % 3);
            for (int k = 0; k < flips; ++k) {
                const std::size_t at = rng() % mutated.size();
                switch (rng() % 3) {
                case 0:
                    mutated[at] = static_cast<char>(mutated[at] ^ (1 << (rng() % 8)));
                    break;
                case 1:
                    mutated[at] = static_cast<char>(rng() % 256);
                    break;
                default:
                    mutated.erase(at, 1 + rng() % 4);
                    if (mutated.empty()) {
                        mutated = "x";
                    }
                    break;
                }
            }
            check(mutated);
        }
    }
}
