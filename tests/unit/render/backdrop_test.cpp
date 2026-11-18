// The glass backdrop (task T-016, PLAN.md 13 T-016 acceptance), offscreen:
//   - the blur's arithmetic (taps, sigma, size)
//   - exactly one blur sequence per changed frame while enabled, none while disabled or when the image did not
//     change (counter test); none while idle
//   - the blurred output matches a CPU rebuild of the same blur from the captured frame, pixel by pixel (which also
//     proves the orientation rule: readBackdrop flips by `yUp`), and the committed reference image within the
//     reference tests' structural tolerance
//   - the handle follows render.h's ownership and orientation rules; 200 resizes with the backdrop on neither crash
//     nor leak
//   - the cost of the blur at 1920x1080 (printed)
//
// To regenerate the reference image after an intended change: RAY_UPDATE_RENDER_REFERENCES=1 and run
//   ray_render_tests "[backdrop][reference]"
#include "viewport_scenes.h"
#include "viewport_testing.h"

#include <backdrop.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::render;
using namespace ray::render::test;
using Catch::Matchers::WithinAbs;
namespace rd = ray::render::detail;

namespace {

std::unique_ptr<IOffscreenViewport> viewportOrSkip(int w, int h, bool software = false)
{
    std::string whyNot;
    auto vp = makeViewport(w, h, software, &whyNot);
    if (!vp)
        SKIP("no RHI backend could be created: " << whyNot);
    return vp;
}

std::uint64_t blurs(const IRenderViewport& vp)
{
    return vp.info().backdropBlurs;
}

std::uint64_t frames(const IRenderViewport& vp)
{
    return vp.info().framesRendered;
}

bool updating()
{
    const char* v = std::getenv("RAY_UPDATE_RENDER_REFERENCES");
    return v && std::string(v) == "1";
}

// The blur rebuilt on the CPU from the full-resolution frame, as backdrop_pass.cpp does it on the GPU: a 4 x 4 box
// filter, then the Gaussian across and down with clamped edges, rounding to 8 bits after each step (the GPU keeps
// each step in an RGBA8 texture).
Image cpuBlur(const Image& full, double sigma)
{
    const int w = rd::backdropSize(full.width);
    const int h = rd::backdropSize(full.height);
    REQUIRE(w * rd::kBackdropDownscale == full.width);
    REQUIRE(h * rd::kBackdropDownscale == full.height);
    std::vector<double> a(std::size_t(w) * h * 3), b(a.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                double sum = 0;
                for (int dy = 0; dy < 4; ++dy) {
                    for (int dx = 0; dx < 4; ++dx)
                        sum += rgbAt(full, x * 4 + dx, y * 4 + dy)[std::size_t(c)];
                }
                a[(std::size_t(y) * w + x) * 3 + c] = std::round(sum / 16.0);
            }
        }
    }
    const std::vector<double> g = rd::gaussianWeights(sigma);
    const int n = int(g.size()) - 1;
    const auto pass = [&](const std::vector<double>& in, std::vector<double>& out, bool across) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 3; ++c) {
                    double sum = 0;
                    for (int k = -n; k <= n; ++k) {
                        const int xx = across ? std::clamp(x + k, 0, w - 1) : x;
                        const int yy = across ? y : std::clamp(y + k, 0, h - 1);
                        sum += g[std::size_t(std::abs(k))] * in[(std::size_t(yy) * w + xx) * 3 + c];
                    }
                    out[(std::size_t(y) * w + x) * 3 + c] = std::round(sum);
                }
            }
        }
    };
    pass(a, b, true);
    pass(b, a, false);
    Image out;
    out.width = w;
    out.height = h;
    out.rgba.resize(std::size_t(w) * h * 4);
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        for (int c = 0; c < 3; ++c)
            out.rgba[i * 4 + c] = std::uint8_t(std::clamp(a[i * 3 + c], 0.0, 255.0));
        out.rgba[i * 4 + 3] = 255;
    }
    return out;
}

} // namespace

