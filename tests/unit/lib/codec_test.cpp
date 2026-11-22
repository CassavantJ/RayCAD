// The file format of library items, the compiled-in schemas and the schema validator.
#include "codec.h"
#include "json.h"
#include "lib_test_util.h"
#include "schema.h"

#include <ray/libraries.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace libtest;
using ray::ErrorCode;
namespace detail = ray::lib::detail;

TEST_CASE("toJson and fromJson round-trip every kind", "[lib][codec]")
{
    const auto tool = sampleTool(1, ray::lib::ToolType::Tap);
    auto t = ray::lib::toolFromJson(ray::lib::toJson(tool));
    REQUIRE(t);
    CHECK(ray::lib::toJson(*t) == ray::lib::toJson(tool));
    CHECK(t->threadSpec == "M6x1");
    REQUIRE(t->presets.size() == 2);
    CHECK(t->presets[1].opClass == "finish");

    auto h = ray::lib::holderFromJson(ray::lib::toJson(sampleHolder(2)));
    REQUIRE(h);
    CHECK(h->segments.size() == 2);
    auto m = ray::lib::materialFromJson(ray::lib::toJson(sampleMaterial(3)));
    REQUIRE(m);
    CHECK(m->densityGcm3 == 2.7);
    auto machine = ray::lib::machineFromJson(ray::lib::toJson(sampleMachine(4)));
    REQUIRE(machine);
    CHECK(machine->axes.size() == 3);
    CHECK(machine->axes[2].letter == 'Z');
    CHECK(machine->postPropertyOverrides.at("useG187") == "true");
    CHECK(machine->postPropertyOverrides.at("comment") == "\"x\"");
    CHECK_FALSE(machine->verified);
    auto tmpl = ray::lib::templateFromJson(ray::lib::toJson(sampleTemplate(5)));
    REQUIRE(tmpl);
    CHECK_FALSE(tmpl->material.has_value());
    CHECK(tmpl->toolQueryJson == R"({"threadSpec":"M6x1","type":"tap"})"); // canonical: sorted keys
}

TEST_CASE("toJson is canonical JSON", "[lib][codec]")
{
    const std::string text = ray::lib::toJson(sampleMaterial(3));
    CHECK(text == "{\"density\":2.7,\"group\":\"Aluminium\",\"hardnessHb\":95,\"id\":\"" + fixedId(3).toString() +
                      "\",\"name\":\"6061-T6\",\"schema\":1,\"specificCuttingForce\":700}");
}

TEST_CASE("Tool::contentHash follows the tool's content", "[lib][codec]")
{
    const auto a = sampleTool(1);
    auto b = sampleTool(1);
    CHECK(a.contentHash() == b.contentHash());
    b.diameter = 6.000001;
    CHECK(a.contentHash() != b.contentHash());
    b = sampleTool(1);
    b.presets[0].surfaceSpeedMMin = 251;
    CHECK(a.contentHash() != b.contentHash()); // cutting data is part of the content
    b = sampleTool(1);
    b.cornerRadius = -0.0; // -0 is written as 0
    CHECK(a.contentHash() == b.contentHash());
}

TEST_CASE("readers report the first problem and refuse newer formats", "[lib][codec]")
{
    auto bad = ray::lib::toolFromJson("{\"schema\":1}");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == ErrorCode::FormatError);
    CHECK(bad.error().message == "/id: is required");
    std::string newer = ray::lib::toJson(sampleTool(1));
    newer.replace(newer.find("\"schema\":1"), 10, "\"schema\":3");
    auto tooNew = ray::lib::toolFromJson(newer);
    REQUIRE_FALSE(tooNew);
    CHECK(tooNew.error().code == ErrorCode::FormatTooNew);
    auto tap = sampleTool(1, ray::lib::ToolType::Tap);
    tap.threadPitch = 0;
    auto noPitch = ray::lib::toolFromJson(ray::lib::toJson(tap));
    REQUIRE_FALSE(noPitch);
    CHECK(noPitch.error().message == "/threadPitch: a tap needs its thread pitch");
    auto machine = sampleMachine(4);
    machine.axes[1].letter = 'X';
    CHECK_FALSE(ray::lib::machineFromJson(ray::lib::toJson(machine)));
    machine = sampleMachine(4);
    machine.axes[0].letter = 'Q';
    auto badLetter = ray::lib::machineFromJson(ray::lib::toJson(machine));
    REQUIRE_FALSE(badLetter);
    CHECK(badLetter.error().message.rfind("/axes/0/letter: ", 0) == 0);
    auto preset = sampleTool(1);
    preset.presets[0].maxRadialFraction = 1.5;
    auto badPreset = ray::lib::toolFromJson(ray::lib::toJson(preset));
    REQUIRE_FALSE(badPreset);
    CHECK(badPreset.error().message == "/presets/0/maxRadialFraction: must be at most 1");
}

