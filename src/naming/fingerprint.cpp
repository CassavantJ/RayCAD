// Geometric fingerprints (task T-007): quantisation, the tolerance compare and the ordinal order.
// Deterministic module: no <cmath> trig; std::floor and std::isfinite only.
#include "naming_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ray::naming {

namespace {

constexpr double kClamp = 1152921504606846976.0; // 2^60

// Canonical sign of an axis that has no inherent direction: the first non-zero component is positive.
void canonicalSign(std::int64_t& x, std::int64_t& y, std::int64_t& z) noexcept
{
    const std::int64_t first = x != 0 ? x : (y != 0 ? y : z);
    if (first < 0) {
        x = -x;
        y = -y;
        z = -z;
    }
}

void setAxis(Fingerprint& out, const Vec3& axis, bool canonical) noexcept
{
    out.qAxisX = detail::quantise(axis.x, detail::kAxisQuanta);
    out.qAxisY = detail::quantise(axis.y, detail::kAxisQuanta);
    out.qAxisZ = detail::quantise(axis.z, detail::kAxisQuanta);
    if (canonical)
        canonicalSign(out.qAxisX, out.qAxisY, out.qAxisZ);
}

void setPosition(Fingerprint& out, const Vec3& p) noexcept
{
    out.qCx = detail::quantise(p.x, detail::kLengthQuantaPerMm);
    out.qCy = detail::quantise(p.y, detail::kLengthQuantaPerMm);
    out.qCz = detail::quantise(p.z, detail::kLengthQuantaPerMm);
}

// Tolerances of matches() in quanta (naming.h): 1e-3 mm on centroid (and radius), 1e-7 relative on area.
// Axis components get 1e-5. One or two quanta of slack everywhere, because values rounded to a grid flip at
// rounding boundaries.
constexpr double kPositionTolerance = 10.0; // 1e-3 mm
constexpr double kRadiusTolerance = 10.0;   // 1e-3 mm
constexpr double kAxisTolerance = 10.0;     // 1e-5
constexpr double kAreaRelative = 1e-7;
constexpr double kAreaMinimum = 2.0; // 2e-4 mm^2 (or mm for edge lengths)

double diff(std::int64_t a, std::int64_t b) noexcept
{
    return static_cast<double>(a - b); // both within +-2^60: no overflow
}

} // namespace

namespace detail {

std::int64_t quantise(double value, double quantaPerUnit) noexcept
{
    if (!(value == value))
        return 0;
    double scaled = std::floor(value * quantaPerUnit + 0.5);
    scaled = std::clamp(scaled, -kClamp, kClamp);
    return static_cast<std::int64_t>(scaled);
}

Fingerprint emptyFingerprint() noexcept
{
    Fingerprint out{};
    out.surface = kernel::SurfaceType::Other;
    return out;
}

Fingerprint faceFingerprint(const kernel::SurfaceInfo& info, std::uint16_t neighbourCount) noexcept
{
    Fingerprint out = emptyFingerprint();
    out.surface = info.type;
    out.qArea = quantise(info.area, kLengthQuantaPerMm);
    setPosition(out, info.centroid);
    out.qRadius = quantise(info.radius, kLengthQuantaPerMm);
    if (info.type == kernel::SurfaceType::Plane) {
        // Outward normal: two coincident faces back to back differ by it (spike S4, cause C).
        const double sign = info.reversed ? -1.0 : 1.0;
        setAxis(out, Vec3{sign * info.axis.x, sign * info.axis.y, sign * info.axis.z}, false);
    } else if (info.type != kernel::SurfaceType::Bspline && info.type != kernel::SurfaceType::Other) {
        setAxis(out, info.axis, true);
    }
    out.neighbourCount = neighbourCount;
    return out;
}

Fingerprint edgeFingerprint(const kernel::CurveInfo& info, std::uint16_t neighbourCount) noexcept
{
    Fingerprint out = emptyFingerprint();
    // naming.h: "or curve type for edges". The numeric value of the CurveType is stored.
    out.surface = static_cast<kernel::SurfaceType>(static_cast<int>(info.type));
    const bool conic = info.type == kernel::CurveType::Circle || info.type == kernel::CurveType::Ellipse;
    if (info.closed && conic) {
        setPosition(out, info.center);
    } else {
        setPosition(out, Vec3{(info.start.x + info.end.x) * 0.5, (info.start.y + info.end.y) * 0.5,
                              (info.start.z + info.end.z) * 0.5});
    }
    out.qArea = quantise(info.length, kLengthQuantaPerMm);
    out.qRadius = quantise(info.radius, kLengthQuantaPerMm);
    if (info.type == kernel::CurveType::Line || conic)
        setAxis(out, info.axis, true);
    out.neighbourCount = neighbourCount;
    return out;
}

Fingerprint vertexFingerprint(const Vec3& point, std::uint16_t neighbourCount) noexcept
{
    Fingerprint out = emptyFingerprint();
    setPosition(out, point);
    out.neighbourCount = neighbourCount;
    return out;
}

Hash256 fingerprintKey(const Fingerprint& f)
{
    Hasher hasher;
    hasher.str("ray.naming.fingerprint.v1");
    hasher.i64(static_cast<std::int64_t>(f.surface));
    for (const std::int64_t value : {f.qArea, f.qCx, f.qCy, f.qCz, f.qRadius, f.qAxisX, f.qAxisY, f.qAxisZ})
        hasher.i64(value);
    hasher.i64(f.neighbourCount);
    return hasher.finish();
}

std::strong_ordering ordinalOrder(const Fingerprint& a, const Fingerprint& b) noexcept
{
    auto key = [](const Fingerprint& f) {
        return std::array<std::int64_t, 10>{f.qCx,           f.qCy,     f.qCz,
                                            f.qArea,         f.qAxisX,  f.qAxisY,
                                            f.qAxisZ,        f.qRadius, static_cast<std::int64_t>(f.surface),
                                            f.neighbourCount};
    };
    return key(a) <=> key(b);
}

} // namespace detail

bool Fingerprint::matches(const Fingerprint& other) const
{
    if (surface != other.surface || neighbourCount != other.neighbourCount)
        return false;
    const double dx = diff(qCx, other.qCx);
    const double dy = diff(qCy, other.qCy);
    const double dz = diff(qCz, other.qCz);
    if (dx * dx + dy * dy + dz * dz > kPositionTolerance * kPositionTolerance)
        return false;
    const double largest = std::max(std::abs(static_cast<double>(qArea)), std::abs(static_cast<double>(other.qArea)));
    if (std::abs(diff(qArea, other.qArea)) > std::max(kAreaMinimum, kAreaRelative * largest))
        return false;
    if (std::abs(diff(qRadius, other.qRadius)) > kRadiusTolerance)
        return false;
    return std::abs(diff(qAxisX, other.qAxisX)) <= kAxisTolerance &&
           std::abs(diff(qAxisY, other.qAxisY)) <= kAxisTolerance &&
           std::abs(diff(qAxisZ, other.qAxisZ)) <= kAxisTolerance;
}

} // namespace ray::naming
