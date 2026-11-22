// LibraryStack: layered roots, tool queries and template lookup.
#include "lib_test_util.h"
#include "tool_query.h"

#include <ray/libraries.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
    const std::map<std::string, double> vars = {{"feature.minWidth", 12}, {"feature.diameter", 6}};
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
    CHECK(stack.queryTools("{}", vars).size() == 7);
    // Malformed queries match nothing.
    CHECK(stack.queryTools(R"({"colour":"red"})", vars).empty());
    CHECK(stack.queryTools(R"({"diameter":"feature.unknown"})", vars).empty());
    CHECK(stack.queryTools(R"({"threadSpec":"$feature.threadSpec"})", vars).empty());
    CHECK(stack.queryTools("[", vars).empty());
}

TEST_CASE("expressions", "[lib][stack]")
{
    using ray::lib::detail::evaluate;
    const std::map<std::string, double> vars = {{"feature.threadDepth", 12}, {"tool.threadPitch", 1.25}, {"w", 4}};
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