// ---------- arithmetic, no GPU ----------
TEST_CASE("backdrop blur taps are a normalised, symmetric Gaussian", "[render][backdrop][unit]")
{
    CHECK(rd::backdropSize(1920) == 480);
    CHECK(rd::backdropSize(1081) == 271);
    CHECK(rd::backdropSize(1) == 1);
    for (const double sigma : {0.5, 1.0, 3.0, 4.7, 7.3}) {
        INFO("sigma " << sigma);
        const auto taps = rd::gaussianTaps(sigma);
        CHECK(taps.size() % 2 == 1);
        CHECK(int(taps.size()) <= rd::kMaxBlurTaps);
        double sum = 0, first = 0;
        for (const rd::BlurTap& t : taps) {
            sum += t.weight;
            first += t.weight * t.offset;
        }
        CHECK_THAT(sum, WithinAbs(1.0, 1e-5));
        CHECK_THAT(first, WithinAbs(0.0, 1e-5)); // symmetric
        // The bilinear pairs reproduce the discrete weights' second moment (the variance of the cut Gaussian).
        const auto w = rd::gaussianWeights(sigma);
        double variance = 0, pairs = 0;
        for (std::size_t k = 1; k < w.size(); ++k)
            variance += 2.0 * w[k] * double(k * k);
        for (const rd::BlurTap& t : taps)
            pairs += t.weight * t.offset * t.offset;
        CHECK(pairs <= variance + 1e-6); // pairing only loses the within-pair spread
        CHECK(pairs >= variance * 0.8);
    }
    // blur.radius is two standard deviations in logical pixels, a quarter of them per backdrop texel.
    const auto dark = theme("graphite-dark");
    const double radius = dark->metrics.at(rd::kMetricBlurRadius);
    CHECK_THAT(rd::backdropSigma(dark.get(), 1.0), WithinAbs(radius / 2 / 4, 1e-9));
    CHECK_THAT(rd::backdropSigma(dark.get(), 1.5), WithinAbs(radius * 1.5 / 2 / 4, 1e-9));
    CHECK(rd::backdropSigma(dark.get(), 8.0) <= 7.34); // clamped to what the taps carry
    CHECK_THAT(rd::backdropSigma(nullptr, 1.0), WithinAbs(rd::kDefaultBlurRadiusPx / 8, 1e-9));
}

// ---------- GPU ----------
TEST_CASE("one blur per changed frame while enabled, none otherwise", "[render][backdrop][counter]")
{
    auto vp = viewportOrSkip(320, 240);
    vp->setTheme(theme("graphite-dark"));
    buildScene("box", *vp, 320.0 / 240.0);
    REQUIRE(vp->renderNow().has_value());
    CHECK(blurs(*vp) == 0); // disabled by default
    CHECK(vp->blurredBackdropTexture().rhiTexture == nullptr);
    REQUIRE(!vp->readBackdrop().has_value());

    // Enabling blurs the image the viewport already holds, without drawing the scene again.
    vp->setBackdropEnabled(true);
    REQUIRE(vp->renderNow().has_value());
    CHECK(blurs(*vp) == 1);
    CHECK(vp->blurredBackdropTexture().rhiTexture != nullptr);

    // Every changed frame: exactly one more.
    Camera cam = vp->camera();
    for (int i = 0; i < 5; ++i) {
        cam = orbitCamera(cam, 7, 0, cam.target);
        vp->setCamera(cam);
        REQUIRE(vp->renderNow().has_value());
        CHECK(blurs(*vp) == std::uint64_t(2 + i));
    }
    // Frames that do not change the image (one that only answers a pick, one forced with nothing new) blur nothing.
    std::optional<std::optional<PickHit>> hit;
    vp->pick(160, 120, std::uint8_t(PickFilter::Faces), [&](std::optional<PickHit> h) { hit = h; });
    REQUIRE(processEventsUntil([&] { return hit.has_value(); }, 30000));
    REQUIRE(vp->renderNow().has_value());
    CHECK(blurs(*vp) == 6);
    // Setter calls coalesced into one frame give one blur; an idle viewport neither renders nor blurs.
    for (int i = 0; i < 10; ++i) {
        cam = orbitCamera(cam, 1, 0, cam.target);
        vp->setCamera(cam);
    }
    const std::uint64_t f0 = frames(*vp);
    processEventsFor(200);
    CHECK(frames(*vp) == f0 + 1);
    CHECK(blurs(*vp) == 7);
    processEventsFor(500);
    CHECK(frames(*vp) == f0 + 1);
    CHECK(blurs(*vp) == 7);
    // A frame never blurs twice: as many blurs as changed frames.
    const std::uint64_t b0 = blurs(*vp), fr0 = frames(*vp);
    for (int i = 0; i < 20; ++i) {
        vp->requestFrame();
        REQUIRE(vp->renderNow().has_value());
    }
    CHECK(blurs(*vp) - b0 == frames(*vp) - fr0);

    // Disabled: frames go on, blurs do not, and the texture is gone.
    vp->setBackdropEnabled(false);
    for (int i = 0; i < 5; ++i) {
        cam = orbitCamera(cam, 7, 0, cam.target);
        vp->setCamera(cam);
        REQUIRE(vp->renderNow().has_value());
    }
    CHECK(blurs(*vp) == b0 + 20);
    CHECK(vp->blurredBackdropTexture().rhiTexture == nullptr);
}

