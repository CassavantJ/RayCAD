// Helpers shared by the geom2d tests (task T-050): shapes in integer nanometres, a portable seeded random generator,
// and an independent distance from a point to a path (its own arithmetic, not geom2d's) to measure deviations.
#pragma once
#include "cam/geom2d.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ray::cam::geom2d::test {

inline constexpr Coord kMm = 1'000'000;
inline constexpr double kPiD = 3.141592653589793238462643383279502884;

template <class T>
T take(Result<T> r)
{
    INFO("unexpected error: " << (r.has_value() ? std::string() : r.error().message));
    REQUIRE(r.has_value());
    return std::move(*r);
}

inline Span line(Pt end)
{
    return Span{SpanKind::Line, end, {}, 0};
}
inline Span arc(Pt end, Pt centre, Coord r, bool ccw)
{
    return Span{ccw ? SpanKind::ArcCCW : SpanKind::ArcCW, end, centre, r};
}
inline Path closedPath(Pt start, std::vector<Span> spans)
{
    Path p;
    p.start = start;
    p.spans = std::move(spans);
    p.closed = true;
    return p;
}
inline Path openPath(Pt start, std::vector<Span> spans)
{
    Path p = closedPath(start, std::move(spans));
    p.closed = false;
    return p;
}
// Counter-clockwise rectangle starting at its lower-left corner.
inline Path rect(Coord x0, Coord y0, Coord x1, Coord y1)
{
    return closedPath({x0, y0}, {line({x1, y0}), line({x1, y1}), line({x0, y1}), line({x0, y0})});
}
// Counter-clockwise rectangle with corner radius r, starting at its lowest vertex (x0, y0 + r).
inline Path roundedRect(Coord x0, Coord y0, Coord x1, Coord y1, Coord r)
{
    return closedPath({x0, y0 + r}, {arc({x0 + r, y0}, {x0 + r, y0 + r}, r, true), line({x1 - r, y0}),
                                     arc({x1, y0 + r}, {x1 - r, y0 + r}, r, true), line({x1, y1 - r}),
                                     arc({x1 - r, y1}, {x1 - r, y1 - r}, r, true), line({x0 + r, y1}),
                                     arc({x0, y1 - r}, {x0 + r, y1 - r}, r, true), line({x0, y0 + r})});
}
// Counter-clockwise obround about the segment (x0, y) - (x1, y), radius r, starting at (x0, y - r).
inline Path slot(Coord x0, Coord x1, Coord y, Coord r)
{
    return closedPath({x0, y - r}, {line({x1, y - r}), arc({x1, y + r}, {x1, y}, r, true), line({x0, y + r}),
                                    arc({x0, y - r}, {x0, y}, r, true)});
}
inline Region region(Path outer, std::vector<Path> holes = {})
{
    return Region{std::move(outer), std::move(holes)};
}

// splitmix64: the same numbers on every platform (std distributions are not specified bit for bit).
struct Rng {
    std::uint64_t state;
    std::uint64_t next()
    {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double between(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    template <std::integral A, std::integral B>
    Coord between(A lo, B hi)
    {
        const Coord l = static_cast<Coord>(lo), h = static_cast<Coord>(hi);
        return l + static_cast<Coord>(next() % static_cast<std::uint64_t>(h - l + 1));
    }
};

// ---------- An independent distance from a point to a path ----------
struct P2 {
    double x, y;
};
inline double dist(P2 a, P2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}
inline P2 p2(Pt p)
{
    return P2{static_cast<double>(p.x), static_cast<double>(p.y)};
}
inline double segmentDistance(P2 p, P2 a, P2 b)
{
    const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    if (l2 <= 0)
        return dist(p, a);
    const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0);
    return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}
inline double arcSweep(P2 a, P2 b, P2 c, bool ccw, bool whole)
{
    if (whole)
        return 2 * kPiD;
    const double a0 = std::atan2(a.y - c.y, a.x - c.x), a1 = std::atan2(b.y - c.y, b.x - c.x);
    double s = ccw ? a1 - a0 : a0 - a1;
    while (s <= 0)
        s += 2 * kPiD;
    return s;
}
inline double arcDistance(P2 p, P2 a, P2 b, P2 c, double r, bool ccw, bool whole)
{
    const double sweep = arcSweep(a, b, c, ccw, whole);
    const double a0 = std::atan2(a.y - c.y, a.x - c.x), ap = std::atan2(p.y - c.y, p.x - c.x);
    double s = ccw ? ap - a0 : a0 - ap;
    while (s < 0)
        s += 2 * kPiD;
    while (s >= 2 * kPiD)
        s -= 2 * kPiD;
    if (s <= sweep)
        return std::abs(dist(p, c) - r);
    return std::min(dist(p, a), dist(p, b));
}
inline double pathDistance(P2 p, const Path& path)
{
    double best = std::numeric_limits<double>::infinity();
    Pt cur = path.start;
    if (path.spans.empty())
        return dist(p, p2(cur));
    for (const Span& s : path.spans) {
        if (s.kind == SpanKind::Line)
            best = std::min(best, segmentDistance(p, p2(cur), p2(s.end)));
        else
            best = std::min(best, arcDistance(p, p2(cur), p2(s.end), p2(s.centre), static_cast<double>(s.radius),
                                              s.kind == SpanKind::ArcCCW, s.end == cur));
        cur = s.end;
    }
    return best;
}
// Points along a path, about `perSpan` per span (and its ends).
inline std::vector<P2> samples(const Path& path, int perSpan)
{
    std::vector<P2> out{p2(path.start)};
    Pt cur = path.start;
    for (const Span& s : path.spans) {
        const P2 a = p2(cur), b = p2(s.end);
        if (s.kind == SpanKind::Line) {
            for (int k = 1; k <= perSpan; ++k)
                out.push_back(P2{a.x + (b.x - a.x) * k / perSpan, a.y + (b.y - a.y) * k / perSpan});
        } else {
            const P2 c = p2(s.centre);
            const bool ccw = s.kind == SpanKind::ArcCCW;
            const double sweep = arcSweep(a, b, c, ccw, s.end == cur);
            const double a0 = std::atan2(a.y - c.y, a.x - c.x);
            const double r = static_cast<double>(s.radius);
            for (int k = 1; k < perSpan; ++k) {
                const double t = a0 + (ccw ? 1 : -1) * sweep * k / perSpan;
                out.push_back(P2{c.x + r * std::cos(t), c.y + r * std::sin(t)});
            }
            out.push_back(b);
        }
        cur = s.end;
    }
    return out;
}
// The largest distance from samples of `a` to `b`.
inline double oneWay(const Path& a, const Path& b, int perSpan = 32)
{
    double worst = 0;
    for (const P2& p : samples(a, perSpan))
        worst = std::max(worst, pathDistance(p, b));
    return worst;
}

// Every arc of every path: endpoints within `tol` nm of its circle.
inline double worstArcEndError(const Regions& rs)
{
    double worst = 0;
    const auto check = [&](const Path& p) {
        Pt cur = p.start;
        for (const Span& s : p.spans) {
            if (s.kind != SpanKind::Line) {
                worst = std::max(worst, std::abs(dist(p2(cur), p2(s.centre)) - static_cast<double>(s.radius)));
                worst = std::max(worst, std::abs(dist(p2(s.end), p2(s.centre)) - static_cast<double>(s.radius)));
            }
            cur = s.end;
        }
    };
    for (const Region& r : rs) {
        check(r.outer);
        for (const Path& h : r.holes)
            check(h);
    }
    return worst;
}

} // namespace ray::cam::geom2d::test
