// LibraryStack: layered roots, tool queries and template lookup.
#include "lib_test_util.h"
#include "library_internal.h"
#include "test_dispatcher.h"
#include "tool_query.h"

#include <ray/libraries.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <random>
#include <string>

using namespace libtest;
using ray::lib::LibraryStack;
using ray::lib::ToolType;

namespace {

void saveAll(const std::string& root, const std::vector<ray::lib::Tool>& tools)
{
    auto library = ray::lib::ILibrary::open(root);
    REQUIRE(library);
    for (const auto& t : tools) {
        REQUIRE((*library)->save(t));
    }
}

std::vector<std::string> names(const std::vector<ray::lib::Tool>& tools)
{
    std::vector<std::string> out;
    for (const auto& t : tools) {
        out.push_back(t.name);
    }
    return out;
}

// The names a query finds; the test fails if the query is malformed.
std::vector<std::string> names(const ray::Result<std::vector<ray::lib::Tool>>& found)
{
    if (!found) {
        FAIL("the query failed: " << found.error().message);
        return {};
    }
    return names(*found);
}

// The message of a query that must fail.
std::string failure(const ray::Result<std::vector<ray::lib::Tool>>& found)
{
    REQUIRE_FALSE(found);
    CHECK(found.error().code == ray::ErrorCode::InvalidArgument);
    return found.error().message;
}

} // namespace

TEST_CASE("later roots override earlier ones by id", "[lib][stack]")
{
    TempDir builtin;
    TempDir shop;
    TempDir user;
    auto a = sampleTool(1);
    auto b = sampleTool(2);
    auto c = sampleTool(3);
    saveAll(builtin.dir(), {a, b, c});
    b.name = "shop's tool 2";
    c.name = "shop's tool 3";
    saveAll(shop.dir(), {b, c});
    c.name = "user's tool 3";
    saveAll(user.dir(), {c});

    LibraryStack stack;
    stack.setRoots({builtin.dir(), shop.dir(), user.dir()});
    CHECK(stack.issues().empty());
    REQUIRE(stack.tool(a.id));
    CHECK(stack.tool(a.id)->name == "Tool 1");
    CHECK(stack.tool(b.id)->name == "shop's tool 2");
    CHECK(stack.tool(c.id)->name == "user's tool 3");
    CHECK(stack.tool(ray::ToolId{fixedId(99)}) == nullptr);

    // The other order: the built-in library now wins.
    stack.setRoots({user.dir(), shop.dir(), builtin.dir()});
    CHECK(stack.tool(c.id)->name == "Tool 3");

    // Holders, materials and machines layer the same way.
    {
        auto lower = ray::lib::ILibrary::open(builtin.dir());
        auto upper = ray::lib::ILibrary::open(user.dir());
        REQUIRE(lower);
        REQUIRE(upper);
        auto m = sampleMachine(4);
        REQUIRE((*lower)->save(m));
        m.name = "user machine";
        REQUIRE((*upper)->save(m));
        REQUIRE((*lower)->save(sampleMaterial(5)));
        REQUIRE((*upper)->save(sampleHolder(6)));
    }
    stack.setRoots({builtin.dir(), shop.dir(), user.dir()});
    CHECK(stack.machine(ray::MachineId{fixedId(4)})->name == "user machine");
    CHECK(stack.material(ray::MaterialId{fixedId(5)}) != nullptr);
    CHECK(stack.holder(ray::HolderId{fixedId(6)}) != nullptr);
}

TEST_CASE("a root that cannot be opened is an issue, and refresh picks it up when it appears", "[lib][stack]")
{
    TempDir builtin;
    TempDir parent;
    saveAll(builtin.dir(), {sampleTool(1)});
    const std::string shop = parent.path("shop share");
    LibraryStack stack;
    stack.setRoots({builtin.dir(), shop});
    REQUIRE(stack.issues().size() == 1);
    CHECK(stack.issues()[0].file == shop);
    CHECK(stack.tool(ray::ToolId{fixedId(1)}) != nullptr);

    auto t = sampleTool(1);
    t.name = "from the share";
    writeFile(parent.path("shop share/tools/x.tool.json"), ray::lib::toJson(t));
    CHECK(stack.refresh());
    CHECK(stack.issues().empty());
    CHECK(stack.tool(ray::ToolId{fixedId(1)})->name == "from the share");
}

