// Private to src/render: the arithmetic of the glass backdrop (task T-016, PLAN.md 6.6), without Qt, so tests can
// rebuild the same blur on the CPU. The GPU side is rhi/backdrop_pass.h.
//
// The backdrop is the viewport's final colour image, box-filtered to a quarter of its size (4 x 4 pixels per
// texel), then blurred with a separable Gaussian (one horizontal and one vertical pass). Every glass panel samples
// that ONE texture at its own position.
#pragma once

#include <ray/render.h>

#include <vector>

namespace ray::render::detail {

constexpr int kBackdropDownscale = 4;
// Taps per Gaussian pass, centre included: the longest shader variant (shaders/backdrop_blur*.frag).
constexpr int kMaxBlurTaps = 23;
// The tap counts of the shader variants; a pass uses the shortest one that holds its taps (padding has weight 0).
constexpr int kBlurTapVariants[] = {7, 11, 15, 23};
// Theme metric: the blur's reach in logical pixels (about two standard deviations), and its value without one.
constexpr const char* kMetricBlurRadius = "blur.radius";
constexpr double kDefaultBlurRadiusPx = 24.0;

// One sample of a blur pass: `offset` texels from the texel centre along the pass direction, with its weight.
struct BlurTap {
    float offset = 0, weight = 0;
};

// Size of the backdrop for a viewport side of `pixels` physical pixels.
int backdropSize(int pixels);
// Standard deviation of the Gaussian in backdrop texels for the theme's blur.radius at this device pixel ratio,
// clamped to what kMaxBlurTaps can carry (and at least half a texel).
double backdropSigma(const Theme* theme, double devicePixelRatio);
// A normalised Gaussian of `sigma` texels cut at three sigma, as bilinear pairs: the centre tap first, then for
// each pair of texels (k, k+1) one tap between them with their summed weight, mirrored. Odd count, at most
// kMaxBlurTaps; the weights sum to 1.
std::vector<BlurTap> gaussianTaps(double sigma);
// The same Gaussian as discrete per-texel weights w[0..n] (w[k] for offsets +k and -k), for CPU references.
std::vector<double> gaussianWeights(double sigma);

} // namespace ray::render::detail
