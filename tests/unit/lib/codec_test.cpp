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

TEST_CASE("geometryHash is the shape alone", "[lib][codec]")
{
    const auto a = sampleTool(1);
    auto b = sampleTool(1);
    CHECK(a.geometryHash() == b.geometryHash());
    // Not the shape: none of these changes it.
    b.id = ray::ToolId{fixedId(2)};
    b.name = "renamed";
    b.vendor = "Acme";
    b.productId = "X-1";
    b.comment = "from the catalogue";
    b.extraJson = R"({"coating":"TiAlN"})";
    b.presets[0].surfaceSpeedMMin = 251;
    b.presets.pop_back();
    b.bodyMaterial = "HSS";
    b.threadSpec = "M8x1.25";
    b.defaultToolNumber = 7;
    CHECK(a.geometryHash() == b.geometryHash());
    CHECK(a.contentHash() != b.contentHash());
    // The shape: each of these does.
    for (int field = 0; field < 11; ++field) {
        INFO("field " << field);
        auto c = sampleTool(1);
        switch (field) {
        case 0:
            c.type = ray::lib::ToolType::BallEndMill;
            break;
        case 1:
            c.flutes = 4;
            break;
        case 2:
            c.diameter += 0.001;
            break;
        case 3:
            c.shankDiameter += 0.001;
            break;
        case 4:
            c.fluteLength += 0.001;
            break;
        case 5:
            c.shoulderLength += 0.001;
            break;
        case 6:
            c.overallLength += 0.001;
            break;
        case 7:
            c.cornerRadius = 0.5;
            break;
        case 8:
            c.tipAngleDeg = 118;
            break;
        case 9:
            c.tipDiameter = 0.2;
            break;
        default:
            c.threadPitch = 1.5;
            break;
        }
        CHECK(c.geometryHash() != a.geometryHash());
    }

    const auto h = sampleHolder(1);
    auto g = sampleHolder(1);
    g.name = "renamed";
    g.vendor = "Acme";
    g.comment = "measured";
    g.id = ray::HolderId{fixedId(2)};
    CHECK(h.geometryHash() == g.geometryHash());
    g.segments[1].upperDiameter += 0.01;
    CHECK(h.geometryHash() != g.geometryHash());
    g = sampleHolder(1);
    g.gaugeLength += 0.01;
    CHECK(h.geometryHash() != g.geometryHash());
}

TEST_CASE("comments and unknown keys round-trip, at any depth of nested objects", "[lib][codec]")
{
    auto tool = sampleTool(1);
    tool.comment = "Generic 6 mm end mill";
    tool.presets[0].comment = "Starting values";
    tool.extraJson = R"({"coating":"TiAlN","future":{"x":[1,2]}})";
    const std::string text = ray::lib::toJson(tool);
    CHECK(text.find(R"("comment":"Generic 6 mm end mill")") != std::string::npos);
    CHECK(text.find(R"("comment":"Starting values")") != std::string::npos);
    CHECK(text.find(R"("coating":"TiAlN")") != std::string::npos);
    auto back = ray::lib::toolFromJson(text);
    REQUIRE(back);
    CHECK(back->comment == "Generic 6 mm end mill");
    CHECK(back->presets[0].comment == "Starting values");
    CHECK(back->presets[1].comment.empty());
    CHECK(back->extraJson == R"({"coating":"TiAlN","future":{"x":[1,2]}})");
    CHECK(ray::lib::toJson(*back) == text);
    // Without a comment there is no "comment" key, so an item without one is written as before.
    CHECK(ray::lib::toJson(sampleTool(1)).find("comment") == std::string::npos);

    // A machine with unknown keys inside its nested objects.
    std::string machine = ray::lib::toJson(sampleMachine(4));
    machine.replace(machine.find("\"maxRpm\""), 0, "\"gearRanges\":2,");
    machine.replace(machine.find("\"model\""), 0, "\"comment\":\"Published values\",\"futureTop\":true,");
    auto m = ray::lib::machineFromJson(machine);
    REQUIRE(m);
    CHECK(m->comment == "Published values");
    CHECK(m->extraJson == R"({"futureTop":true,"spindle":{"gearRanges":2}})");
    auto parsed = ray::lib::json::parse(machine);
    REQUIRE(parsed);
    CHECK(ray::lib::toJson(*m) == ray::lib::json::write(*parsed));
    // A known key in extraJson does not override the field.
    m->extraJson = R"({"name":"other","x":1})";
    CHECK(ray::lib::machineFromJson(ray::lib::toJson(*m))->name == "Machine 4");

    auto material = sampleMaterial(3);
    material.comment = "ASM handbook";
    CHECK(ray::lib::materialFromJson(ray::lib::toJson(material))->comment == "ASM handbook");
    auto holder = sampleHolder(2);
    holder.comment = "measured";
    CHECK(ray::lib::holderFromJson(ray::lib::toJson(holder))->comment == "measured");
    auto tmpl = sampleTemplate(5);
    tmpl.comment = "shop standard";
    tmpl.extraJson = R"({"owner":"jo"})";
    auto t = ray::lib::templateFromJson(ray::lib::toJson(tmpl));
    REQUIRE(t);
    CHECK(t->comment == "shop standard");
    CHECK(t->extraJson == R"({"owner":"jo"})");
}

TEST_CASE("default-constructed items hold their defaults", "[lib][codec]")
{
    // Default-initialised (not value-initialised): without member defaults these would be indeterminate.
    ray::lib::Tool tool;
    CHECK(tool.type == ray::lib::ToolType::FlatEndMill);
    CHECK(tool.flutes == 0);
    CHECK(tool.diameter == 0);
    CHECK(tool.shankDiameter == 0);
    CHECK(tool.fluteLength == 0);
    CHECK(tool.shoulderLength == 0);
    CHECK(tool.overallLength == 0);
    ray::lib::ToolPreset preset;
    CHECK(preset.surfaceSpeedMMin == 0);
    CHECK(preset.chipLoadMm == 0);
    CHECK(preset.rampAngleDeg == 0);
    ray::lib::Holder holder;
    CHECK(holder.gaugeLength == 0);
    ray::lib::HolderSegment segment;
    CHECK(segment.height == 0);
    ray::lib::Material material;
    CHECK(material.hardnessHb == 0);
    CHECK(material.densityGcm3 == 0);
    ray::lib::AxisDef axis;
    CHECK(axis.letter == 'X');
    CHECK(axis.maxTravel == 0);
    ray::lib::MachineDef machine;
    CHECK(machine.kind == ray::lib::MachineDef::Kind::Mill3Axis);
    CHECK(machine.maxRpm == 0);
    CHECK(machine.toolCapacity == 0);
    CHECK_FALSE(machine.rigidTapping);
    CHECK_FALSE(machine.hasProbe);
    CHECK(machine.maxBlockNumber == 0);
    ray::lib::ShopRule rule;
    CHECK(rule.priority == 0);
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