TEST_CASE("queryTools filters and sorts by (diameter, id), whatever the order of the files", "[lib][stack]")
{
    std::vector<ray::lib::Tool> tools;
    tools.push_back(sampleTool(1, ToolType::FlatEndMill, 6));
    tools.push_back(sampleTool(2, ToolType::FlatEndMill, 10));
    tools.push_back(sampleTool(3, ToolType::FlatEndMill, 6)); // same diameter as 1: id breaks the tie
    tools.push_back(sampleTool(4, ToolType::BallEndMill, 6));
    tools.push_back(sampleTool(5, ToolType::FlatEndMill, 3));
    tools.push_back(sampleTool(6, ToolType::Tap, 6));
    tools.back().presets.clear();
    tools.push_back(sampleTool(7, ToolType::Drill, 5));
    tools.back().bodyMaterial = "HSS";
    for (auto& t : tools) {
        t.name = "T" + std::to_string(&t - tools.data() + 1);
    }

    std::mt19937_64 rng(7);
    std::vector<std::vector<std::string>> results;
    for (int round = 0; round < 3; ++round) {
        std::shuffle(tools.begin(), tools.end(), rng); // a different write order each round
        TempDir dir;
        saveAll(dir.dir(), tools);
        LibraryStack stack;
        stack.setRoots({dir.dir()});
        results.push_back(names(stack.queryTools(R"({"type":"flatEndMill"})", {})));
    }
    CHECK(results[0] == std::vector<std::string>{"T5", "T1", "T3", "T2"});
    CHECK(results[1] == results[0]);
    CHECK(results[2] == results[0]);

    TempDir dir;
    saveAll(dir.dir(), tools);
    LibraryStack stack;
    stack.setRoots({dir.dir()});
    const ray::lib::QueryVars vars = {
        {"feature.minWidth", 12.0}, {"feature.diameter", 6.0}, {"feature.threadSpec", std::string("M6 x 1")}};
    CHECK(names(stack.queryTools(R"({"type":["FlatEndMill","ballEndMill"],"diameter":{"max":"0.6*feature.minWidth"}})",
                                 vars)) == std::vector<std::string>{"T5", "T1", "T3", "T4"});
    CHECK(names(stack.queryTools(R"({"diameter":"$feature.diameter","type":"tap"})", vars)) ==
          std::vector<std::string>{"T6"});
    CHECK(names(stack.queryTools(R"({"diameter":{"min":5,"max":6},"bodyMaterial":"hss"})", vars)) ==
          std::vector<std::string>{"T7"});
    CHECK(names(stack.queryTools(R"({"threadSpec":"m6 x1"})", vars)) == std::vector<std::string>{"T6"});
    CHECK(names(stack.queryTools(R"({"opClass":"finish","diameter":{"min":7}})", vars)) ==
          std::vector<std::string>{"T2"});
    CHECK(names(stack.queryTools(R"({"material":")" + fixedId(900).toString() + R"(","type":"tap"})", vars)).empty());
    CHECK(names(stack.queryTools("{}", vars)).size() == 7);
    // A string variable (the plan's own template example, PLAN.md 3.6), and the canonical and bare spellings.
    CHECK(names(stack.queryTools(R"({"type":"tap","threadSpec":"$feature.threadSpec"})", vars)) ==
          std::vector<std::string>{"T6"});
    CHECK(names(stack.queryTools(R"({"diameter":{"max":"0.5 * $feature.minWidth"}})", vars)) ==
          names(stack.queryTools(R"({"diameter":{"max":"0.5 * feature.minWidth"}})", vars)));
    // Without the '$' a string key is a literal: no tool's thread is called "feature.threadSpec".
    CHECK(names(stack.queryTools(R"({"threadSpec":"feature.threadSpec"})", vars)).empty());
    // Malformed queries are errors that say what is wrong, not silent empty results.
    CHECK(failure(stack.queryTools(R"({"colour":"red"})", vars)).find("colour") != std::string::npos);
    CHECK(failure(stack.queryTools(R"({"diameter":"feature.unknown"})", vars))
              .find("unknown variable $feature.unknown") != std::string::npos);
    CHECK(failure(stack.queryTools(R"({"threadSpec":"$feature.nothing"})", vars))
              .find("unknown variable $feature.nothing") != std::string::npos);
    CHECK(failure(stack.queryTools(R"({"diameter":"$feature.threadSpec"})", vars)).find("is a string, not a number") !=
          std::string::npos);
    CHECK(failure(stack.queryTools(R"({"threadSpec":"$feature.diameter"})", vars)).find("is a number, not a string") !=
          std::string::npos);
    CHECK(failure(stack.queryTools("[", vars)).find("not valid JSON") != std::string::npos);
}

