// Per-body colour (MeshStyle::faceColor, task T-016): faces take the item's own colour, edges and highlights keep
// their theme tokens, and selection and hover highlights stay clearly visible on any body colour (very light, very
// dark, saturated, and the highlight colours themselves) in both themes.
#include "viewport_scenes.h"
#include "viewport_testing.h"

#include <highlight.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::render;
using namespace ray::render::test;
namespace rd = ray::render::detail;

namespace {

std::unique_ptr<IOffscreenViewport> viewportOrSkip(int w, int h)
{
    std::string whyNot;
    auto vp = makeViewport(w, h, false, &whyNot);
    if (!vp)
        SKIP("no RHI backend could be created: " << whyNot);
    return vp;
}

Color linear(double r, double g, double b)
{
    const auto decode = [](double e) {
        return float(e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4));
    };
    return Color{decode(r), decode(g), decode(b), 1.0f};
}

struct Named {
    const char* name;
    Color colour;
};

// Body colours a file could carry, plus the highlight colours of the theme itself.
std::vector<Named> bodyColours(const Theme& t)
{
    return {
        {"white", linear(1, 1, 1)},
        {"black", linear(0, 0, 0)},
        {"light grey", linear(0.92, 0.92, 0.92)},
        {"red", linear(0.8, 0.1, 0.1)},
        {"green", linear(0.1, 0.7, 0.2)},
        {"blue", linear(0.1, 0.2, 0.9)},
        {"yellow", linear(1, 0.9, 0)},
        {"Selection", t.colors[std::size_t(Token::Selection)]},
        {"Hover", t.colors[std::size_t(Token::Hover)]},
    };
}

// The middle third of the image, where the front face of the box is.
template <class F>
void forCentre(int w, int h, F fn)
{
    for (int y = h / 3; y < 2 * h / 3; ++y) {
        for (int x = w / 3; x < 2 * w / 3; ++x)
            fn(x, y);
    }
}

} // namespace

TEST_CASE("highlight paint: theme bodies unchanged, any other colour gets a visible highlight",
          "[render][bodycolor][unit]")
{
    for (const char* name : {"graphite-dark", "graphite-light"}) {
        const auto t = theme(name);
        INFO("theme " << name);
        const Color sel = t->colors[std::size_t(Token::Selection)];
        const Color fill = t->colors[std::size_t(Token::SelectionFill)];
        const Color hover = t->colors[std::size_t(Token::Hover)];
        const Color model = t->colors[std::size_t(Token::ModelFace)];
        // Theme-coloured body: exactly the theme's highlight.
        const rd::HighlightPaint same = rd::highlightPaint(sel, fill, fill.a, model, model, false);
        CHECK(same.fillAlpha == fill.a);
        CHECK(rd::encodedDistance(same.fill, fill) == 0.0f);
        CHECK(rd::encodedDistance(same.line, sel) == 0.0f);
        const float themeShift = fill.a * rd::encodedDistance(fill, model);
        for (const Named& body : bodyColours(*t)) {
            INFO("body " << body.name);
            for (const bool isHover : {false, true}) {
                const Color line = isHover ? hover : sel;
                const Color f = isHover ? hover : fill;
                const rd::HighlightPaint p = rd::highlightPaint(line, f, fill.a, body.colour, model, true);
                // How far the fill moves the face colour, in sRGB units.
                const float shift = p.fillAlpha * rd::encodedDistance(p.fill, body.colour);
                CHECK(shift >= std::max(themeShift, rd::kMinCustomShift) - 1e-4f);
                CHECK(p.fillAlpha <= rd::kMaxFillAlpha + 1e-6f);
                CHECK(rd::encodedDistance(p.line, body.colour) >= rd::kMinLineDistance - 1e-4f);
            }
        }
    }
}