TEST_CASE("the compiled-in schemas are the files in schemas/", "[lib][codec]")
{
    for (const char* name : {"tool", "holder", "material", "machine", "optemplate"}) {
        INFO(name);
        const std::string onDisk = readFile(std::string(RAY_SOURCE_DIR) + "/schemas/" + name + ".schema.json");
        REQUIRE(!onDisk.empty());
        CHECK(detail::builtinSchemaText(name) == onDisk);
        CHECK(detail::Schema::load(onDisk)); // and they load (every keyword is one the validator implements)
    }
}

TEST_CASE("the validator refuses keywords it does not implement and catches planted errors", "[lib][codec]")
{
    auto unknown = detail::Schema::load(R"({"type":"object","propertyNames":{"pattern":"^a"}})");
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().message.find("propertyNames") != std::string::npos);
    CHECK_FALSE(detail::Schema::load(R"({"$ref":"https://example.com/x.json"})"));

    auto schema = detail::Schema::load(R"({
        "type": "object", "required": ["a"],
        "properties": {
            "a": {"type": "integer", "minimum": 1, "maximum": 3},
            "b": {"anyOf": [{"type": "null"}, {"$ref": "#/$defs/s"}]},
            "c": {"type": "array", "minItems": 1, "items": {"enum": ["x", "y"]}},
            "d": {"type": "object", "additionalProperties": false, "properties": {"e": {"const": true}}}
        },
        "$defs": {"s": {"type": "string", "pattern": "^[0-9]+$"}}
    })");
    REQUIRE(schema);
    const auto check = [&](const char* text) {
        auto value = ray::lib::json::parse(text);
        REQUIRE(value);
        return (*schema)->validate(*value);
    };
    CHECK(check(R"({"a":2,"b":null,"c":["x"],"d":{"e":true},"other":1})"));
    CHECK(check(R"({"a":2,"b":"123"})"));
    CHECK(check(R"({})").error().message == "/a: is required");
    CHECK(check(R"({"a":2.5})").error().message == "/a: must be an integer, not a number with a fraction");
    CHECK(check(R"({"a":4})").error().message == "/a: must be at most 3");
    CHECK(check(R"({"a":2,"b":"12x"})").error().message == "/b: \"12x\" does not match the pattern ^[0-9]+$");
    CHECK(check(R"({"a":2,"c":[]})").error().message == "/c: must have at least 1 items");
    CHECK(check(R"({"a":2,"c":["z"]})").error().message == "/c/0: must be one of \"x\", \"y\", not \"z\"");
    CHECK(check(R"({"a":2,"d":{"f":1}})").error().message == "/d/f: is not allowed here");
    CHECK(check(R"({"a":2,"d":{"e":false}})").error().message == "/d/e: must be true, not false");
}

TEST_CASE("unknown keys survive a merge, known keys come from the item", "[lib][codec]")
{
    auto raw = ray::lib::json::parse(R"({"comment":"keep me","name":"old","future":{"x":1}})");
    REQUIRE(raw);
    auto known = ray::lib::json::parse(R"({"name":"new","schema":1})");
    REQUIRE(known);
    const auto merged = detail::mergeUnknownKeys(*raw, *known);
    CHECK(ray::lib::json::write(merged) == R"({"comment":"keep me","future":{"x":1},"name":"new","schema":1})");
    CHECK(ray::lib::json::writePretty(merged) == "{\n  \"comment\": \"keep me\",\n  \"future\": {\n    \"x\": 1\n  "
                                                 "},\n  \"name\": \"new\",\n  \"schema\": 1\n}\n");
}