TEST_CASE("expressions", "[lib][stack]")
{
    using ray::lib::detail::evaluate;
    const ray::lib::QueryVars vars = {{"feature.threadDepth", 12.0}, {"tool.threadPitch", 1.25}, {"w", 4.0}};
    CHECK(*evaluate("$feature.threadDepth + 2 * $tool.threadPitch", vars) == 14.5);
    CHECK(*evaluate("0.6*w", vars) == 0.6 * 4);
    CHECK(*evaluate("-(w - 1) / 2", vars) == -1.5);
    CHECK(*evaluate(" 1 - 2 - 3 ", vars) == -4); // left to right
    CHECK(*evaluate("2e1 + 0.5", vars) == 20.5);
    CHECK_FALSE(evaluate("w / 0", vars));
    CHECK_FALSE(evaluate("w +", vars));
    CHECK_FALSE(evaluate("(w", vars));
    CHECK_FALSE(evaluate("w w", vars));
    CHECK_FALSE(evaluate("nope", vars));
    CHECK_FALSE(evaluate(".5", vars));
}

TEST_CASE("templatesFor picks matching templates, the most specific first", "[lib][stack]")
{
    TempDir dir;
    const ray::MaterialId aluminium{fixedId(50)};
    const ray::MaterialId steel{fixedId(51)};
    const ray::MachineId haas{fixedId(60)};
    {
        auto library = ray::lib::ILibrary::open(dir.dir());
        REQUIRE(library);
        auto any = sampleTemplate(1);
        any.name = "b any";
        auto alu = sampleTemplate(2);
        alu.name = "c aluminium";
        alu.material = aluminium;
        auto aluHaas = sampleTemplate(3);
        aluHaas.name = "d aluminium on the Haas";
        aluHaas.material = aluminium;
        aluHaas.machine = haas;
        auto steelOnly = sampleTemplate(4);
        steelOnly.material = steel;
        auto otherKind = sampleTemplate(5);
        otherKind.featureKind = "pocket.closed";
        auto any2 = sampleTemplate(6);
        any2.name = "a any";
        for (const auto& t : {any, alu, aluHaas, steelOnly, otherKind, any2}) {
            REQUIRE((*library)->save(t));
        }
    }
    LibraryStack stack;
    stack.setRoots({dir.dir()});
    const auto found = stack.templatesFor("hole.tapped", aluminium, haas);
    std::vector<std::string> got;
    for (const auto& t : found) {
        got.push_back(t.name);
    }
    CHECK(got == std::vector<std::string>{"d aluminium on the Haas", "c aluminium", "a any", "b any"});
    CHECK(stack.templatesFor("hole.tapped", steel, ray::MachineId{}).size() == 3);
    CHECK(stack.templatesFor("", steel, ray::MachineId{}).size() == 4);
}

