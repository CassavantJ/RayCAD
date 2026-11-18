// Helpers shared by the sketch tests (task T-090): a sketch builder with ordered ids, an independent geometric check
// of every constraint type, the spike S7/S11 test sketches and small statistics.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <ray/sketch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace ray::sketch::test {

// The value of a Result, failing the test with the error message if there is none.
template <class T>
T take(Result<T> result)
{
    INFO("unexpected error: " << (result.has_value() ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    return std::move(*result);
}

// Ids whose order is creation order (the counter is stored big-endian in the last bytes).
inline Uuid orderedUuid(std::uint64_t n, std::uint8_t kind)
{
    Uuid u;
    u.bytes[0] = kind;
    for (int i = 0; i < 8; ++i) {
        u.bytes[static_cast<std::size_t>(15 - i)] = static_cast<std::uint8_t>(n >> (8 * i));
    }
    return u;
}

struct Builder {
    Sketch s;
    std::uint64_t next = 1;

    SketchEntId add(std::variant<Point, Line, Arc, Circle, Spline, Slot> g, bool construction = false)
    {
        SketchEntId id{orderedUuid(next++, 0x10)};
        s.entities.push_back(Entity{id, std::move(g), construction});
        return id;
    }
    SketchEntId point(double x, double y) { return add(Point{{x, y}}); }
    SketchEntId line(SketchEntId a, SketchEntId b) { return add(Line{a, b}); }
    SketchEntId arc(SketchEntId c, SketchEntId st, SketchEntId en, bool ccw = true) { return add(Arc{c, st, en, ccw}); }
    SketchEntId circle(SketchEntId c, double r) { return add(Circle{c, r}); }
    Uuid constrain(ConstraintType t, std::vector<SketchEntId> refs, double value = 0, bool driven = false)
    {
        Constraint c;
        c.id = orderedUuid(next++, 0x20);
        c.type = t;
        c.refs = std::move(refs);
        c.value = value;
        c.driven = driven;
        s.constraints.push_back(c);
        return c.id;
    }
};

inline const Entity* findEntity(const Sketch& s, SketchEntId id)
{
    for (const Entity& e : s.entities) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}
inline Vec2 pointAt(const Sketch& s, SketchEntId id)
{
    const Entity* e = findEntity(s, id);
    REQUIRE(e);
    REQUIRE(std::holds_alternative<Point>(e->geom));
    return std::get<Point>(e->geom).p;
}

// ---------- An independent check of every constraint, from the geometry alone (no solver code) ----------
namespace oracle {
inline double len(Vec2 v)
{
    return std::sqrt(v.x * v.x + v.y * v.y);
}
inline Vec2 sub(Vec2 a, Vec2 b)
{
    return {a.x - b.x, a.y - b.y};
}
inline double cross(Vec2 a, Vec2 b)
{
    return a.x * b.y - a.y * b.x;
}
inline double dot(Vec2 a, Vec2 b)
{
    return a.x * b.x + a.y * b.y;
}
inline double wrap(double a)
{
    while (a > std::numbers::pi) {
        a -= 2 * std::numbers::pi;
    }
    while (a <= -std::numbers::pi) {
        a += 2 * std::numbers::pi;
    }
    return a;
}
struct Seg {
    Vec2 a, b;
};
struct Circ {
    Vec2 c;
    double r;
};
inline double lineDist(Vec2 p, Seg l)
{
    return std::fabs(cross(sub(l.b, l.a), sub(p, l.a))) / len(sub(l.b, l.a));
}
} // namespace oracle

// The geometric error of constraint `c` in `s` (mm or rad); `original` supplies the coordinates a Fix holds. Arcs are
// circles through their start point. Returns nullopt for a combination the solver does not support.
inline std::optional<double> constraintError(const Sketch& s, const Sketch& original, const Constraint& c)
{
    using namespace oracle;
    std::vector<const Entity*> r;
    for (SketchEntId id : c.refs) {
        r.push_back(findEntity(s, id));
    }
    auto kind = [&](std::size_t i) {
        return r[i]->geom.index();
    }; // 0 point 1 line 2 arc 3 circle
    auto P = [&](std::size_t i) {
        return std::get<Point>(r[i]->geom).p;
    };
    auto L = [&](std::size_t i) {
        const Line& l = std::get<Line>(r[i]->geom);
        return Seg{pointAt(s, l.a), pointAt(s, l.b)};
    };
    auto isC = [&](std::size_t i) {
        return kind(i) == 2 || kind(i) == 3;
    };
    auto C = [&](std::size_t i) {
        if (kind(i) == 3) {
            const Circle& k = std::get<Circle>(r[i]->geom);
            return Circ{pointAt(s, k.center), k.radius};
        }
        const Arc& a = std::get<Arc>(r[i]->geom);
        const Vec2 ctr = pointAt(s, a.center);
        return Circ{ctr, len(sub(pointAt(s, a.start), ctr))};
    };
    const std::size_t n = r.size();
    switch (c.type) {
    case ConstraintType::Coincident:
        return len(sub(P(0), P(1)));
    case ConstraintType::Horizontal:
    case ConstraintType::Vertical: {
        const bool h = c.type == ConstraintType::Horizontal;
        Vec2 a, b;
        if (n == 1) {
            a = L(0).a;
            b = L(0).b;
        } else {
            a = P(0);
            b = P(1);
        }
        return h ? std::fabs(a.y - b.y) : std::fabs(a.x - b.x);
    }
    case ConstraintType::Parallel:
    case ConstraintType::Perpendicular: {
        const Vec2 d1 = sub(L(0).b, L(0).a), d2 = sub(L(1).b, L(1).a);
        const double v = c.type == ConstraintType::Parallel ? cross(d1, d2) : dot(d1, d2);
        return std::fabs(v) / (len(d1) * len(d2));
    }
    case ConstraintType::Tangent:
        if (n == 2) {
            for (std::size_t i = 0; i < 2; ++i) {
                if (kind(i) == 1 && isC(1 - i)) {
                    return std::fabs(lineDist(C(1 - i).c, L(i)) - C(1 - i).r);
                }
            }
            const Circ a = C(0), b = C(1);
            const double d = len(sub(a.c, b.c));
            return std::min(std::fabs(d - (a.r + b.r)), std::fabs(d - std::fabs(a.r - b.r)));
        } else {
            // the curves' normals at the point are parallel
            const Vec2 p = P(2);
            auto normal = [&](std::size_t i) {
                if (kind(i) == 1) {
                    const Vec2 d = sub(L(i).b, L(i).a);
                    return Vec2{-d.y, d.x};
                }
                return sub(p, C(i).c);
            };
            const Vec2 n1 = normal(0), n2 = normal(1);
            return std::fabs(cross(n1, n2)) / (len(n1) * len(n2));
        }
    case ConstraintType::Equal:
        if (kind(0) == 1) {
            return std::fabs(len(sub(L(0).b, L(0).a)) - len(sub(L(1).b, L(1).a)));
        }
        return std::fabs(C(0).r - C(1).r);
    case ConstraintType::Symmetric: {
        const Vec2 a = P(0), b = P(1);
        const Vec2 mid{(a.x + b.x) / 2, (a.y + b.y) / 2};
        if (kind(2) == 0) {
            return len(sub(mid, P(2)));
        }
        const Seg l = L(2);
        const Vec2 d = sub(l.b, l.a);
        return lineDist(mid, l) + std::fabs(dot(sub(b, a), d)) / len(d);
    }
    case ConstraintType::Concentric:
        return len(sub(C(0).c, C(1).c));
    case ConstraintType::Midpoint: {
        const std::size_t pi = kind(0) == 0 ? 0 : 1;
        const Seg l = L(1 - pi);
        return len(sub(P(pi), Vec2{(l.a.x + l.b.x) / 2, (l.a.y + l.b.y) / 2}));
    }
    case ConstraintType::PointOnCurve: {
        const std::size_t pi = kind(0) == 0 ? 0 : 1;
        if (kind(1 - pi) == 1) {
            return lineDist(P(pi), L(1 - pi));
        }
        return std::fabs(len(sub(P(pi), C(1 - pi).c)) - C(1 - pi).r);
    }
    case ConstraintType::Fix: {
        double e = 0;
        auto fixed = [&](SketchEntId id) {
            e = std::max(e, len(sub(pointAt(s, id), pointAt(original, id))));
        };
        for (const Entity* ent : r) {
            if (const auto* l = std::get_if<Line>(&ent->geom)) {
                fixed(l->a);
                fixed(l->b);
            } else if (const auto* a = std::get_if<Arc>(&ent->geom)) {
                fixed(a->center);
                fixed(a->start);
                fixed(a->end);
            } else if (const auto* k = std::get_if<Circle>(&ent->geom)) {
                fixed(k->center);
                e = std::max(e, std::fabs(k->radius - std::get<Circle>(findEntity(original, ent->id)->geom).radius));
            } else if (std::holds_alternative<Point>(ent->geom)) {
                fixed(ent->id);
            }
        }
        return e;
    }
    case ConstraintType::Distance: {
        double d = 0;
        if (n == 1) {
            d = len(sub(L(0).b, L(0).a));
        } else if (kind(0) == 0 && kind(1) == 0) {
            d = len(sub(P(0), P(1)));
        } else {
            bool done = false;
            for (std::size_t i = 0; i < 2 && !done; ++i) {
                const std::size_t j = 1 - i;
                if (kind(i) == 0 && kind(j) == 1) {
                    d = lineDist(P(i), L(j));
                    done = true;
                } else if (kind(i) == 0 && isC(j)) {
                    d = std::fabs(len(sub(P(i), C(j).c)) - C(j).r);
                    done = true;
                } else if (isC(i) && kind(j) == 1) {
                    d = std::fabs(lineDist(C(i).c, L(j)) - C(i).r);
                    done = true;
                }
            }
            if (!done) {
                const Circ a = C(0), b = C(1);
                const double dc = len(sub(a.c, b.c));
                d = (dc >= a.r && dc >= b.r) ? dc - a.r - b.r : std::max(a.r, b.r) - std::min(a.r, b.r) - dc;
            }
        }
        return std::fabs(d - c.value);
    }
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY: {
        const bool x = c.type == ConstraintType::DistanceX;
        double v;
        if (n == 1 && kind(0) == 0) {
            v = x ? P(0).x : P(0).y;
        } else if (n == 1) {
            v = x ? L(0).b.x - L(0).a.x : L(0).b.y - L(0).a.y;
        } else {
            v = x ? P(1).x - P(0).x : P(1).y - P(0).y;
        }
        return std::fabs(v - c.value);
    }
    case ConstraintType::Radius:
        return std::fabs(C(0).r - c.value);
    case ConstraintType::Diameter:
        return std::fabs(2 * C(0).r - c.value);
    case ConstraintType::Angle: {
        auto dir = [&](std::size_t i) {
            const Vec2 d = sub(L(i).b, L(i).a);
            return std::atan2(d.y, d.x);
        };
        const double a = n == 1 ? dir(0) : dir(1) - dir(0);
        return std::fabs(wrap(a - c.value));
    }
    }
    return std::nullopt;
}

// The largest error over every enforced constraint, and over the arc consistency (start and end at the same radius).
inline double maxError(const Sketch& s, const Sketch& original)
{
    double e = 0;
    for (const Constraint& c : s.constraints) {
        if (c.driven) {
            continue;
        }
        const auto v = constraintError(s, original, c);
        REQUIRE(v.has_value());
        e = std::max(e, *v);
    }
    for (const Entity& ent : s.entities) {
        if (const auto* a = std::get_if<Arc>(&ent.geom)) {
            const Vec2 ctr = pointAt(s, a->center);
            e = std::max(e, std::fabs(oracle::len(oracle::sub(pointAt(s, a->start), ctr)) -
                                      oracle::len(oracle::sub(pointAt(s, a->end), ctr))));
        }
    }
    return e;
}

// Every coordinate (points, circle radii) of a sketch, in entity order.
inline std::vector<double> coordinates(const Sketch& s)
{
    std::vector<double> v;
    for (const Entity& e : s.entities) {
        if (const auto* p = std::get_if<Point>(&e.geom)) {
            v.push_back(p->p.x);
            v.push_back(p->p.y);
        } else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            v.push_back(c->radius);
        }
    }
    return v;
}
inline double maxDiff(const std::vector<double>& a, const std::vector<double>& b)
{
    REQUIRE(a.size() == b.size());
    double d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        d = std::max(d, std::fabs(a[i] - b[i]));
    }
    return d;
}
inline std::uint64_t fnv(std::uint64_t h, const std::vector<double>& v)
{
    const auto* b = reinterpret_cast<const unsigned char*>(v.data());
    for (std::size_t i = 0; i < v.size() * sizeof(double); ++i) {
        h = (h ^ b[i]) * 1099511628211ULL;
    }
    return h;
}
constexpr std::uint64_t kFnvBasis = 1469598103934665603ULL;