TEST_CASE("a body colour replaces the face token; theme switches keep it", "[render][bodycolor]")
{
    constexpr int w = 160, h = 120;
    auto vp = viewportOrSkip(w, h);
    const auto dark = theme("graphite-dark");
    const auto light = theme("graphite-light");
    vp->setTheme(dark);
    const Box3 box{Vec3{0, 0, 0}, Vec3{40, 30, 20}};
    MeshStyle style{Token::ModelFace, Token::ModelEdge, 1.0f, true, false};
    const ItemId id = vp->addMesh(meshOf(kernel::makeBox(box.min, box.max)), Xform::identity(), style);
    REQUIRE(id != 0);
    vp->setCamera(standardViewCamera(StandardView::Front, box, double(w) / h, false));
    const Image themed = capture(*vp, w, h);

    style.faceColor = linear(0.8, 0.1, 0.1);
    vp->setStyle(id, style);
    const Image red = capture(*vp, w, h);
    const auto c = rgbAt(red, w / 2, h / 2);
    INFO("centre " << c[0] << "," << c[1] << "," << c[2]);
    CHECK(c[0] > 2 * c[1]);
    CHECK(c[0] > 2 * c[2]);
    CHECK(maxChannelDiff(c, rgbAt(themed, w / 2, h / 2)) > 40);
    vp->setTheme(light);
    const auto c2 = rgbAt(capture(*vp, w, h), w / 2, h / 2);
    CHECK(maxChannelDiff(c, c2) <= 2); // the face is data, not a token: the theme does not change it
    // Not a number: ignored, the token applies again; no colour set: the token.
    style.faceColor = Color{std::nanf(""), 0, 0, 1};
    vp->setStyle(id, style);
    const Image back = capture(*vp, w, h);
    style.faceColor.reset();
    vp->setStyle(id, style);
    CHECK(maxChannelDiff(rgbAt(back, w / 2, h / 2), rgbAt(capture(*vp, w, h), w / 2, h / 2)) <= 2);
}

TEST_CASE("selection and hover stay clearly visible on any body colour, in both themes", "[render][bodycolor]")
{
    constexpr int w = 200, h = 150;
    // At least this much change (largest channel, 0..255) on nearly every pixel of the highlighted face.
    constexpr int kVisible = 24;
    for (const char* themeName : {"graphite-dark", "graphite-light"}) {
        auto vp = viewportOrSkip(w, h);
        const auto t = theme(themeName);
        vp->setTheme(t);
        vp->setPreset(presetFor(Tier::Medium));
        const Box3 box{Vec3{0, 0, 0}, Vec3{40, 30, 20}};
        MeshStyle style{Token::ModelFace, Token::ModelEdge, 1.0f, true, false};
        const ItemId id = vp->addMesh(meshOf(kernel::makeBox(box.min, box.max)), Xform::identity(), style);
        vp->setCamera(standardViewCamera(StandardView::Front, box, double(w) / h, false));
        static_cast<void>(capture(*vp, w, h));
        std::optional<std::optional<PickHit>> got;
        vp->pick(w / 2, h / 2, std::uint8_t(PickFilter::Faces), [&](std::optional<PickHit> hit) { got = hit; });
        REQUIRE(processEventsUntil([&] { return got.has_value(); }, 30000));
        REQUIRE(got->has_value());
        const PickHit face = **got;
        const PickHit body{id, kernel::SubKind::Solid, 0, {}, 0};

        for (const Named& colour : bodyColours(*t)) {
            INFO(themeName << ", body " << colour.name);
            style.faceColor = colour.colour;
            vp->setStyle(id, style);
            vp->setHighlight({}, std::nullopt);
            const Image base = capture(*vp, w, h);
            const auto visibleOn = [&](const Image& img, const char* what) {
                int n = 0, changed = 0, worst = 255;
                forCentre(w, h, [&](int x, int y) {
                    const int d = maxChannelDiff(rgbAt(img, x, y), rgbAt(base, x, y));
                    ++n;
                    changed += d >= kVisible ? 1 : 0;
                    worst = std::min(worst, d);
                });
                INFO(what << ": " << changed << " of " << n << " pixels changed by " << kVisible
                          << " or more, least change " << worst);
                CHECK(changed >= n * 98 / 100);
            };
            vp->setHighlight(std::span<const PickHit>(&face, 1), std::nullopt);
            visibleOn(capture(*vp, w, h), "selected face");
            vp->setHighlight({}, face);
            visibleOn(capture(*vp, w, h), "hovered face");
            vp->setHighlight(std::span<const PickHit>(&body, 1), std::nullopt);
            visibleOn(capture(*vp, w, h), "selected body");
        }
    }
}
