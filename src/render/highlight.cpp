// Hover and selection colours on bodies drawn in their own colour (task T-016). See highlight.h.
#include "highlight.h"

#include <algorithm>
#include <cmath>

namespace ray::render::detail {

namespace {

// How far the two variants of a highlight colour move towards black and towards white (linear light).
constexpr float kDarkVariant = 0.2f;  // keeps 20 % of the colour
constexpr float kLightVariant = 0.7f; // goes 70 % of the way to white

float encode(float linear)
{
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

Color scaled(Color c, float k)
{
    return Color{c.r * k, c.g * k, c.b * k, c.a};
}

Color towardsWhite(Color c, float k)
{
    return Color{c.r + (1 - c.r) * k, c.g + (1 - c.g) * k, c.b + (1 - c.b) * k, c.a};
}

// The variant of `c` (much darker or much lighter) that is farther from `body`.
Color contrastVariant(Color c, Color body)
{
    const Color dark = scaled(c, kDarkVariant);
    const Color light = towardsWhite(c, kLightVariant);
    return encodedDistance(dark, body) >= encodedDistance(light, body) ? dark : light;
}

} // namespace

float encodedDistance(Color a, Color b)
{
    const float dr = encode(a.r) - encode(b.r);
    const float dg = encode(a.g) - encode(b.g);
    const float db = encode(a.b) - encode(b.b);
    return std::sqrt(dr * dr + dg * dg + db * db);
}

HighlightPaint highlightPaint(Color line, Color fill, float fillAlpha, Color body, Color themeBody, bool customBody)
{
    fillAlpha = std::clamp(fillAlpha, 0.0f, 1.0f);
    if (!customBody)
        return HighlightPaint{fill, fillAlpha, line};
    // At least what the theme gives on its own body colour, and at least kMinCustomShift.
    const float wanted = std::max(fillAlpha * encodedDistance(fill, themeBody), kMinCustomShift);
    const float maxAlpha = std::max(kMaxFillAlpha, fillAlpha);
    Color target = fill;
    if (encodedDistance(target, body) * maxAlpha < wanted)
        target = contrastVariant(fill, body);
    const float distance = std::max(encodedDistance(target, body), 1e-6f);
    const float alpha = std::clamp(wanted / distance, fillAlpha, maxAlpha);
    const Color outline = encodedDistance(line, body) < kMinLineDistance ? contrastVariant(line, body) : line;
    return HighlightPaint{target, alpha, outline};
}

} // namespace ray::render::detail