struct Lcg {
    std::uint64_t s;
    double next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
    }
    double sym(double amp) { return (next() * 2.0 - 1.0) * amp; }
    double range(double lo, double hi) { return lo + next() * (hi - lo); }
    int below(int n) { return static_cast<int>(next() * n) % n; }
};

// Order statistics of timings (ms).
struct Stats {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double pct(double p)
    {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0 : v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())))];
    }
    double median() { return pct(0.5); }
    double worst() { return pct(1.0); }
};
using Clock = std::chrono::steady_clock;

// True for an optimised build without sanitizers: the only builds in which solver timings mean anything, and in which
// the long tests run at full size.
inline bool optimisedBuild()
{
#if !defined(NDEBUG) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
    return false;
#else
    return true;
#endif
#else
    return true;
#endif
}
inline double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---------- The spike S7/S11 sketches ----------
// A 5 x rows grid of rounded rectangles (4 lines, 4 arcs, 2 diagonals: 10 curves each), every unit linked to its
// neighbour, so the sketch is one coupled group of 50 * rows curves. Same recipe and order as
// spikes/s11-sketch-drag/src/bench.cpp: endpoint tangency is a coincident constraint plus a tangent-at-point
// constraint (PlaneGCS angle-via-point), as FreeCAD does it.
enum class GridVariant { Nearly, Fully, Loose, Decoupled, Redundant };
struct GridUnit {
    SketchEntId bottom, right, top, left;
    SketchEntId bottomP2, rightP2;
    SketchEntId blCenter;
};
struct Grid {
    Sketch sketch;
    std::vector<GridUnit> units;
    int curves = 0;
};

