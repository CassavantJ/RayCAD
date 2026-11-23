// Helpers of the linking tests (task T-051): paths and passes in integer nanometres, options, a portable seeded random
// generator, the integer text form of moves (the golden-file format), and an independent height-field check of a
// linked toolpath (its own arithmetic: nothing of src/cam's geometry) for the two acceptance properties.
#pragma once
#include "cam/link.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ray::cam::link::test {

using geom2d::Region;
using geom2d::Span;
using geom2d::SpanKind;

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
inline Path openPath(Pt start, std::vector<Span> spans)
{
    Path p;
    p.start = start;
    p.spans = std::move(spans);
    return p;
}
inline Path closedPath(Pt start, std::vector<Span> spans)
{
    Path p = openPath(start, std::move(spans));
    p.closed = true;
    return p;
}
// Counter-clockwise rectangle from its lower-left corner.
inline Path rect(Coord x0, Coord y0, Coord x1, Coord y1)
{
    return closedPath({x0, y0}, {line({x1, y0}), line({x1, y1}), line({x0, y1}), line({x0, y0})});
}
inline Regions regionOf(Path outer, std::vector<Path> holes = {})
{
    return Regions{Region{std::move(outer), std::move(holes)}};
}
inline Stock boxStock(Coord x0, Coord y0, Coord x1, Coord y1, Nm top)
{
    Stock s;
    s.region = regionOf(rect(x0, y0, x1, y1));
    s.top = top;
    return s;
}

// Heights 10 / 5 / 1 mm above a stock top at 0, a 6 mm tool, feeds 1000 mm/min cut.
inline Options baseOptions(Coord toolDiameter = 6 * kMm)
{
    Options o;
    o.clearance = 10 * kMm;
    o.retract = 5 * kMm;
    o.feedPlane = 1 * kMm;
    o.toolRadius = toolDiameter / 2;
    o.feeds.plunge = 300'000;
    o.feeds.ramp = 500'000;
    o.feeds.leadIn = 800'000;
    o.feeds.leadOut = 900'000;
    o.feeds.link = 1'200'000;
    return o;
}
inline constexpr std::int32_t kCutFeed = 1'000'000;

// splitmix64: the same numbers on every platform.
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
    bool chance(double p) { return uniform() < p; }
};

// ---------- Text (golden-file format): one move per line, integers only ----------
inline const char* kindText(MoveKind k)
{
    switch (k) {
    case MoveKind::Rapid:
        return "G0";
    case MoveKind::Linear:
        return "G1";
    case MoveKind::ArcCW:
        return "G2";
    case MoveKind::ArcCCW:
        return "G3";
    default:
        return "?";
    }
}
inline const char* roleText(MoveRole r)
{
    static const char* const names[] = {"cut", "plunge", "ramp", "leadin", "leadout", "link", "retract", "approach"};
    return names[static_cast<std::size_t>(r) % 8];
}
inline std::string toText(const Linked& l)
{
    std::string t =
        "start " + std::to_string(l.start.x) + " " + std::to_string(l.start.y) + " " + std::to_string(l.start.z) + "\n";
    for (const Move& m : l.moves) {
        t += std::string(kindText(m.kind)) + " " + roleText(m.role) + " " + std::to_string(m.x) + " " +
             std::to_string(m.y) + " " + std::to_string(m.z);
        if (m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW)
            t += " c " + std::to_string(m.i) + " " + std::to_string(m.j);
        if (m.feedMicronPerMin != 0)
            t += " f " + std::to_string(m.feedMicronPerMin);
        if (m.compensation != 0)
            t += " comp " + std::to_string(m.compensation);
        t += "\n";
    }
    static const char* const entered[] = {"free", "staydown", "step", "helix", "ramp", "predrill", "plunge"};
    for (std::size_t k = 0; k < l.order.size(); ++k)
        t += "pass " + std::to_string(l.order[k].pass) + (l.order[k].reversed ? " reversed" : "") + " entered " +
             entered[static_cast<std::size_t>(l.entered[k]) % 7] + " at move " + std::to_string(l.passMove[k]) + "\n";
    for (const std::string& w : l.warnings)
        t += "warning " + w + "\n";
    return t;
}

