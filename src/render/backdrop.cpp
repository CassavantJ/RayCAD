// The arithmetic of the glass backdrop (task T-016). See backdrop.h.
#include "backdrop.h"

#include <algorithm>
#include <cmath>

namespace ray::render::detail {

namespace {

// Texels covered on each side of the centre: three sigma.
int reach(double sigma)
{
    return std::max(1, int(std::ceil(3.0 * sigma)));
}

// The largest sigma whose three-sigma reach still fits kMaxBlurTaps bilinear taps: (taps - 1) / 2 pairs per side,
// two texels per pair.
double maxSigma()
{
    const int texelsPerSide = (kMaxBlurTaps - 1) / 2 * 2;
    return double(texelsPerSide) / 3.0 - 1e-6; // so that ceil(3 sigma) cannot round up past the last pair
}

} // namespace

int backdropSize(int pixels)
{
    return std::max(1, (pixels + kBackdropDownscale - 1) / kBackdropDownscale);
}

double backdropSigma(const Theme* theme, double devicePixelRatio)
{
    double radius = kDefaultBlurRadiusPx;
    if (theme) {
        const auto it = theme->metrics.find(kMetricBlurRadius);
        if (it != theme->metrics.end() && std::isfinite(it->second) && it->second >= 0)
            radius = it->second;
    }
    const double dpr = devicePixelRatio > 0 && std::isfinite(devicePixelRatio) ? devicePixelRatio : 1.0;
    // blur.radius is about two standard deviations, in logical pixels; the backdrop has a quarter of the pixels.
    const double sigma = radius * dpr / 2.0 / kBackdropDownscale;
    return std::clamp(sigma, 0.5, maxSigma());
}

std::vector<double> gaussianWeights(double sigma)
{
    const int n = reach(sigma);
    std::vector<double> w(std::size_t(n) + 1);
    double sum = 0;
    for (int k = 0; k <= n; ++k) {
        w[std::size_t(k)] = std::exp(-double(k) * k / (2.0 * sigma * sigma));
        sum += k == 0 ? w[0] : 2.0 * w[std::size_t(k)];
    }
    for (double& v : w)
        v /= sum;
    return w;
}

std::vector<BlurTap> gaussianTaps(double sigma)
{
    const std::vector<double> w = gaussianWeights(sigma);
    const int n = int(w.size()) - 1;
    std::vector<BlurTap> taps;
    taps.push_back(BlurTap{0.0f, float(w[0])});
    for (int k = 1; k <= n; k += 2) {
        const double a = w[std::size_t(k)];
        const double b = k + 1 <= n ? w[std::size_t(k) + 1] : 0.0;
        const double weight = a + b;
        if (weight <= 0)
            continue;
        const double offset = (k * a + (k + 1) * b) / weight;
        taps.push_back(BlurTap{float(offset), float(weight)});
        taps.push_back(BlurTap{float(-offset), float(weight)});
    }
    if (int(taps.size()) > kMaxBlurTaps)
        taps.resize(std::size_t(kMaxBlurTaps)); // only for a sigma above backdropSigma's clamp; an odd count stays
    return taps;
}

} // namespace ray::render::detail
