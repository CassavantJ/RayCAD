// The shipped fixture models (task T-077): resources/library/fixtures/*.step are exactly the shapes fixture_models.h
// builds with RayCAD's kernel (bodies, names, volumes, boxes, faces), their headers carry no date, person or place, and
// with RAY_T077_WRITE_FIXTURES=<folder> this test writes them (the generator; see
// resources/library/fixtures/README.md).
#include "fixture_models.h"

#include <ray/kernel.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

using namespace ray;
namespace fm = ray::cam::test::fixtures;

namespace {

const std::string kFixtureDir = std::string(RAY_SOURCE_DIR) + "/resources/library/fixtures";

// The header line OCCT writes with the clock and its own placeholders, made fixed: name, a fixed date, no author and
// no organisation.
std::string normalised(std::string text, const std::string& fileName, const std::vector<fm::NamedShape>& shapes)
{
    text = std::regex_replace(text, std::regex(R"(FILE_NAME\([^;]*;)"),
                              "FILE_NAME('" + fileName +
                                  "','2026-01-01T00:00:00',(''),(''),'RayCAD fixture library','RayCAD','');");
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const std::string from = "'Body " + std::to_string(i + 1) + "'";
        const std::string to = "'" + shapes[i].name + "'";
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
        }
    }
    return text;
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace

TEST_CASE("the shipped fixture models are the shapes built in code", "[fixtures][library]")
{
    const char* writeTo = std::getenv("RAY_T077_WRITE_FIXTURES");
    for (const fm::LibraryFile& file : fm::kLibraryFiles) {
        INFO(file.fileName);
        const std::vector<fm::NamedShape> shapes = file.make();
        REQUIRE(!shapes.empty());
        for (const fm::NamedShape& s : shapes) {
            REQUIRE(!s.shape.isNull());
            REQUIRE(s.shape.count(kernel::SubKind::Solid) == 1);
        }
        if (writeTo != nullptr && *writeTo != '\0') {
            const std::filesystem::path out = std::filesystem::path(writeTo) / file.fileName;
            std::vector<kernel::Shape> list;
            for (const fm::NamedShape& s : shapes) {
                list.push_back(s.shape);
            }
            JobContext ctx;
            const std::u8string u8 = out.u8string();
            REQUIRE(kernel::exportStep(std::string(u8.begin(), u8.end()), list, ctx));
            const std::string text = normalised(readText(out), file.fileName, shapes);
            std::ofstream(out, std::ios::binary) << text;
        }
        const std::filesystem::path shipped = std::filesystem::path(kFixtureDir) / file.fileName;
        REQUIRE(std::filesystem::exists(shipped));
        // No clock, person or place in the header.
        const std::string text = readText(shipped);
        CHECK(text.find("FILE_NAME('" + file.fileName +
                        "','2026-01-01T00:00:00',(''),(''),'RayCAD fixture library','RayCAD','');") !=
              std::string::npos);
        CHECK(text.find("'Body 1'") == std::string::npos);
        JobContext ctx;
        const std::u8string u8 = shipped.u8string();
        auto imported = kernel::importStep(std::string(u8.begin(), u8.end()), kernel::ImportOptions{}, ctx);
        REQUIRE(imported);
        CHECK(imported->sourceUnits == UnitSystem::Metric);
        REQUIRE(imported->bodies.size() == shapes.size());
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            INFO(shapes[i].name);
            const kernel::Shape& got = imported->bodies[i].shape;
            CHECK(imported->bodies[i].name == shapes[i].name);
            auto a = kernel::massProps(got);
            auto b = kernel::massProps(shapes[i].shape);
            REQUIRE(a);
            REQUIRE(b);
            CHECK(a->volume == Catch::Approx(b->volume).epsilon(1e-9));
            CHECK(a->area == Catch::Approx(b->area).epsilon(1e-9));
            CHECK(got.count(kernel::SubKind::Face) == shapes[i].shape.count(kernel::SubKind::Face));
            const Box3 x = kernel::bounds(got);
            const Box3 y = kernel::bounds(shapes[i].shape);
            CHECK(x.min.x == Catch::Approx(y.min.x).margin(1e-5));
            CHECK(x.min.y == Catch::Approx(y.min.y).margin(1e-5));
            CHECK(x.min.z == Catch::Approx(y.min.z).margin(1e-5));
            CHECK(x.max.x == Catch::Approx(y.max.x).margin(1e-5));
            CHECK(x.max.y == Catch::Approx(y.max.y).margin(1e-5));
            CHECK(x.max.z == Catch::Approx(y.max.z).margin(1e-5));
        }
    }
    // The vise as a machinist knows it: the jaws 6 in wide and 1.75 in tall above the floor, the moving jaw 2 in open.
    const std::vector<fm::NamedShape> v = fm::vise();
    const Box3 jaw = kernel::bounds(v[1].shape);
    CHECK(jaw.max.x - jaw.min.x == Catch::Approx(152.4).margin(1e-6));
    CHECK(jaw.max.z - jaw.min.z == Catch::Approx(44.45).margin(1e-6));
    CHECK(jaw.min.y == Catch::Approx(50.8).margin(1e-6));
}