// ---------- The independent height field ----------
// Material is a top height per square cell (its centre), the stock box (or `region` rectangles) up to the stock top.
// Moves are replayed in order: cutting roles (Cut, Plunge, Ramp, LeadIn, LeadOut) lower every cell whose centre lies
// within the tool radius of the move's XY path to the move's height there (a sloped piece to its higher end, so the
// field never removes more than the tool does); every other move (rapids, links, retracts, approaches) must not meet
// material: no cell within (radius - margin) of its path stands above its lower end. Arcs are walked as chords with a
// sagitta of at most 0.5 um. All arithmetic is plain double on nanometre values; nothing of geom2d.
struct FieldIssue {
    std::size_t move = 0;
    std::string what;
    double x = 0, y = 0, z = 0, top = 0;
};
class HeightField {
public:
    struct P3 {
        double x, y, z;
    };
    HeightField(Coord x0, Coord y0, Coord x1, Coord y1, Nm top, Coord cell)
        : x0_(static_cast<double>(x0)), y0_(static_cast<double>(y0)), cell_(static_cast<double>(cell)),
          nx_(static_cast<int>((x1 - x0) / cell)), ny_(static_cast<int>((y1 - y0) / cell))
    {
        top_.assign(static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_), static_cast<double>(top));
        bx0_ = static_cast<double>(x0);
        by0_ = static_cast<double>(y0);
        bx1_ = static_cast<double>(x1);
        by1_ = static_cast<double>(y1);
    }
    // A known free area: a circle cut down to z.
    void clearCircle(Pt c, Coord r, Nm z)
    {
        forCells(static_cast<double>(c.x) - r, static_cast<double>(c.y) - r, static_cast<double>(c.x) + r,
                 static_cast<double>(c.y) + r, [&](double x, double y, double& t) {
                     if (std::hypot(x - static_cast<double>(c.x), y - static_cast<double>(c.y)) <=
                         static_cast<double>(r))
                         t = std::min(t, static_cast<double>(z));
                 });
    }

    struct Replay {
        Coord radius = 0;
        double margin = 2000; // nm
        Nm feedPlane = 0;
        bool rapidRetract = false;
    };
    // A step-in move (Entered::Step) may cut only what the pass it goes to cuts anyway: within the tool radius of the
    // pass's XY path (its lead-in, cut and lead-out moves, given as points every few micrometres along it).
    struct StepCheck {
        std::size_t move = 0;
        std::vector<std::pair<double, double>> path;
    };
    // Every issue (at most `max`).
    std::vector<FieldIssue> replay(const Linked& l, const Replay& r, const std::vector<StepCheck>& steps = {},
                                   std::size_t max = 20)
    {
        std::vector<FieldIssue> out;
        P3 cur{static_cast<double>(l.start.x), static_cast<double>(l.start.y), static_cast<double>(l.start.z)};
        const double rad = static_cast<double>(r.radius);
        for (std::size_t k = 0; k < l.moves.size() && out.size() < max; ++k) {
            const Move& m = l.moves[k];
            const P3 end{static_cast<double>(m.x), static_cast<double>(m.y), static_cast<double>(m.z)};
            const std::vector<P3> pts = walk(cur, m);
            for (const StepCheck& s : steps)
                if (s.move == k && cutsOutside(cur, end, rad - r.margin, s.path, rad + r.margin) > 0)
                    out.push_back({k, "a step-in cuts outside what its pass cuts", end.x, end.y, end.z, 0});
            const bool cuts = m.kind != MoveKind::Rapid &&
                              (m.role == MoveRole::Cut || m.role == MoveRole::Plunge || m.role == MoveRole::Ramp ||
                               m.role == MoveRole::LeadIn || m.role == MoveRole::LeadOut);
            if (m.kind == MoveKind::Rapid) {
                const bool vertical = m.x == static_cast<Nm>(cur.x) && m.y == static_cast<Nm>(cur.y);
                const bool up = end.z > cur.z;
                const double lowest = std::min(cur.z, end.z);
                if (lowest < static_cast<double>(r.feedPlane) && !(vertical && up && r.rapidRetract)) {
                    // Below the feed plane: only outside the stock bounds (grown by the radius).
                    for (const P3& p : pts) {
                        if (p.x > bx0_ - rad && p.x < bx1_ + rad && p.y > by0_ - rad && p.y < by1_ + rad &&
                            p.z < static_cast<double>(r.feedPlane)) {
                            out.push_back({k, "rapid below the feed plane over the stock", p.x, p.y, p.z, 0});
                            break;
                        }
                    }
                }
            }
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                const P3 a = pts[i], b = pts[i + 1];
                if (cuts) {
                    const double z = std::max(a.z, b.z);
                    segmentCells(a, b, rad, [&](double, double, double& t) { t = std::min(t, z); });
                } else {
                    const double z = std::min(a.z, b.z);
                    bool hit = false;
                    segmentCells(a, b, rad - r.margin, [&](double x, double y, double& t) {
                        if (!hit && t > z + 1.0) {
                            out.push_back({k, "a non-cutting move meets material", x, y, z, t});
                            hit = true;
                        }
                    });
                    if (hit)
                        break;
                }
            }
            cur = end;
        }
        return out;
    }

    // The cells within `radius` of the segment from..to standing above its height and farther than `reach` from every
    // point of `path`.
    std::size_t cutsOutside(P3 from, P3 to, double radius, const std::vector<std::pair<double, double>>& path,
                            double reach)
    {
        std::size_t n = 0;
        segmentCells(from, to, radius, [&](double x, double y, double& t) {
            if (t <= to.z + 1.0)
                return;
            for (std::size_t i = 0; i + 1 < path.size(); ++i) {
                const double ax = path[i].first, ay = path[i].second;
                const double dx = path[i + 1].first - ax, dy = path[i + 1].second - ay, l2 = dx * dx + dy * dy;
                const double u = l2 > 0 ? std::clamp(((x - ax) * dx + (y - ay) * dy) / l2, 0.0, 1.0) : 0.0;
                if (std::hypot(x - ax - u * dx, y - ay - u * dy) <= reach)
                    return;
            }
            ++n;
        });
        return n;
    }

    // The XY points of moves k0.. while their role is a pass's own (lead-in, cut, lead-out).
    static std::vector<std::pair<double, double>> passPath(const Linked& l, std::size_t k0)
    {
        std::vector<std::pair<double, double>> pts;
        const Move& before = l.moves[k0 - 1];
        P3 cur{static_cast<double>(before.x), static_cast<double>(before.y), static_cast<double>(before.z)};
        pts.emplace_back(cur.x, cur.y);
        for (std::size_t k = k0; k < l.moves.size(); ++k) {
            const Move& m = l.moves[k];
            if (m.role != MoveRole::LeadIn && m.role != MoveRole::Cut && m.role != MoveRole::LeadOut)
                break;
            for (const P3& p : walk(cur, m))
                pts.emplace_back(p.x, p.y);
            cur = P3{static_cast<double>(m.x), static_cast<double>(m.y), static_cast<double>(m.z)};
        }
        return pts;
    }