TEST_CASE("the blurred backdrop matches a CPU rebuild and the reference image", "[render][backdrop][reference]")
{
    constexpr int w = 320, h = 240;
    for (const bool software : {true, false}) {
        DYNAMIC_SECTION((software ? "software rasteriser" : "default adapter"))
        {
            auto empty = viewportOrSkip(w, h, software);
            const auto dark = theme("graphite-dark");
            empty->setTheme(dark);
            empty->setPreset(presetFor(Tier::Low));
            empty->setBackdropEnabled(true);
            REQUIRE(empty->renderNow().has_value());
            const Image background = take(empty->readBackdrop());

            auto vp = viewportOrSkip(w, h, software);
            vp->setTheme(dark);
            vp->setPreset(presetFor(Tier::Low)); // no MSAA: the capture is the frame the blur saw
            buildScene("two_bodies", *vp, double(w) / h);
            vp->setBackdropEnabled(true);
            REQUIRE(vp->renderNow().has_value());
            const Image blurred = take(vp->readBackdrop());
            const IViewport::Backdrop handle = vp->blurredBackdropTexture();
            const ViewportInfo info = vp->info();
            std::cout << "[backdrop] " << info.backend << " on " << info.device << ": " << handle.width << "x"
                      << handle.height << (handle.yUp ? ", y up" : ", y down") << "\n";
            REQUIRE(blurred.width == w / 4);
            REQUIRE(blurred.height == h / 4);

            // The CPU rebuild from the very frame the GPU blurred.
            const Image full = capture(*vp, w, h);
            const Image expected = cpuBlur(full, rd::backdropSigma(dark.get(), 1.0));
            int worst = 0;
            double mean = 0;
            for (int y = 0; y < blurred.height; ++y) {
                for (int x = 0; x < blurred.width; ++x) {
                    const int d = maxChannelDiff(rgbAt(blurred, x, y), rgbAt(expected, x, y));
                    worst = std::max(worst, d);
                    mean += d;
                }
            }
            mean /= double(blurred.width) * blurred.height;
            INFO("CPU rebuild: worst " << worst << ", mean " << mean);
            CHECK(worst <= 6);
            CHECK(mean <= 1.0);
            // Flipped, it would not match: the orientation is right.
            Image flipped = blurred;
            for (int y = 0; y < blurred.height; ++y)
                std::copy_n(&blurred.rgba[std::size_t(blurred.height - 1 - y) * blurred.width * 4], blurred.width * 4,
                            &flipped.rgba[std::size_t(y) * blurred.width * 4]);
            int flippedWorst = 0;
            for (int y = 0; y < blurred.height; ++y) {
                for (int x = 0; x < blurred.width; ++x)
                    flippedWorst = std::max(flippedWorst, maxChannelDiff(rgbAt(flipped, x, y), rgbAt(expected, x, y)));
            }
            CHECK(flippedWorst > 20);

            // The committed reference, with the reference tests' structural comparison.
            const std::string path = std::string(RAY_RENDER_REFERENCE_DIR) + "/backdrop_two_bodies.png";
            if (updating() && software) {
                REQUIRE(writePng(path, blurred).has_value());
                WARN("wrote reference " << path);
                continue;
            }
            auto reference = readPng(path);
            INFO("reference " << path);
            REQUIRE(reference.has_value());
            const Comparison c = compare(blurred, *reference, background, 4);
            std::cout << "[backdrop] reference: tile colour worst " << c.worstBlockColour << " mean "
                      << c.meanBlockColour << ", tile coverage worst " << c.worstBlockCoverage << ", coverage "
                      << c.coverageA << " vs " << c.coverageB << "\n";
            CHECK(c.coverageA > 0.05);
            CHECK(c.worstBlockColour <= 40.0);
            CHECK(c.meanBlockColour <= 3.0);
            CHECK(c.worstBlockCoverage <= 0.30);
            CHECK(std::abs(c.coverageA - c.coverageB) <= 0.002 + 0.08 * c.coverageB);
            // ... and tells a blur from the sharp image.
            const Comparison sharp = compare(blurred, cpuBlur(full, 0.5), background, 4);
            CHECK((sharp.worstBlockColour > 6.0 || sharp.meanBlockColour > 1.0));
        }
    }
}

