// The drilling operation's checks (task T-035): validate refuses each problem on its own, in words a machinist reads,
// and generate refuses the same; what is only worth knowing becomes a toolpath warning.
#include "drill_test_util.h"

#include "cam/json.h"          // src/cam's JSON reader, to read the schema
#include "cam/ops/op_params.h" // the params reader

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

const kernel::Shape& block()
{
    static const kernel::Shape s = goldenBlock();
    return s;
}

Case on(std::vector<std::pair<double, double>> holes, const std::string& params, lib::Tool t)
{
    Case c;
    c.op = drillOp(params);
    c.tool = std::move(t);
    std::vector<kernel::SubId> faces;
    for (const auto& [x, y] : holes) {
        faces.push_back(holeFace(block(), x, y));
    }
    c.geom = geometryOn(block(), faces);
    return c;
}

// validate and generate both refuse `c` with `code` and a message containing `text`.
void refused(const Case& c, const std::string& text, ErrorCode code = ErrorCode::InvalidArgument)
{
    auto v = c.validate();
    REQUIRE_FALSE(v);
    INFO(v.error().message);
    CHECK(v.error().code == code);
    CHECK_THAT(v.error().message, ContainsSubstring(text));
    auto g = c.generate();
    REQUIRE_FALSE(g);
    CHECK(g.error().message == v.error().message);
}

void accepted(const Case& c)
{
    auto v = c.validate();
    if (!v) {
        FAIL(v.error().message);
    }
    (void)c.path();
}