inline Grid makeGrid(int rows, GridVariant variant, double noise, std::uint64_t seed = 12345)
{
    constexpr int kCols = 5;
    constexpr double kW = 30, kH = 20, kR = 4, kPitchX = 40, kPitchY = 30, pi = std::numbers::pi;
    const bool nearly = variant == GridVariant::Nearly || variant == GridVariant::Redundant;
    const bool loose = variant == GridVariant::Loose;
    const bool dec = variant == GridVariant::Decoupled;
    const int nUnits = kCols * rows;
    auto freePitch = [&](int i) {
        for (int c : {nUnits - 1, 2 * nUnits / 3, nUnits / 3}) {
            if (i == (c % 5 == 0 ? c + 1 : c)) {
                return true;
            }
        }
        return false;
    };
    Lcg rng{seed};
    auto n = [&] {
        return noise > 0 ? rng.sym(noise) : 0.0;
    };
    Builder b;
    struct LineE {
        SketchEntId id, p1, p2;
    };
    struct ArcE {
        SketchEntId id, center, start, end;
    };
    auto addLine = [&](double x1, double y1, double x2, double y2) {
        LineE l;
        l.p1 = b.point(x1 + n(), y1 + n());
        l.p2 = b.point(x2 + n(), y2 + n());
        l.id = b.line(l.p1, l.p2);
        return l;
    };
    auto addArc = [&](double cx, double cy, double r, double a0, double a1) {
        ArcE a;
        a.center = b.point(cx + n(), cy + n());
        a.start = b.point(cx + r * std::cos(a0) + n(), cy + r * std::sin(a0) + n());
        a.end = b.point(cx + r * std::cos(a1) + n(), cy + r * std::sin(a1) + n());
        a.id = b.arc(a.center, a.start, a.end, true);
        return a;
    };
    struct U {
        ArcE bl, br, tr, tl;
        LineE bottom, right, top, left, d1, d2;
    };
    std::vector<U> us;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const double ox = col * kPitchX, oy = row * kPitchY, w = kW, h = kH, r = kR;
            U u;
            u.bl = addArc(ox + r, oy + r, r, pi, 1.5 * pi);
            u.bottom = addLine(ox + r, oy, ox + w - r, oy);
            u.br = addArc(ox + w - r, oy + r, r, 1.5 * pi, 2 * pi);
            u.right = addLine(ox + w, oy + r, ox + w, oy + h - r);
            u.tr = addArc(ox + w - r, oy + h - r, r, 0, 0.5 * pi);
            u.top = addLine(ox + w - r, oy + h, ox + r, oy + h);
            u.tl = addArc(ox + r, oy + h - r, r, 0.5 * pi, pi);
            u.left = addLine(ox, oy + h - r, ox, oy + r);
            u.d1 = addLine(ox + r, oy + r, ox + w - r, oy + h - r);
            u.d2 = addLine(ox + w - r, oy + r, ox + r, oy + h - r);
            us.push_back(u);
        }
    }
    using CT = ConstraintType;
    auto tangentAtEnd = [&](const LineE& l, SketchEntId lp, const ArcE& a, SketchEntId ap) {
        b.constrain(CT::Coincident, {lp, ap});
        b.constrain(CT::Tangent, {l.id, a.id, lp});
    };
    for (int i = 0; i < nUnits; ++i) {
        const U& u = us[static_cast<std::size_t>(i)];
        const int row = i / kCols, col = i % kCols;
        tangentAtEnd(u.bottom, u.bottom.p1, u.bl, u.bl.end);
        tangentAtEnd(u.bottom, u.bottom.p2, u.br, u.br.start);
        tangentAtEnd(u.right, u.right.p1, u.br, u.br.end);
        tangentAtEnd(u.right, u.right.p2, u.tr, u.tr.start);
        tangentAtEnd(u.top, u.top.p1, u.tr, u.tr.end);
        tangentAtEnd(u.top, u.top.p2, u.tl, u.tl.start);
        tangentAtEnd(u.left, u.left.p1, u.tl, u.tl.end);
        tangentAtEnd(u.left, u.left.p2, u.bl, u.bl.start);
        b.constrain(CT::Coincident, {u.d1.p1, u.bl.center});
        b.constrain(CT::Coincident, {u.d1.p2, u.tr.center});
        b.constrain(CT::Coincident, {u.d2.p1, u.br.center});
        b.constrain(CT::Coincident, {u.d2.p2, u.tl.center});
        b.constrain(CT::Horizontal, {u.bottom.id});
        b.constrain(CT::Vertical, {u.left.id});
        b.constrain(CT::Parallel, {u.top.id, u.bottom.id});
        b.constrain(CT::Parallel, {u.right.id, u.left.id});
        b.constrain(CT::Equal, {u.br.id, u.bl.id});
        b.constrain(CT::Equal, {u.tr.id, u.bl.id});
        b.constrain(CT::Equal, {u.tl.id, u.bl.id});
        const U* prev = i > 0 ? &us[static_cast<std::size_t>(i - 1)] : nullptr;
        const bool last = i == nUnits - 1;
        if (i % 4 == 0 || dec) {
            b.constrain(CT::Radius, {u.bl.id}, kR);
        } else {
            b.constrain(CT::Equal, {u.bl.id, prev->bl.id});
        }
        if (dec) {
            if (!last) {
                b.constrain(CT::Distance, {u.bottom.id}, kW - 2 * kR);
            }
            b.constrain(CT::Distance, {u.left.id}, kH - 2 * kR);
        } else if (i % 2 == 0) {
            if (!(nearly && i == 0) && !loose) {
                b.constrain(CT::Distance, {u.bottom.id}, kW - 2 * kR);
                b.constrain(CT::Distance, {u.left.id}, kH - 2 * kR);
            }
        } else {
            b.constrain(CT::Equal, {u.bottom.id, prev->bottom.id});
            b.constrain(CT::Equal, {u.left.id, prev->left.id});
        }
        if (i == 0 || dec) {
            b.constrain(CT::DistanceX, {u.bl.center}, col * kPitchX + kR);
            b.constrain(CT::DistanceY, {u.bl.center}, row * kPitchY + kR);
        } else if (col > 0) {
            b.constrain(CT::Horizontal, {u.bl.center, prev->bl.center});
            if (!(nearly && freePitch(i)) && !loose) {
                b.constrain(CT::Distance, {u.bl.center, prev->bl.center}, kPitchX);
            }
        } else {
            const U& below = us[static_cast<std::size_t>(i - kCols)];
            b.constrain(CT::Vertical, {u.bl.center, below.bl.center});
            if (!loose) {
                b.constrain(CT::Distance, {u.bl.center, below.bl.center}, kPitchY);
            }
        }
    }
    if (variant == GridVariant::Redundant) { // the top of unit 3 is already parallel to its horizontal bottom
        b.constrain(CT::Horizontal, {us[3].top.id});
    }
    Grid g;
    g.sketch = std::move(b.s);
    g.curves = nUnits * 10;
    for (const U& u : us) {
        g.units.push_back(GridUnit{u.bottom.id, u.right.id, u.top.id, u.left.id, u.bottom.p2, u.right.p2, u.bl.center});
    }
    return g;
}