TEST_CASE("the backdrop handle follows the ownership and orientation rules", "[render][backdrop][handle]")
{
    static_cast<void>(takeQtWarnings());
    {
        auto vp = viewportOrSkip(320, 240);
        vp->setTheme(theme("graphite-dark"));
        buildScene("box", *vp, 320.0 / 240.0);
        REQUIRE(vp->renderNow().has_value());
        const std::int64_t withoutBackdrop = vp->info().liveRhiResources;

        vp->setBackdropEnabled(true);
        REQUIRE(vp->renderNow().has_value());
        const IViewport::Backdrop a = vp->blurredBackdropTexture();
        REQUIRE(a.rhiTexture != nullptr);
        CHECK(a.width == 80);
        CHECK(a.height == 60);
        CHECK(a.yUp == (vp->info().backend == "OpenGL"));
        CHECK(a.generation >= 1);
        const std::int64_t withBackdrop = vp->info().liveRhiResources;
        CHECK(withBackdrop > withoutBackdrop);

        // A new blur of the same size: same texture, next generation.
        vp->requestFrame();
        REQUIRE(vp->renderNow().has_value());
        const IViewport::Backdrop b = vp->blurredBackdropTexture();
        CHECK(b.rhiTexture == a.rhiTexture);
        CHECK(b.generation == a.generation + 1);
        // An unchanged image: same generation.
        REQUIRE(vp->renderNow().has_value());
        CHECK(vp->blurredBackdropTexture().generation == b.generation);

        // A resize replaces the texture (the old one is retired until the frame is done, then freed).
        vp->resize(400, 300);
        REQUIRE(vp->renderNow().has_value());
        const IViewport::Backdrop c = vp->blurredBackdropTexture();
        REQUIRE(c.rhiTexture != nullptr);
        CHECK(c.width == 100);
        CHECK(c.height == 75);
        CHECK(c.generation > b.generation);
        CHECK(vp->info().liveRhiResources == withBackdrop);

        // Disabled: no handle, and every resource of the backdrop is released.
        vp->setBackdropEnabled(false);
        REQUIRE(vp->renderNow().has_value());
        CHECK(vp->blurredBackdropTexture().rhiTexture == nullptr);
        CHECK(vp->info().liveRhiResources == withoutBackdrop);
        CHECK(!vp->readBackdrop().has_value());
        // Enabled again: a fresh texture, blurred from the image already there.
        vp->setBackdropEnabled(true);
        REQUIRE(vp->renderNow().has_value());
        CHECK(vp->blurredBackdropTexture().rhiTexture != nullptr);
        CHECK(vp->info().liveRhiResources == withBackdrop);
    }
    for (const std::string& w : takeQtWarnings()) {
        INFO("Qt warning: " << w);
        CHECK(w.find("unreleased") == std::string::npos);
        CHECK(w.find("leak") == std::string::npos);
    }
}

