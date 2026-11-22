// Helpers of the CAM state and engine tests (task T-032): a test-only operation type, library items as JSON (in the
// library's file format, which embedded items are: cam.h embeddedJson), and CAM command builders.
//
// The operation type "test.op" makes a toolpath of one rapid and one feed move to the heights of the operation, and
// counts its runs. Its params steer it: {"mode": "block"} waits until the gate opens (polling cancellation every
// millisecond), {"mode": "fail"} fails, {"mode": "invalid"} fails validate().
#pragma once

#include <ray/cam.h>
#include <ray/document.h>
#include <ray/kernel.h>
#include <ray/naming.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace ray::cam::test {

struct Gate {
    std::atomic<bool> open{true};
    std::atomic<int> waiting{0};
    std::atomic<int> runs{0};
    static Gate& instance()
    {
        static Gate gate;
        return gate;
    }
    void reset()
    {
        open = true;
        waiting = 0;
        runs = 0;
    }
};

class TestOperation final : public IOperationType {
public:
    std::string_view typeName() const override { return "test.op"; }
    std::string_view paramsSchemaJson() const override
    {
        return R"({"type":"object","properties":{"mode":{"enum":["plain","block","fail","invalid"],"default":"plain"}}})";
    }
    Status validate(const GenerateInput& in) const override
    {
        if (in.op.paramsJson.find("\"invalid\"") != std::string::npos) {
            return tl::unexpected(Error{ErrorCode::ToolpathFailed, "Test validation failed", {}});
        }
        return {};
    }
    Result<Toolpath> generate(const GenerateInput& in, JobContext& ctx) const override
    {
        Gate& gate = Gate::instance();
        ++gate.runs;
        if (in.op.paramsJson.find("\"block\"") != std::string::npos) {
            ++gate.waiting;
            while (!gate.open.load()) {
                if (auto st = ctx.cancel.check(); !st) {
                    --gate.waiting;
                    return tl::unexpected(st.error());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            --gate.waiting;
        }
        if (in.op.paramsJson.find("\"fail\"") != std::string::npos) {
            return tl::unexpected(Error{ErrorCode::ToolpathFailed, "Test generator failed", {}});
        }
        Toolpath tp{};
        Move rapid{};
        rapid.kind = MoveKind::Rapid;
        rapid.role = MoveRole::Approach;
        rapid.z = toNm(in.op.heights.clearance);
        Move feed{};
        feed.kind = MoveKind::Linear;
        feed.role = MoveRole::Cut;
        feed.z = toNm(in.op.heights.bottom);
        feed.feedMicronPerMin = static_cast<std::int32_t>(in.op.cutting.feedCut * 1000.0);
        tp.moves = {rapid, feed};
        tp.stats.cutLengthMm = in.tool.diameter; // something the tool decides
        tp.warnings.push_back("parts " + std::to_string(in.geom.partShapes.size()) + ", selected " +
                              std::to_string(in.geom.selected.size()));
        return tp;
    }
};

inline std::shared_ptr<const OperationRegistry> testOperations()
{
    auto registry = std::make_shared<OperationRegistry>();
    registry->add(std::make_unique<TestOperation>());
    return registry;
}

// Fixed ids: uuid('t', 1) = "t0000000-..." is not hex, so ids use hex kind letters: a (setups), c (ops), d (tools),
// e (holders), f (features), b (bodies), 9 (machines).
inline std::string id(char kind, std::uint64_t n)
{
    static const char* const hex = "0123456789abcdef";
    std::string tail(12, '0');
    for (int i = 11; i >= 0; --i) {
        tail[static_cast<std::size_t>(i)] = hex[n % 16];
        n /= 16;
    }
    return std::string(1, kind) + "0000000-0000-4000-8000-" + tail;
}

inline std::string toolJson(std::uint64_t n, double diameter = 6.0)
{
    return R"({"schema":1,"id":")" + id('d', n) + R"(","name":"Tool )" + std::to_string(n) +
           R"(","type":"flatEndMill","flutes":3,"diameter":)" + std::to_string(diameter) +
           R"(,"shankDiameter":6,"fluteLength":20,"shoulderLength":25,"overallLength":60,"defaultToolNumber":)" +
           std::to_string(n) + "}";
}

inline std::string holderJson(std::uint64_t n)
{
    return R"({"schema":1,"id":")" + id('e', n) + R"(","name":"Holder )" + std::to_string(n) +
           R"(","gaugeLength":40,"segments":[{"height":40,"lowerDiameter":30,"upperDiameter":40}]})";
}

