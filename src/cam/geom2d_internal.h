// Private to src/cam/geom2d*.cpp (task T-050): exact integer predicates and the small vector arithmetic the geom2d
// files share. Everything here is deterministic: integer predicates are exact in 128 bits, double arithmetic is plain
// IEEE (src/cam is built without contraction or fast math) and trigonometry goes through ray::dmath.
#pragma once
#include "geom2d.h"

#include <cmath>
#include <cstdint>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace ray::cam::geom2d::detail {

inline constexpr double kPi = 3.141592653589793238462643383279502884;
inline constexpr double kTwoPi = 2.0 * kPi;

// ---------- 128-bit products ----------
struct I128 {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0; // two's complement high word
};
inline I128 mul(std::int64_t a, std::int64_t b)
{
#if defined(__SIZEOF_INT128__)
    const __int128 r = static_cast<__int128>(a) * static_cast<__int128>(b);
    return I128{static_cast<std::uint64_t>(r), static_cast<std::uint64_t>(static_cast<unsigned __int128>(r) >> 64)};
#elif defined(_M_X64)
    std::int64_t high = 0;
    const std::int64_t low = _mul128(a, b, &high);
    return I128{static_cast<std::uint64_t>(low), static_cast<std::uint64_t>(high)};
#elif defined(_M_ARM64)
    return I128{static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b),
                static_cast<std::uint64_t>(__mulh(a, b))};
#else
#error "geom2d needs a 64 x 64 -> 128 bit multiply"
#endif
}
inline I128 add(I128 a, I128 b)
{
    const std::uint64_t lo = a.lo + b.lo;
    return I128{lo, a.hi + b.hi + (lo < a.lo ? 1u : 0u)};
}
inline I128 sub(I128 a, I128 b)
{
    return I128{a.lo - b.lo, a.hi - b.hi - (a.lo < b.lo ? 1u : 0u)};
}
inline int sign(I128 v)
{
    if (static_cast<std::int64_t>(v.hi) < 0)
        return -1;
    return (v.hi != 0 || v.lo != 0) ? 1 : 0;
}
inline int compare(I128 a, I128 b)
{
    return sign(sub(a, b));
}

// Sign of (b - a) x (c - a): +1 counter-clockwise (c left of a->b), -1 clockwise, 0 collinear. Exact.
inline int orient(Pt a, Pt b, Pt c)
{
    return sign(sub(mul(b.x - a.x, c.y - a.y), mul(b.y - a.y, c.x - a.x)));
}
// Sign of u x v and of u . v for integer vectors. Exact.
inline int crossSign(Coord ux, Coord uy, Coord vx, Coord vy)
{
    return sign(sub(mul(ux, vy), mul(uy, vx)));
}
inline int dotSign(Coord ux, Coord uy, Coord vx, Coord vy)
{
    return sign(add(mul(ux, vx), mul(uy, vy)));
}
// Sign of |p - c|^2 - r^2. Exact.
inline int circleSide(Pt p, Pt c, Coord r)
{
    const Coord dx = p.x - c.x, dy = p.y - c.y;
    return compare(add(mul(dx, dx), mul(dy, dy)), mul(r, r));
}

// ---------- Doubles ----------
struct D2 {
    double x = 0, y = 0;
};
inline D2 d2(Pt p)
{
    return D2{static_cast<double>(p.x), static_cast<double>(p.y)};
}
inline D2 operator+(D2 a, D2 b)
{
    return D2{a.x + b.x, a.y + b.y};
}
inline D2 operator-(D2 a, D2 b)
{
    return D2{a.x - b.x, a.y - b.y};
}
inline D2 operator*(D2 a, double s)
{
    return D2{a.x * s, a.y * s};
}
inline double dot(D2 a, D2 b)
{
    return a.x * b.x + a.y * b.y;
}
inline double cross(D2 a, D2 b)
{
    return a.x * b.y - a.y * b.x;
}
inline double norm(D2 a)
{
    return dmath::sqrt(a.x * a.x + a.y * a.y);
}
inline D2 unit(D2 a)
{
    const double n = norm(a);
    return n > 0.0 ? D2{a.x / n, a.y / n} : D2{0.0, 0.0};
}
inline double distance(D2 a, D2 b)
{
    return norm(a - b);
}
inline Pt roundPt(D2 p)
{
    return Pt{roundNm(p.x), roundNm(p.y)};
}
// Rotations by +90 (left) and -90 (right) degrees.
inline D2 leftOf(D2 t)
{
    return D2{-t.y, t.x};
}
inline D2 rightOf(D2 t)
{
    return D2{t.y, -t.x};
}

inline bool isArc(const Span& s)
{
    return s.kind != SpanKind::Line;
}
inline bool isCcw(const Span& s)
{
    return s.kind == SpanKind::ArcCCW;
}
// The angle of p about c, in (-pi, pi].
inline double angleOf(D2 p, D2 c)
{
    return dmath::atan2(p.y - c.y, p.x - c.x);
}
// The sweep from angle a0 to a1 going counter-clockwise (ccw) or clockwise, in (0, 2 pi]; whole circle for equal.
inline double sweepBetween(double a0, double a1, bool ccw)
{
    double s = ccw ? a1 - a0 : a0 - a1;
    while (s <= 0.0)
        s += kTwoPi;
    while (s > kTwoPi)
        s -= kTwoPi;
    return s;
}
// The point of the circle (c, r) at angle a, as a double.
inline D2 onCircle(D2 c, double r, double a)
{
    return D2{c.x + r * dmath::cos(a), c.y + r * dmath::sin(a)};
}
// Unit tangent of a span at its start (s = start point) or end, in the direction of travel.
inline D2 tangentAtStart(Pt start, const Span& s)
{
    if (!isArc(s))
        return unit(d2(s.end) - d2(start));
    const D2 radial = d2(start) - d2(s.centre);
    return unit(isCcw(s) ? leftOf(radial) : rightOf(radial));
}
inline D2 tangentAtEnd(Pt start, const Span& s)
{
    if (!isArc(s))
        return unit(d2(s.end) - d2(start));
    const D2 radial = d2(s.end) - d2(s.centre);
    return unit(isCcw(s) ? leftOf(radial) : rightOf(radial));
}
inline double spanLength(Pt start, const Span& s)
{
    if (!isArc(s))
        return distance(d2(start), d2(s.end));
    return static_cast<double>(s.radius) * sweepOf(start, s);
}

// The vertices of a path: start, then every span end (a closed path's last end, equal to start, is left out).
std::vector<Pt> vertices(const Path& p);

// Cancellation poll every `every` steps.
inline Status poll(JobContext& ctx, std::size_t step, std::size_t every = 256)
{
    if (step % every == 0)
        return ctx.cancel.check();
    return {};
}

} // namespace ray::cam::geom2d::detail