TEST_CASE("200 resizes with the backdrop on neither crash nor leak", "[render][backdrop][resize]")
{
    static_cast<void>(takeQtWarnings());
    {
        auto vp = viewportOrSkip(320, 240);
        vp->setTheme(theme("graphite-dark"));
        vp->setPreset(presetFor(Tier::High));
        buildScene("two_bodies", *vp, 320.0 / 240.0);
        vp->setBackdropEnabled(true);
        REQUIRE(vp->renderNow().has_value());
        const std::int64_t baseline = vp->info().liveRhiResources;
        std::int64_t most = baseline;
        for (int i = 0; i < 200; ++i) {
            const int w = 64 + (i * 37) % 900;
            const int h = 48 + (i * 53) % 700;
            vp->resize(w, h);
            if (i % 7 == 3)
                vp->setBackdropEnabled(i % 2 == 0); // toggled now and then as glass panels come and go
            REQUIRE(vp->renderNow().has_value());
            const IViewport::Backdrop b = vp->blurredBackdropTexture();
            if (b.rhiTexture) {
                CHECK(b.width == rd::backdropSize(w));
                CHECK(b.height == rd::backdropSize(h));
            }
            most = std::max(most, vp->info().liveRhiResources);
        }
        vp->setBackdropEnabled(true);
        vp->resize(320, 240);
        REQUIRE(vp->renderNow().has_value());
        CHECK(vp->info().liveRhiResources == baseline);
        CHECK(most <= baseline + 8);
        CHECK(take(vp->readBackdrop()).width == 80);
    }
    for (const std::string& w : takeQtWarnings()) {
        INFO("Qt warning: " << w);
        CHECK(w.find("unreleased") == std::string::npos);
        CHECK(w.find("leak") == std::string::npos);
    }
}

TEST_CASE("cost of the blur at 1920x1080", "[render][backdrop][cost]")
{
    for (const bool software : {false, true}) {
        auto vp = viewportOrSkip(1920, 1080, software);
        vp->setTheme(theme("graphite-dark"));
        vp->setPreset(presetFor(Tier::Medium));
        buildScene("filleted_body", *vp, 1920.0 / 1080.0);
        REQUIRE(vp->renderNow().has_value());
        const int n = software ? 10 : 60;
        // GPU time per frame (and the frame's wall time), with and without the blur, in alternating blocks so that
        // both see the same load; every frame draws the scene again, as an orbit does.
        std::vector<double> on, off, onWall, offWall;
        for (int block = 0; block < 6; ++block) {
            const bool blur = block % 2 == 1;
            vp->setBackdropEnabled(blur);
            for (int i = 0; i < 3; ++i) { // settle: resources made, first frames out of the way
                vp->requestFrame();
                REQUIRE(vp->renderNow().has_value());
            }
            for (int i = 0; i < n; ++i) {
                vp->requestFrame();
                const auto t0 = std::chrono::steady_clock::now();
                REQUIRE(vp->renderNow().has_value());
                const double wall =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                (blur ? on : off).push_back(vp->lastFrameStats().gpuMs);
                (blur ? onWall : offWall).push_back(wall);
            }
        }
        const auto median = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        const ViewportInfo info = vp->info();
        std::cout << "[backdrop cost] 1920x1080 (backdrop " << rd::backdropSize(1920) << "x" << rd::backdropSize(1080)
                  << "), " << info.backend << " on " << info.device << (info.softwareRenderer ? " (software)" : "")
                  << ": GPU median " << median(off) << " ms without, " << median(on) << " ms with the blur (+"
                  << median(on) - median(off) << " ms); frame wall median " << median(offWall) << " / "
                  << median(onWall) << " ms (+" << median(onWall) - median(offWall) << " ms)\n";
        CHECK(blurs(*vp) >= std::uint64_t(n));
    }
}