private:
    template <class F>
    void forCells(double xa, double ya, double xb, double yb, F f)
    {
        const int i0 = std::max(0, static_cast<int>(std::floor((xa - x0_) / cell_ - 0.5)));
        const int i1 = std::min(nx_ - 1, static_cast<int>(std::ceil((xb - x0_) / cell_ - 0.5)));
        const int j0 = std::max(0, static_cast<int>(std::floor((ya - y0_) / cell_ - 0.5)));
        const int j1 = std::min(ny_ - 1, static_cast<int>(std::ceil((yb - y0_) / cell_ - 0.5)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const double x = x0_ + (i + 0.5) * cell_, y = y0_ + (j + 0.5) * cell_;
                f(x, y,
                  top_[static_cast<std::size_t>(j) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(i)]);
            }
    }

    template <class F>
    void segmentCells(P3 a, P3 b, double rad, F f)
    {
        if (rad <= 0)
            return;
        const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
        forCells(std::min(a.x, b.x) - rad, std::min(a.y, b.y) - rad, std::max(a.x, b.x) + rad, std::max(a.y, b.y) + rad,
                 [&](double x, double y, double& t) {
                     double u = l2 > 0 ? ((x - a.x) * dx + (y - a.y) * dy) / l2 : 0.0;
                     u = std::clamp(u, 0.0, 1.0);
                     if (std::hypot(x - a.x - u * dx, y - a.y - u * dy) <= rad)
                         f(x, y, t);
                 });
    }

    // Points along a move: its ends, arcs and sloped lines in small steps.
    static std::vector<P3> walk(P3 from, const Move& m)
    {
        const P3 to{static_cast<double>(m.x), static_cast<double>(m.y), static_cast<double>(m.z)};
        std::vector<P3> pts{from};
        if (m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW) {
            const double cx = static_cast<double>(m.i), cy = static_cast<double>(m.j);
            const double r = std::hypot(from.x - cx, from.y - cy);
            const double a0 = std::atan2(from.y - cy, from.x - cx), a1 = std::atan2(to.y - cy, to.x - cx);
            const bool ccw = m.kind == MoveKind::ArcCCW;
            double sweep = ccw ? a1 - a0 : a0 - a1;
            while (sweep <= 0)
                sweep += 2 * kPiD;
            if (from.x == to.x && from.y == to.y)
                sweep = 2 * kPiD;
            const double step = 2 * std::sqrt(2 * r * 500.0) / std::max(r, 1.0); // chord sagitta 0.5 um
            const int n = std::max(2, static_cast<int>(std::ceil(sweep / step)));
            for (int k = 1; k < n; ++k) {
                const double a = a0 + (ccw ? 1 : -1) * sweep * k / n;
                pts.push_back(P3{cx + r * std::cos(a), cy + r * std::sin(a), from.z + (to.z - from.z) * k / n});
            }
        } else if (from.z != to.z) {
            const int n = std::max(1, static_cast<int>(std::ceil(std::abs(to.z - from.z) / 20'000.0)));
            for (int k = 1; k < n; ++k)
                pts.push_back(P3{from.x + (to.x - from.x) * k / n, from.y + (to.y - from.y) * k / n,
                                 from.z + (to.z - from.z) * k / n});
        }
        pts.push_back(to);
        return pts;
    }

    double x0_, y0_, cell_;
    int nx_, ny_;
    double bx0_, by0_, bx1_, by1_;
    std::vector<double> top_;
};

inline std::string describe(const std::vector<FieldIssue>& issues)
{
    std::string s;
    for (const FieldIssue& i : issues)
        s += "move " + std::to_string(i.move) + ": " + i.what + " at (" + std::to_string(i.x / 1e6) + ", " +
             std::to_string(i.y / 1e6) + ", " + std::to_string(i.z / 1e6) + ") mm, material top " +
             std::to_string(i.top / 1e6) + "\n";
    return s;
}

// The cut moves of a pass as given appear in the output, in order, contiguously (for a pass not reversed or rotated).
inline bool containsRun(const std::vector<Move>& out, const std::vector<Move>& run)
{
    if (run.empty())
        return true;
    for (std::size_t k = 0; k + run.size() <= out.size(); ++k) {
        bool same = true;
        for (std::size_t i = 0; i < run.size() && same; ++i) {
            const Move &a = out[k + i], &b = run[i];
            same = a.kind == b.kind && a.role == b.role && a.x == b.x && a.y == b.y && a.z == b.z && a.i == b.i &&
                   a.j == b.j && a.feedMicronPerMin == b.feedMicronPerMin && a.compensation == b.compensation;
        }
        if (same)
            return true;
    }
    return false;
}

} // namespace ray::cam::link::test