inline std::string machineJson(std::uint64_t n, double maxRpm = 12000)
{
    return R"({"schema":1,"id":")" + id('9', n) + R"(","name":"Mill )" + std::to_string(n) +
           R"(","kind":"mill3","axes":[{"letter":"X","min":0,"max":500,"rapid":20000,"maxFeed":10000,"accel":3000}],)"
           R"("spindle":{"maxRpm":)" +
           std::to_string(maxRpm) +
           R"(,"powerKw":7.5},"toolChanger":{"capacity":20},"control":{"dialect":"generic-iso","defaultPost":"generic-iso","rigidTapping":true},)"
           R"("postProperties":{"useG187":true,"mode":"fast"}})";
}

inline std::string library(const std::string& tools, const std::string& holders = {}, const std::string& machines = {})
{
    std::string out = "{";
    const auto add = [&](const char* key, const std::string& items) {
        if (items.empty()) {
            return;
        }
        out += (out.size() > 1 ? "," : "") + std::string("\"") + key + "\":[" + items + "]";
    };
    add("tools", tools);
    add("holders", holders);
    add("machines", machines);
    return out + "}";
}

inline doc::Command command(std::string name, std::string args)
{
    return doc::Command{std::move(name), std::move(args), "test"};
}

// cam.setup.add of setup 'a' n on `parts` (comma-separated quoted ids), machine 1.
inline doc::Command setupAdd(std::uint64_t n, const std::string& parts, const std::string& extra = {})
{
    return command("cam.setup.add", R"({"id":")" + id('a', n) + R"(","name":"Setup )" + std::to_string(n) +
                                        R"(","partBodies":[)" + parts + R"(],"machine":")" + id('9', 1) + "\"" + extra +
                                        "}");
}

inline const char* kCutting = R"({"rpm":8000,"feedCut":1200,"feedPlunge":300})";
inline const char* kHeights = R"({"clearance":25,"retract":5,"feedPlane":2,"top":0,"bottom":-5})";

// cam.operation.add of operation 'c' n in setup 'a' s, tool 'd' tool.
// `toolExtra`: more members of the tool reference, e.g. R"(,"holder":"...")".
inline doc::Command opAdd(std::uint64_t n, std::uint64_t setup, std::uint64_t tool, const std::string& geometry = "[]",
                          const std::string& params = R"({"mode":"plain"})", const std::string& extra = {},
                          const std::string& toolExtra = {})
{
    return command("cam.operation.add", R"({"id":")" + id('c', n) + R"(","setup":")" + id('a', setup) +
                                            R"(","type":"test.op","name":"Op )" + std::to_string(n) +
                                            R"(","tool":{"tool":")" + id('d', tool) + "\"" + toolExtra +
                                            R"(},"cutting":)" + kCutting + R"(,"heights":)" + kHeights +
                                            R"(,"geometry":)" + geometry + R"(,"params":)" + params + extra + "}");
}

inline OperationId opId(std::uint64_t n)
{
    return OperationId{Uuid::fromString(id('c', n))};
}

inline BodyId body(char kind, std::uint64_t n = 1)
{
    return BodyId{Uuid::fromString(id(kind, n))};
}

// Applies a built-in command to a snapshot, or fails the test.
inline doc::DocStatePtr exec(const doc::DocStatePtr& state, const doc::Command& c)
{
    const doc::ICommandHandler* handler = doc::builtinCommandRegistry()->find(c.name);
    REQUIRE(handler != nullptr);
    auto next = handler->apply(state, c.argsJson);
    if (!next) {
        FAIL(c.name << " " << c.argsJson << " failed: " << next.error().message);
    }
    return *next;
}

inline Error applyFails(const doc::DocStatePtr& state, const doc::Command& c)
{
    const doc::ICommandHandler* handler = doc::builtinCommandRegistry()->find(c.name);
    REQUIRE(handler != nullptr);
    auto next = handler->apply(state, c.argsJson);
    if (next) {
        FAIL(c.name << " " << c.argsJson << " should have failed");
    }
    return next.error();
}

// The first face of `shape` whose surface passes `pred`.
template <class Pred>
kernel::SubId findFace(const kernel::Shape& shape, Pred pred)
{
    std::optional<kernel::SubId> found;
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face) && !found; ++i) {
        const kernel::SubId sub{kernel::SubKind::Face, i};
        auto info = kernel::surfaceInfo(shape, sub);
        if (info && pred(*info)) {
            found = sub;
        }
    }
    REQUIRE(found.has_value());
    return found.value_or(kernel::SubId{});
}

} // namespace ray::cam::test