bool warns(const Toolpath& p, const std::string& text)
{
    for (const std::string& w : p.warnings) {
        UNSCOPED_INFO("warning: " << w);
        if (w.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("drill: parameters are read against the schema", "[cam][drill][validate]")
{
    refused(on({{15, 15}}, R"({"pecks":3})", drill(6)), "Unknown parameter 'pecks'");
    refused(on({{15, 15}}, R"({"cycle":"peck","peck":-1})", drill(6)), "Peck depth ('peck') must be at least 0");
    refused(on({{15, 15}}, R"({"cycle":"helix"})", drill(6)), "Cycle ('cycle') must be one of 'spot', 'drill'");
    refused(on({{15, 15}}, R"({"dwell":"long"})", drill(6)), "Dwell ('dwell') must be a number, not text");
    refused(on({{15, 15}}, "[1]", drill(6)), "must be a JSON object");
    accepted(on({{15, 15}}, "", drill(6))); // no params: every default
}

TEST_CASE("drill: the selection must be faces of recognised holes", "[cam][drill][validate]")
{
    Case none = on({}, "{}", drill(6));
    refused(none, "Pick the holes to machine");

    // The block's top face is not part of a hole: the message says where it is.
    Case top = on({}, "{}", drill(6));
    const auto face = [&](auto pred) {
        std::optional<kernel::SubId> found;
        for (std::uint32_t i = 0; i < block().count(kernel::SubKind::Face) && !found; ++i) {
            auto s = kernel::surfaceInfo(block(), kernel::SubId{kernel::SubKind::Face, i});
            if (s && pred(*s)) {
                found = kernel::SubId{kernel::SubKind::Face, i};
            }
        }
        REQUIRE(found.has_value());
        return found.value_or(kernel::SubId{});
    };
    top.geom = geometryOn(block(), {face([](const kernel::SurfaceInfo& s) {
                              return s.type == kernel::SurfaceType::Plane && s.bounds.min.z > 19.99 &&
                                     s.bounds.max.x - s.bounds.min.x > 50;
                          })});
    refused(top, "Selection 1 (the face centred at X", ErrorCode::ToolpathFailed);
    refused(top, "is not part of a hole that can be machined from this setup", ErrorCode::ToolpathFailed);

    Case edge = on({}, "{}", drill(6));
    edge.geom = geometryOn(block(), {kernel::SubId{kernel::SubKind::Edge, 0}});
    refused(edge, "Selection 1 is not a face");

    // Two faces of one hole drill it once.
    Case twice = on({{15, 15}, {15, 15}}, "{}", drill(6));
    const Toolpath p = twice.path();
    CHECK(p.moves.size() == 3); // approach, one hole, cancel
}

TEST_CASE("drill: the tool must fit the cycle", "[cam][drill][validate]")
{
    refused(on({{15, 15}}, "{}", tool(lib::ToolType::FlatEndMill, 6, 0)),
            "The tool is a flat end mill; drilling needs a drill (or turn on 'Allow other tools')");
    accepted(on({{15, 15}}, R"({"allowOtherTools":true})", tool(lib::ToolType::FlatEndMill, 6, 0)));
    refused(on({{15, 15}}, R"({"allowOtherTools":true})", tap(6, 1)), "A tap can only tap");
    refused(on({{70, 12}}, R"({"cycle":"tap"})", drill(5)), "Tapping needs a tap; the tool is a drill");
    refused(on({{15, 15}}, R"({"cycle":"ream"})", drill(6.6)), "reaming needs a reamer");
    refused(on({{15, 15}}, R"({"cycle":"bore"})", tool(lib::ToolType::Reamer, 6.6, 0)), "boring needs a boring bar");
    accepted(on({{15, 15}}, R"({"cycle":"ream"})", tool(lib::ToolType::Reamer, 6.6, 0)));
    refused(on({{70, 12}}, R"({"cycle":"tap"})", tap(6, 0)), "The tap has no thread pitch");
    refused(on({{15, 15}}, R"({"cycle":"spot"})", tool(lib::ToolType::ChamferMill, 10, 0)),
            "Spot drilling to a diameter needs a tool with a point angle");
    lib::Tool noDiameter = drill(6);
    noDiameter.diameter = 0;
    refused(on({{15, 15}}, "{}", noDiameter), "The tool has no diameter");
}

TEST_CASE("drill: the tool must fit the hole", "[cam][drill][validate]")
{
    refused(on({{15, 15}}, "{}", drill(7)), "The 7 mm drill is larger than the 6.6 mm hole at X15 Y15");
    accepted(on({{15, 15}}, "{}", drill(6.6))); // the same size
    accepted(on({{15, 15}}, "{}", drill(3)));   // smaller is allowed
    refused(on({{15, 15}}, R"({"cycle":"spot","spotDiameter":12})", spotDrill(10)),
            "The 10 mm spot drill cannot make a 12 mm spot at the hole at X15 Y15");
    refused(on({{70, 12}}, R"({"cycle":"tap"})", tap(5, 0.8)),
            "The 5 mm hole at X70 Y12 is not smaller than the 5 mm tap");
    // Flutes and stick-out against the depth below the hole top: H1 goes 20 + 1 + 1.802582 = 22.802582 deep.
    refused(on({{15, 15}}, "{}", drill(6, 118, 20)),
            "At the hole at X15 Y15 the tool goes 22.803 mm deep, but its flutes are 20 mm long");
    Case shortStickOut = on({{15, 15}}, "{}", drill(6));
    shortStickOut.op.tool.stickOut = 23;
    refused(shortStickOut, "it sticks out only 23 mm: the holder needs 1 mm above the hole top");
    shortStickOut.op.tool.stickOut = 23.81;
    accepted(shortStickOut);
}

TEST_CASE("drill: heights, speeds and feeds are checked", "[cam][drill][validate]")
{
    Case c = on({{15, 15}}, "{}", drill(6));
    c.op.heights.top = 3;
    refused(c, "The retract height (R plane, 2 above the hole top) must not be below the top height (3)");
    c = on({{15, 15}}, "{}", drill(6));
    c.op.heights.clearance = 1;
    refused(c, "The clearance height (1) must not be below the retract height (2)");
    // H6 sits on the step at Z 14: a clearance of 2 above it is under the part's top (Z 20).
    c = on({{70, 38}}, "{}", drill(5));
    c.op.heights.clearance = 2;
    refused(c, "The clearance height ends at Z16, below the top of the stock, the part or a fixture (Z20)");
    // A hole top moved down past the tip.
    c = on({{15, 15}}, R"({"depthFrom":"value","depth":2,"depthReference":"tip"})", drill(6));
    c.op.heights.top = -5;
    refused(c, "the tool tip would end at Z18, not below the hole top Z15");

    c = on({{15, 15}}, "{}", drill(6));
    c.op.cutting.spindleRpm = 0;
    refused(c, "Set a spindle speed");
    c.op.cutting.spindleRpm = 9000;
    refused(c, "The spindle speed 9000 rpm is above the machine's maximum of 8100 rpm");
    c.op.cutting.spindleRpm = 100;
    c.machine.minRpm = 200;
    refused(c, "The spindle speed 100 rpm is below the machine's minimum of 200 rpm");
    c = on({{15, 15}}, "{}", drill(6));
    c.op.cutting.feedCut = 0;
    refused(c, "Set a cutting feed");
    c = on({{70, 12}}, R"({"cycle":"tap"})", tap(6, 1));
    c.op.cutting.spindleRpm = 500.5;
    refused(c, "Tapping needs a whole-number spindle speed");
}

TEST_CASE("drill: the holes must be within the machine's reach", "[cam][drill][validate]")
{
    Case c = on({{15, 15}, {35, 15}}, "{}", drill(6));
    c.machine.axes[0].maxTravel = 10; // X 0..10
    refused(c, "The holes span 20 mm in X; the machine's X travel is 10 mm", ErrorCode::ToolpathFailed);
    c = on({{15, 15}}, "{}", drill(6));
    c.machine.axes[2].minTravel = -30; // Z -30..0; clearance 35 to the tip at -2.802582
    refused(c, "From the clearance height to the deepest point is 37.803 mm; the machine's Z travel is 30 mm",
            ErrorCode::ToolpathFailed);
    c.machine.axes = {lib::AxisDef{'X', 0, 0, 0, 0, 0}}; // travel not known: not checked
    accepted(c);
}

TEST_CASE("drill: what is only worth knowing becomes a toolpath warning", "[cam][drill][validate]")
{
    // A tap on a hole the model gives no thread for (the kernel part has no hole features), and no rigid tapping.
    Case c = on({{70, 12}}, R"({"cycle":"tap"})", tap(6, 1));
    c.machine.rigidTapping = false;
    accepted(c);
    const Toolpath p = c.path();
    CHECK(warns(p, "The model gives no thread for the hole at X70 Y12"));
    CHECK(warns(p, "right-hand with the tap's 1 mm pitch"));
    CHECK(warns(p, "not set up for rigid tapping"));
    // A hole smaller than the thread's minor diameter: 6.35 - 1.0825317547 x 0.9071428571 = 5.368 mm.
    Case small = on({{70, 12}}, R"({"cycle":"tap"})", tap(6.35, 25.4 / 28));
    CHECK(warns(small.path(), "smaller than the thread's minor diameter (5.368 mm)"));
    // Left-hand for a hole without a thread spec comes from the parameter.
    Case left = on({{70, 12}}, R"({"cycle":"tap","leftHand":true})", tap(6, 1));
    const Toolpath lp = left.path();
    CHECK(lp.moves.at(1).cycleLeftHand == 1);
    CHECK(warns(lp, "left-hand"));
}

TEST_CASE("drill: the schema is complete enough for a generic form", "[cam][drill][schema]")
{
    // What the operation panel (T-045) reads: every property has a title, a description and a default; enums have
    // their labels; x-relevantWhen names real properties and values; x-propertyOrder lists each property once.
    auto schema = cam::json::parse(ops::makeDrillOperationType()->paramsSchemaJson());
    REQUIRE(schema);
    const cam::json::Value* props = schema->find("properties");
    REQUIRE(props != nullptr);
    std::vector<std::string> keys;
    for (const cam::json::Member& m : props->asObject()) {
        INFO(m.key);
        keys.push_back(m.key);
        for (const char* annotation : {"title", "description"}) {
            const cam::json::Value* v = m.value.find(annotation);
            REQUIRE(v != nullptr);
            CHECK(v->isString());
        }
        CHECK(m.value.find("default") != nullptr);
        if (const cam::json::Value* options = m.value.find("enum")) {
            const cam::json::Value* titles = m.value.find("x-enumTitles");
            REQUIRE(titles != nullptr);
            CHECK(titles->asArray().size() == options->asArray().size());
        } else {
            CHECK(m.value.find("type") != nullptr);
        }
        if (const cam::json::Value* when = m.value.find("x-relevantWhen")) {
            for (const cam::json::Member& w : when->asObject()) {
                const cam::json::Value* other = props->find(w.key);
                REQUIRE(other != nullptr);
                const cam::json::Value* options = other->find("enum");
                REQUIRE(options != nullptr);
                for (const cam::json::Value& v : w.value.asArray()) {
                    const auto& all = options->asArray();
                    CHECK(std::any_of(all.begin(), all.end(),
                                      [&](const cam::json::Value& o) { return o.asString() == v.asString(); }));
                }
            }
        }
    }
    const cam::json::Value* order = schema->find("x-propertyOrder");
    REQUIRE(order != nullptr);
    std::vector<std::string> ordered;
    for (const cam::json::Value& v : order->asArray()) {
        ordered.push_back(v.asString());
    }
    std::sort(ordered.begin(), ordered.end());
    CHECK(ordered == keys); // the members of a parsed object are sorted by key
    // The reader takes every default from it.
    auto loaded = ops::OpSchema::load(ops::makeDrillOperationType()->paramsSchemaJson());
    REQUIRE(loaded);
    auto defaults = (*loaded)->read("{}");
    REQUIRE(defaults);
    CHECK(defaults->text("cycle") == "drill");
    CHECK(defaults->number("breakthrough") == 1.0);
    CHECK(defaults->number("tapLeadPitches") == 2.0);
    CHECK_FALSE(defaults->given("cycle"));
    auto given = (*loaded)->read(R"({"cycle":"tap"})");
    REQUIRE(given);
    CHECK(given->given("cycle"));
}

TEST_CASE("drill: thread designations that name a left-hand thread", "[cam][drill]")
{
    CHECK(ops::isLeftHandThread("M8x1-LH"));
    CHECK(ops::isLeftHandThread("1/4-20 UNC-2B-LH"));
    CHECK(ops::isLeftHandThread("M10x1.5 LH"));
    CHECK(ops::isLeftHandThread("lh M6"));
    CHECK_FALSE(ops::isLeftHandThread("M6x1"));
    CHECK_FALSE(ops::isLeftHandThread("1/4-20 UNC"));
    CHECK_FALSE(ops::isLeftHandThread("M6 LHX"));
    CHECK_FALSE(ops::isLeftHandThread("ALH"));
}
