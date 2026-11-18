// Reference images of the viewport (task T-014, PLAN.md 13 T-014 acceptance): five scenes rendered offscreen must
// match the committed PNGs in tests/unit/render/reference/ within a tolerance that absorbs rasteriser differences
// (WARP, llvmpipe, hardware), using the tile comparison of viewport_testing.h.
//
// To regenerate after an intended change: set RAY_UPDATE_RENDER_REFERENCES=1 and run
//   ray_render_tests "[reference]"
// The software-rasteriser pass then writes the PNGs into the source tree; review them before committing.
#include "viewport_scenes.h"
#include "viewport_testing.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace ray;
using namespace ray::render;
using namespace ray::render::test;

namespace {

constexpr int kWidth = 160;
constexpr int kHeight = 120;

// Tolerances. WARP against an NVIDIA and an Intel GPU on Direct3D 11 differ by at most 2/255 in a tile's mean colour
// and 5 % in a tile's coverage; the limits leave room for other rasterisers (llvmpipe, Metal: other MSAA patterns,
// edges a pixel off) while a missing body, face colour, edge set or line still fails.
constexpr double kMaxTileColour = 40.0;     // one 8x8 tile, mean colour, 0..255
constexpr double kMaxMeanColour = 3.0;      // mean over all tiles
constexpr double kMaxTileCoverage = 0.30;   // one 8x8 tile, covered fraction
constexpr double kCoverageAbsolute = 0.002; // whole image, covered fraction: 0.002 + 8 % of the reference's
constexpr double kCoverageRelative = 0.08;

std::string referencePath(const std::string& scene)
{
    return std::string(RAY_RENDER_REFERENCE_DIR) + "/" + scene + ".png";
}

bool updating()
{
    const char* v = std::getenv("RAY_UPDATE_RENDER_REFERENCES");
    return v && std::string(v) == "1";
}

} // namespace

TEST_CASE("viewport renders the reference scenes", "[render][viewport][reference]")
{
    for (const bool software : {true, false}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            std::string whyNot;
            auto vp = makeViewport(kWidth, kHeight, software, &whyNot);
            if (!vp)
                SKIP("no RHI backend could be created: " << whyNot);
            const auto dark = theme("graphite-dark");
            vp->setTheme(dark);
            const Image background = capture(*vp, kWidth, kHeight);
            const ViewportInfo info = vp->info();
            std::cout << "[reference] " << info.backend << " on " << info.device
                      << (info.softwareRenderer ? " (software)" : "") << "\n";

            for (const Scene& scene : scenes()) {
                auto view = makeViewport(kWidth, kHeight, software, &whyNot);
                REQUIRE(view);
                view->setTheme(dark);
                view->setPreset(presetFor(scene.preset));
                buildScene(scene.name, *view, double(kWidth) / kHeight);
                const Image actual = capture(*view, kWidth, kHeight);
                const std::string path = referencePath(scene.name);
                if (updating() && software) {
                    REQUIRE(writePng(path, actual).has_value());
                    WARN("wrote reference " << path);
                    continue;
                }
                auto reference = readPng(path);
                INFO("scene " << scene.name << ", reference " << path);
                REQUIRE(reference.has_value());
                const Comparison c = compare(actual, *reference, background);
                const std::string actualPath =
                    (std::filesystem::temp_directory_path() / ("ray_render_" + scene.name + "_actual.png")).string();
                INFO("tile colour worst " << c.worstBlockColour << " mean " << c.meanBlockColour
                                          << ", tile coverage worst " << c.worstBlockCoverage << ", coverage "
                                          << c.coverageA << " vs " << c.coverageB
                                          << "; this run's image: " << actualPath);
                std::cout << "[reference] " << scene.name << ": tile colour worst " << c.worstBlockColour << " mean "
                          << c.meanBlockColour << ", tile coverage worst " << c.worstBlockCoverage << ", coverage "
                          << c.coverageA << " vs " << c.coverageB << "\n";
                const double maxCoverageDelta = kCoverageAbsolute + kCoverageRelative * c.coverageB;
                const bool ok = c.worstBlockColour <= kMaxTileColour && c.meanBlockColour <= kMaxMeanColour &&
                                c.worstBlockCoverage <= kMaxTileCoverage &&
                                std::abs(c.coverageA - c.coverageB) <= maxCoverageDelta;
                if (!ok)
                    static_cast<void>(writePng(actualPath, actual));
                CHECK(c.coverageA > 0.02); // the scene is really there
                CHECK(c.worstBlockColour <= kMaxTileColour);
                CHECK(c.meanBlockColour <= kMaxMeanColour);
                CHECK(c.worstBlockCoverage <= kMaxTileCoverage);
                CHECK(std::abs(c.coverageA - c.coverageB) <= maxCoverageDelta);

                // The comparison is strict enough to tell the scenes apart.
                for (const Scene& other : scenes()) {
                    if (other.name == scene.name)
                        continue;
                    auto otherRef = readPng(referencePath(other.name));
                    REQUIRE(otherRef.has_value());
                    const Comparison o = compare(actual, *otherRef, background);
                    INFO("against " << other.name);
                    CHECK((o.worstBlockColour > kMaxTileColour || o.meanBlockColour > kMaxMeanColour ||
                           o.worstBlockCoverage > kMaxTileCoverage ||
                           std::abs(o.coverageA - o.coverageB) > kCoverageAbsolute + kCoverageRelative * o.coverageB));
                }
            }
        }
    }
}
