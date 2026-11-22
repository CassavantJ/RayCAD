// The built-in library shipped in resources/library (task T-031): it loads with no issues, has what the plan asks
// for, cites a source in every file, and is exactly what the library itself would write.
#include "json.h"
#include "lib_test_util.h"

#include <ray/libraries.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <set>
#include <string>

using namespace libtest;
using ray::lib::ToolType;

namespace {

const std::string kRoot = std::string(RAY_SOURCE_DIR) + "/resources/library";

std::set<std::string> threadDesignations()
{
    std::set<std::string> out;
    auto table = ray::lib::json::parse(readFile(std::string(RAY_SOURCE_DIR) + "/resources/standards/threads.json"));
    REQUIRE(table);
    for (const auto& series : table->find("series")->asArray()) {
        for (const auto& size : series.find("sizes")->asArray()) {
            out.insert(size.find("designation")->asString());
        }
    }
    return out;
}

} // namespace

TEST_CASE("the shipped library loads with zero issues", "[lib][shipped]")
{
    auto library = ray::lib::ILibrary::open(kRoot);
    REQUIRE(library);
    for (const auto& issue : (*library)->issues()) {
        FAIL_CHECK(issue.file << ": " << issue.message);
    }
    ray::lib::LibraryStack stack;
    stack.setRoots({kRoot});
    CHECK(stack.issues().empty());
}

TEST_CASE("the shipped library has what PLAN.md T-031 asks for", "[lib][shipped]")
{
    auto opened = ray::lib::ILibrary::open(kRoot);
    REQUIRE(opened);
    const auto& library = **opened;
    const auto tools = library.tools();
    const auto materials = library.materials();
    const auto machines = library.machines();
    CHECK(tools.size() >= 55);
    CHECK(tools.size() <= 75);
    CHECK(library.holders().size() == 8);
    REQUIRE(materials.size() == 8);
    REQUIRE(machines.size() == 3);
    for (const auto& m : machines) {
        CHECK_FALSE(m.verified);
        CHECK(m.kind == ray::lib::MachineDef::Kind::Mill3Axis);
        CHECK(m.axes.size() == 3);
    }
    std::set<std::string> materialNames;
    std::set<ray::MaterialId> materialIds;
    for (const auto& m : materials) {
        materialNames.insert(m.name);
        materialIds.insert(m.id);
    }
    for (const char* grade : {"6061", "7075", "1018", "4140 ", "4140PH", "304", "17-4", "Ti-6Al-4V"}) {
        bool found = false;
        for (const auto& name : materialNames) {
            found = found || name.rfind(grade, 0) == 0;
        }
        INFO(grade);
        CHECK(found);
    }
    std::map<ToolType, int> byType;
    const auto threads = threadDesignations();
    std::set<std::string> drillSizeKinds;
    for (const auto& t : tools) {
        INFO(t.name);
        ++byType[t.type];
        CHECK(t.vendor.empty());    // generic tools only
        CHECK(t.productId.empty()); // no part numbers
        // Every tool has starting data for every shipped material.
        std::set<ray::MaterialId> covered;
        for (const auto& p : t.presets) {
            CHECK(materialIds.count(p.material) == 1);
            covered.insert(p.material);
        }
        CHECK(covered == materialIds);
        if (t.type == ToolType::Tap) {
            CHECK(threads.count(t.threadSpec) == 1);
        }
        if (t.type == ToolType::Drill) {
            if (t.name.rfind("#", 0) == 0) {
                drillSizeKinds.insert("number");
            } else if (t.name.size() > 1 && t.name[1] == ' ') {
                drillSizeKinds.insert("letter");
            } else if (t.name.find(" mm ") != std::string::npos) {
                drillSizeKinds.insert("metric");
            } else if (t.name.find('/') != std::string::npos) {
                drillSizeKinds.insert("fraction");
            }
        }
    }
    CHECK(drillSizeKinds == std::set<std::string>{"fraction", "letter", "metric", "number"});
    for (const ToolType type :
         {ToolType::FlatEndMill, ToolType::BallEndMill, ToolType::BullEndMill, ToolType::Drill, ToolType::SpotDrill,
          ToolType::Tap, ToolType::ChamferMill, ToolType::FaceMill, ToolType::Reamer, ToolType::ThreadMill}) {
        CHECK(byType[type] > 0);
    }
    std::set<std::string> taps;
    for (const auto& t : tools) {
        if (t.type == ToolType::Tap) {
            taps.insert(t.threadSpec.substr(t.threadSpec.size() - 3));
        }
    }
    CHECK(taps.count("UNC") == 1);
    CHECK(taps.count("UNF") == 1);
}

TEST_CASE("every shipped file cites its source and is in the form the library writes", "[lib][shipped]")
{
    int files = 0;
    for (const char* folder : {"tools", "holders", "materials", "machines"}) {
        for (const std::string& name : listFiles(kRoot + "/" + folder)) {
            INFO(folder << "/" << name);
            const std::string text = readFile(kRoot + "/" + folder + "/" + name);
            auto value = ray::lib::json::parse(text);
            REQUIRE(value);
            const auto* comment = value->find("comment");
            REQUIRE(comment);
            CHECK(comment->asString().size() > 40);
            // Saving the item again would not change a byte (the generator writes what ILibrary::save writes).
            CHECK(ray::lib::json::writePretty(*value) == text);
            // Files are named by their id.
            CHECK(name == value->find("id")->asString() + "." +
                              std::string(folder).substr(0, std::string(folder).size() - 1) + ".json");
            ++files;
        }
    }
    CHECK(files >= 55 + 8 + 8 + 3);
}