TEST_CASE("roots say where items come from, which are writable, and their names", "[lib][stack]")
{
    TempDir builtin;
    TempDir shop;
    TempDir user;
    writeFile(builtin.path("library.json"), R"({"schema":1,"name":"Built-in"})");
    saveAll(builtin.dir(), {sampleTool(1), sampleTool(2)});
    {
        auto library = ray::lib::ILibrary::open(builtin.dir());
        REQUIRE(library);
        REQUIRE((*library)->save(sampleHolder(10)));
        REQUIRE((*library)->save(sampleMaterial(11)));
        REQUIRE((*library)->save(sampleMachine(12)));
        REQUIRE((*library)->save(sampleTemplate(13)));
    }
    auto mine = sampleTool(2);
    mine.name = "my tool 2";
    saveAll(user.dir(), {mine});
    TempDir parent;
    const std::string missing = parent.path("gone");

    LibraryStack stack;
    stack.setRoots({builtin.dir(), shop.dir(), missing, user.dir()}, 1);
    const auto roots = stack.roots();
    REQUIRE(roots.size() == 4);
    CHECK(roots[0].name == "Built-in");
    CHECK(roots[0].open);
    CHECK_FALSE(roots[0].writable); // the built-in library is read-only
    CHECK(roots[1].writable);
    CHECK_FALSE(roots[2].open);
    CHECK_FALSE(roots[2].writable);
    CHECK(roots[2].name == "gone"); // no library.json: the folder's name
    CHECK(roots[3].writable);
    CHECK(stack.library(0) == nullptr);
    CHECK(stack.library(1) != nullptr);
    CHECK(stack.library(2) == nullptr);
    CHECK(stack.library(9) == nullptr);

    using ray::lib::ItemKind;
    CHECK(stack.rootOf(ItemKind::Tool, fixedId(1)) == std::optional<std::size_t>(0));
    CHECK(stack.rootOf(ItemKind::Tool, fixedId(2)) == std::optional<std::size_t>(3)); // the user's copy wins
    CHECK(stack.rootOf(ItemKind::Holder, fixedId(10)) == std::optional<std::size_t>(0));
    CHECK(stack.rootOf(ItemKind::Material, fixedId(11)) == std::optional<std::size_t>(0));
    CHECK(stack.rootOf(ItemKind::Machine, fixedId(12)) == std::optional<std::size_t>(0));
    CHECK(stack.rootOf(ItemKind::Template, fixedId(13)) == std::optional<std::size_t>(0));
    CHECK_FALSE(stack.rootOf(ItemKind::Tool, fixedId(10)).has_value());

    // Merged lists of every kind, sorted by id.
    REQUIRE(stack.tools().size() == 2);
    CHECK(stack.tools()[0].id.value == fixedId(1));
    CHECK(stack.tools()[1].name == "my tool 2");
    CHECK(stack.holders().size() == 1);
    CHECK(stack.materials().size() == 1);
    CHECK(stack.machines().size() == 1);
    REQUIRE(stack.templates().size() == 1);
    REQUIRE(stack.opTemplate(fixedId(13)) != nullptr);
    CHECK(stack.opTemplate(fixedId(13))->name == "Template 13");

    // Saving into a writable root's library shows after remerge, which also calls the listeners.
    int calls = 0;
    const int listener = stack.subscribe([&calls] { ++calls; });
    CHECK(listener != 0);
    CHECK(stack.subscribe({}) == 0);
    auto shopTool = sampleTool(3);
    REQUIRE(stack.library(1)->save(shopTool));
    CHECK(stack.tool(shopTool.id) == nullptr);
    stack.remerge();
    CHECK(calls == 1);
    CHECK(stack.rootOf(ItemKind::Tool, fixedId(3)) == std::optional<std::size_t>(1));
    REQUIRE(stack.library(1)->remove(ItemKind::Tool, fixedId(3)));
    stack.remerge();
    CHECK(calls == 2);
    CHECK(stack.tool(shopTool.id) == nullptr);
    stack.unsubscribe(listener);
    stack.remerge();
    CHECK(calls == 2);
}

TEST_CASE("one watch over the whole stack merges a change on the UI thread and calls the listeners", "[lib][stack]")
{
    ray::lib::detail::setWatchIntervalMs(20);
    TempDir builtin;
    TempDir user;
    saveAll(builtin.dir(), {sampleTool(1)});
    ray::test::TestDispatcher dispatcher;
    {
        LibraryStack stack;
        stack.watch(dispatcher);
        stack.setRoots({builtin.dir(), user.dir()}, 1); // roots set after watch() are watched too
        int calls = 0;
        stack.subscribe([&calls] { ++calls; });
        // Another writer (here: another library over the same folder) adds a tool to the user library.
        saveAll(user.dir(), {sampleTool(2)});
        CHECK(dispatcher.runUntil([&] { return stack.tool(ray::ToolId{fixedId(2)}) != nullptr; },
                                  std::chrono::milliseconds(10000)));
        CHECK(calls >= 1);
        CHECK(stack.rootOf(ray::lib::ItemKind::Tool, fixedId(2)) == std::optional<std::size_t>(1));
        // New roots: callbacks of the old ones that are still queued do nothing.
        stack.setRoots({builtin.dir()}, 1);
        dispatcher.runUntil([] { return false; }, std::chrono::milliseconds(100));
        CHECK(stack.tool(ray::ToolId{fixedId(2)}) == nullptr);
    } // the stack goes with its watch threads; anything still queued finds it gone
    dispatcher.runUntil([] { return false; }, std::chrono::milliseconds(100));
    ray::lib::detail::setWatchIntervalMs(5000);
}
