// Private to src/render: hover and selection colours on bodies drawn in their own colour (task T-016, per-body
// colour). No Qt here, so tests check the arithmetic directly.
//
// On a body drawn with a theme token (ModelFace), highlights are exactly the theme's: the SelectionFill (or Hover)
// colour at SelectionFill's opacity over the face, Selection (or Hover) for lines. A body colour comes from data and
// can be anything, including the highlight colour itself, so on such a body the highlight is made at least as
// visible as kMinCustomShift (in sRGB-encoded units, Euclidean over r, g, b, 0..sqrt(3)), and never less visible
// than the theme's highlight on ModelFace:
//   - the face fill gets the opacity that moves the face colour by that much (between the theme's and
//     kMaxFillAlpha);
//   - when even kMaxFillAlpha is not enough (the body is close to the highlight colour), the fill colour becomes a
//     much lighter or much darker variant of the highlight colour, whichever is farther from the body;
//   - a highlight line closer than kMinLineDistance to the body colour gets the same variant.
// The variants are computed from the theme's colours; no colour is written in code.
#pragma once

#include <ray/render.h>

namespace ray::render::detail {

constexpr float kMinCustomShift = 0.25f;
constexpr float kMaxFillAlpha = 0.85f;
constexpr float kMinLineDistance = 0.35f;

struct HighlightPaint {
    Color fill;      // linear RGB of the face fill
    float fillAlpha; // its opacity
    Color line;      // linear RGB of highlighted edges and vertex markers
};

// `line`: Selection or Hover. `fill`: the fill's colour (SelectionFill's colour, or Hover's for a hover) and
// `fillAlpha` its opacity (SelectionFill's alpha). `body`: the item's face colour; `themeBody`: the colour of the
// item's face token; `customBody`: `body` came from MeshStyle::faceColor. All linear RGB.
HighlightPaint highlightPaint(Color line, Color fill, float fillAlpha, Color body, Color themeBody, bool customBody);

// Distance of two linear colours in sRGB-encoded units (Euclidean over r, g, b).
float encodedDistance(Color a, Color b);

} // namespace ray::render::detail