// The six drag cases of spike S11 (spikes/s11-sketch-drag/src/bench.cpp, kDrags).
struct DragCase {
    const char* name;
    GridVariant variant;
    int unit;  // -1 = last
    int which; // 0 bottom.p2, 1 right.p2
    double dx, dy;
};
inline const std::vector<DragCase>& dragCases()
{
    static const std::vector<DragCase> cases = {
        {"D1 slide (nearly constrained, last unit bottom.p2)", GridVariant::Nearly, -1, 0, 0.10, 0.05},
        {"D2 resize (nearly constrained, unit 0 right.p2)", GridVariant::Nearly, 0, 1, 0.05, 0.03},
        {"D3 blocked (fully constrained, last unit bottom.p2)", GridVariant::Fully, -1, 0, 0.10, 0.05},
        {"D4 loose (no distance dimensions, last unit bottom.p2)", GridVariant::Loose, -1, 0, 0.10, 0.05},
        {"D5 decoupled (20 groups, last unit right.p2)", GridVariant::Decoupled, -1, 1, 0.05, 0.03},
        {"D6 redundant (D1 plus a redundant horizontal on unit 3)", GridVariant::Redundant, -1, 0, 0.10, 0.05},
    };
    return cases;
}
inline SketchEntId dragPoint(const Grid& g, const DragCase& d)
{
    const GridUnit& u = g.units[d.unit < 0 ? g.units.size() - 1 : static_cast<std::size_t>(d.unit)];
    return d.which == 0 ? u.bottomP2 : u.rightP2;
}

} // namespace ray::sketch::test
