// Helpers for the viewport tests (task T-014). No Qt header here: the tests drive the viewport through
// ray_render/viewport.h only (PLAN.md 0.4 rule 1).
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <ray/kernel.h>
#include <ray/render.h>
#include <ray_render/viewport.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ray::render::test {

constexpr double kPi = 3.14159265358979323846;

inline std::string themePath(const char* name)
{
    return std::string(RAY_THEMES_DIR) + "/" + name + ".json";
}

inline std::shared_ptr<const Theme> theme(const char* name)
{
    auto loaded = loadTheme(themePath(name));
    INFO("theme " << name << ": " << (loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded.has_value());
    return std::make_shared<const Theme>(std::move(*loaded));
}

template <class T>
T take(Result<T> result)
{
    INFO("unexpected error: " << (result.has_value() ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    return std::move(*result);
}

// Display mesh of a shape at a fine tolerance, so reference images do not depend on the tier.
inline std::shared_ptr<const kernel::Mesh> meshOf(Result<kernel::Shape> shape, double chordTol = 0.05,
                                                  double angleTolRad = 10 * kPi / 180)
{
    kernel::Shape s = take(std::move(shape));
    JobContext ctx;
    return std::make_shared<const kernel::Mesh>(
        take(kernel::tessellate(s, kernel::MeshOptions{chordTol, angleTolRad}, ctx)));
}

// The offscreen viewport, or nullopt with the reason when no graphics API can be created here (the caller SKIPs).
inline std::unique_ptr<IOffscreenViewport> makeViewport(int w, int h, bool software, std::string* whyNot)
{
    if (Status ok = ensureQtRuntime(); !ok) {
        *whyNot = ok.error().message;
        return nullptr;
    }
    OffscreenOptions options;
    options.width = w;
    options.height = h;
    options.preferSoftware = software;
    auto vp = makeOffscreenViewport(options);
    if (!vp) {
        *whyNot = vp.error().message;
        return nullptr;
    }
    return std::move(*vp);
}

// Captures w x h pixels through captureImage (the public path) and decodes them.
inline Image capture(IViewport& vp, int w, int h)
{
    std::optional<std::vector<std::byte>> png;
    vp.captureImage(w, h, [&png](std::vector<std::byte> bytes) { png = std::move(bytes); });
    REQUIRE(processEventsUntil([&png] { return png.has_value(); }, 60000));
    REQUIRE(!png->empty());
    return take(decodePng(*png));
}

inline std::uint8_t encodeSrgb8(float linear)
{
    const double c = std::clamp(double(linear), 0.0, 1.0);
    const double e = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(std::lround(e * 255.0));
}

inline std::array<int, 3> rgbAt(const Image& img, int x, int y)
{
    const std::size_t i = (std::size_t(y) * std::size_t(img.width) + std::size_t(x)) * 4;
    return {img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]};
}

inline int maxChannelDiff(const std::array<int, 3>& a, const std::array<int, 3>& b)
{
    return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

// Pixels that differ from the empty background render by more than `threshold` in some channel.
inline std::vector<std::uint8_t> coverage(const Image& img, const Image& background, int threshold = 10)
{
    std::vector<std::uint8_t> mask(std::size_t(img.width) * std::size_t(img.height), 0);
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x)
            mask[std::size_t(y) * std::size_t(img.width) + std::size_t(x)] =
                maxChannelDiff(rgbAt(img, x, y), rgbAt(background, x, y)) > threshold ? 1 : 0;
    }
    return mask;
}

// Structural comparison that tolerates rasteriser differences (edge pixels, MSAA patterns, shading rounding):
// both images are cut into `block` x `block` tiles; per tile it compares the mean colour and the fraction of pixels
// covered by the model.
struct Comparison {
    double worstBlockColour = 0;         // largest per-tile mean colour difference, 0..255
    double meanBlockColour = 0;          // mean over tiles
    double worstBlockCoverage = 0;       // largest per-tile difference in covered fraction, 0..1
    double coverageA = 0, coverageB = 0; // covered fraction of the whole image
};

inline Comparison compare(const Image& a, const Image& b, const Image& background, int block = 8)
{
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    const auto maskA = coverage(a, background);
    const auto maskB = coverage(b, background);
    Comparison out;
    int tiles = 0;
    for (int by = 0; by < a.height; by += block) {
        for (int bx = 0; bx < a.width; bx += block) {
            double sumA[3] = {0, 0, 0}, sumB[3] = {0, 0, 0};
            double covA = 0, covB = 0;
            int n = 0;
            for (int y = by; y < std::min(by + block, a.height); ++y) {
                for (int x = bx; x < std::min(bx + block, a.width); ++x) {
                    const auto pa = rgbAt(a, x, y);
                    const auto pb = rgbAt(b, x, y);
                    for (int c = 0; c < 3; ++c) {
                        sumA[c] += pa[std::size_t(c)];
                        sumB[c] += pb[std::size_t(c)];
                    }
                    const std::size_t i = std::size_t(y) * std::size_t(a.width) + std::size_t(x);
                    covA += maskA[i];
                    covB += maskB[i];
                    ++n;
                }
            }
            double colour = 0;
            for (int c = 0; c < 3; ++c)
                colour = std::max(colour, std::abs(sumA[c] - sumB[c]) / n);
            out.worstBlockColour = std::max(out.worstBlockColour, colour);
            out.meanBlockColour += colour;
            out.worstBlockCoverage = std::max(out.worstBlockCoverage, std::abs(covA - covB) / n);
            out.coverageA += covA;
            out.coverageB += covB;
            ++tiles;
        }
    }
    out.meanBlockColour /= std::max(1, tiles);
    out.coverageA /= double(a.width) * a.height;
    out.coverageB /= double(a.width) * a.height;
    return out;
}

} // namespace ray::render::test
