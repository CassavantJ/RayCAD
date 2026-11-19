#include "dimensions.h"

#include <algorithm>
#include <cmath>

namespace ray::ui::sketchui {

using sketch::ConstraintType;

namespace {

enum class Kind { None, Point, Line, Circular, Other };

Kind kindOf(const SketchView& view, const SketchEntId& id)
{
    if (view.pointEntry(id) != nullptr)
        return Kind::Point;
    const ViewCurve* c = view.curve(id);
    if (c == nullptr)
        return Kind::None;
    switch (c->kind) {
    case ViewCurve::Kind::Line:
        return Kind::Line;
    case ViewCurve::Kind::Arc:
    case ViewCurve::Kind::Circle:
        return Kind::Circular;
    default:
        return Kind::Other;
    }
}

Vec2 mid(Vec2 a, Vec2 b)
{
    return mul(add(a, b), 0.5);
}

// The foot of the perpendicular from p on the carrier of a -> b.
Vec2 foot(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2 d = sub(b, a);
    const double l2 = dot(d, d);
    if (!(l2 > 0))
        return a;
    return add(a, mul(d, dot(sub(p, a), d) / l2));
}

double lineDistance(Vec2 p, Vec2 a, Vec2 b)
{
    return distance(p, foot(p, a, b));
}

// Wraps to (-pi, pi].
double wrapPi(double a)
{
    while (a > kPi)
        a -= 2 * kPi;
    while (a <= -kPi)
        a += 2 * kPi;
    return a;
}

double signedAngle(Vec2 from, Vec2 to)
{
    return std::atan2(cross(from, to), dot(from, to));
}

std::optional<Vec2> carrierCrossing(Vec2 a1, Vec2 d1, Vec2 a2, Vec2 d2)
{
    const double den = cross(d1, d2);
    if (std::abs(den) < 1e-12)
        return std::nullopt;
    const double t = cross(sub(a2, a1), d2) / den;
    return add(a1, mul(d1, t));
}

struct Refs {
    std::vector<Kind> kinds;
    std::vector<SketchEntId> ids;
    bool is(std::initializer_list<Kind> want) const
    {
        return kinds.size() == want.size() && std::equal(kinds.begin(), kinds.end(), want.begin());
    }
};

Refs refsOf(const SketchView& view, const sketch::Constraint& c)
{
    Refs r;
    for (const SketchEntId& id : c.refs) {
        r.kinds.push_back(kindOf(view, id));
        r.ids.push_back(id);
    }
    return r;
}

// The two points a distance is measured between (on the curves for gaps).
struct Span {
    Vec2 a, b;
    std::optional<std::pair<Vec2, Vec2>> carrier; // a line the measure runs to (its segment), for a carrier extension
};

std::optional<Span> distanceSpan(const SketchView& view, const Refs& r)
{
    const auto pt = [&](std::size_t i) {
        return *view.point(r.ids[i]);
    };
    const auto cv = [&](std::size_t i) {
        return view.curve(r.ids[i]);
    };
    if (r.is({Kind::Line})) {
        const ViewCurve* l = cv(0);
        return Span{l->a, l->b, std::nullopt};
    }
    if (r.is({Kind::Point, Kind::Point}))
        return Span{pt(0), pt(1), std::nullopt};
    if (r.kinds.size() != 2)
        return std::nullopt;
    for (std::size_t i = 0; i < 2; ++i) {
        const std::size_t j = 1 - i;
        if (r.kinds[i] == Kind::Point && r.kinds[j] == Kind::Line) {
            const ViewCurve* l = cv(j);
            const Vec2 p = pt(i);
            return Span{p, foot(p, l->a, l->b), std::make_pair(l->a, l->b)};
        }
        if (r.kinds[i] == Kind::Point && r.kinds[j] == Kind::Circular) {
            const ViewCurve* c = cv(j);
            const Vec2 p = pt(i);
            const Vec2 dir = unit(sub(p, c->c));
            return Span{p, add(c->c, mul(length(dir) > 0 ? dir : Vec2{1, 0}, c->r)), std::nullopt};
        }
        if (r.kinds[i] == Kind::Circular && r.kinds[j] == Kind::Line) {
            const ViewCurve* c = cv(i);
            const ViewCurve* l = cv(j);
            const Vec2 f = foot(c->c, l->a, l->b);
            Vec2 dir = unit(sub(f, c->c));
            if (!(length(dir) > 0))
                dir = perpLeft(unit(sub(l->b, l->a)));
            return Span{add(c->c, mul(dir, c->r)), f, std::make_pair(l->a, l->b)};
        }
    }
    if (r.is({Kind::Circular, Kind::Circular})) {
        const ViewCurve* c1 = cv(0);
        const ViewCurve* c2 = cv(1);
        Vec2 dir = unit(sub(c2->c, c1->c));
        if (!(length(dir) > 0))
            dir = {1, 0};
        return Span{add(c1->c, mul(dir, c1->r)), sub(c2->c, mul(dir, c2->r)), std::nullopt};
    }
    return std::nullopt;
}

std::optional<std::pair<Vec2, Vec2>> axisSpan(const SketchView& view, const Refs& r)
{
    if (r.is({Kind::Point}))
        return std::make_pair(Vec2{0, 0}, *view.point(r.ids[0]));
    if (r.is({Kind::Point, Kind::Point}))
        return std::make_pair(*view.point(r.ids[0]), *view.point(r.ids[1]));
    if (r.is({Kind::Line})) {
        const ViewCurve* l = view.curve(r.ids[0]);
        return std::make_pair(l->a, l->b);
    }
    return std::nullopt;
}

// Linear dimension between a and b measured along `u` (unit), label at `label`.
DimensionShape linear(Vec2 a, Vec2 b, Vec2 u, Vec2 label, double gap)
{
    DimensionShape s;
    const Vec2 n = perpLeft(u);
    const Vec2 a2 = add(a, mul(n, dot(sub(label, a), n)));
    const Vec2 b2 = add(b, mul(n, dot(sub(label, b), n)));
    const double over = gap * 0.35;
    for (const auto& [from, to] : {std::make_pair(a, a2), std::make_pair(b, b2)}) {
        const Vec2 d = sub(to, from);
        const double len = length(d);
        if (len > gap * 0.05) {
            const Vec2 du = mul(d, 1.0 / len);
            s.lines.push_back({add(from, mul(du, std::min(over, len * 0.5))), add(to, mul(du, over))});
        }
    }
    // The dimension line, out to the label when the label is beyond its ends.
    const Vec2 ab = sub(b2, a2);
    const double span = length(ab);
    const Vec2 along = span > 0 ? mul(ab, 1.0 / span) : u;
    const double t = dot(sub(label, a2), along);
    const Vec2 from = t < 0 ? add(a2, mul(along, t)) : a2;
    const Vec2 to = t > span ? add(a2, mul(along, t)) : b2;
    s.lines.push_back({from, to});
    if (span > 1e-9) {
        s.arrows.push_back({a2, mul(along, -1)});
        s.arrows.push_back({b2, along});
    }
    s.anchor = mid(a, b);
    s.label = label;
    return s;
}

Vec2 defaultSide(Vec2 u)
{
    // Above or to the left of the geometry.
    const Vec2 n = perpLeft(u);
    return n.y < -1e-9 || (std::abs(n.y) <= 1e-9 && n.x > 0) ? mul(n, -1) : n;
}

const Vec2 kDiagonal{0.70710678118654752, 0.70710678118654752};

} // namespace

bool isDimension(ConstraintType type)
{
    switch (type) {
    case ConstraintType::Distance:
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY:
    case ConstraintType::Radius:
    case ConstraintType::Diameter:
    case ConstraintType::Angle:
        return true;
    default:
        return false;
    }
}

std::optional<double> measureSigned(const SketchView& view, const sketch::Constraint& c)
{
    const Refs r = refsOf(view, c);
    switch (c.type) {
    case ConstraintType::Distance: {
        if (r.kinds.size() == 2) {
            for (std::size_t i = 0; i < 2; ++i) {
                const std::size_t j = 1 - i;
                if (r.kinds[i] == Kind::Circular && r.kinds[j] == Kind::Line) {
                    const ViewCurve* cc = view.curve(r.ids[i]);
                    const ViewCurve* l = view.curve(r.ids[j]);
                    return std::abs(lineDistance(cc->c, l->a, l->b) - cc->r);
                }
                if (r.kinds[i] == Kind::Point && r.kinds[j] == Kind::Circular) {
                    const ViewCurve* cc = view.curve(r.ids[j]);
                    return std::abs(distance(*view.point(r.ids[i]), cc->c) - cc->r);
                }
            }
            if (r.is({Kind::Circular, Kind::Circular})) {
                const ViewCurve* c1 = view.curve(r.ids[0]);
                const ViewCurve* c2 = view.curve(r.ids[1]);
                return std::abs(distance(c1->c, c2->c) - c1->r - c2->r);
            }
        }
        const auto s = distanceSpan(view, r);
        if (!s)
            return std::nullopt;
        return distance(s->a, s->b);
    }
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY: {
        const auto s = axisSpan(view, r);
        if (!s)
            return std::nullopt;
        return c.type == ConstraintType::DistanceX ? s->second.x - s->first.x : s->second.y - s->first.y;
    }
    case ConstraintType::Radius:
    case ConstraintType::Diameter:
        if (!r.is({Kind::Circular}))
            return std::nullopt;
        return view.curve(r.ids[0])->r * (c.type == ConstraintType::Diameter ? 2 : 1);
    case ConstraintType::Angle:
        if (r.is({Kind::Line})) {
            const ViewCurve* l = view.curve(r.ids[0]);
            return wrapPi(angleOf(sub(l->b, l->a)));
        }
        if (r.is({Kind::Line, Kind::Line})) {
            const ViewCurve* l1 = view.curve(r.ids[0]);
            const ViewCurve* l2 = view.curve(r.ids[1]);
            return signedAngle(unit(sub(l1->b, l1->a)), unit(sub(l2->b, l2->a)));
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

std::optional<DimensionShape> layoutDimension(const SketchView& view, const sketch::Constraint& c,
                                              std::optional<Vec2> labelOffset, double gap)
{
    const Refs r = refsOf(view, c);
    for (const Kind k : r.kinds) {
        if (k == Kind::None)
            return std::nullopt;
    }
    switch (c.type) {
    case ConstraintType::Distance: {
        const auto s = distanceSpan(view, r);
        if (!s)
            return std::nullopt;
        Vec2 u = unit(sub(s->b, s->a));
        if (!(length(u) > 0))
            u = s->carrier ? perpLeft(unit(sub(s->carrier->second, s->carrier->first))) : Vec2{1, 0};
        if (!(length(u) > 0))
            u = {1, 0};
        const Vec2 anchor = mid(s->a, s->b);
        const Vec2 label = add(anchor, labelOffset.value_or(mul(defaultSide(u), gap * 2)));
        DimensionShape shape = linear(s->a, s->b, u, label, gap);
        if (s->carrier) {
            // The line's carrier out to the foot when the foot is beyond the segment.
            const auto [la, lb] = *s->carrier;
            const Vec2 d = sub(lb, la);
            const double l2 = dot(d, d);
            const double t = l2 > 0 ? dot(sub(s->b, la), d) / l2 : 0;
            if (t < 0 || t > 1)
                shape.lines.push_back({t < 0 ? la : lb, s->b});
        }
        return shape;
    }
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY: {
        const auto s = axisSpan(view, r);
        if (!s)
            return std::nullopt;
        const bool x = c.type == ConstraintType::DistanceX;
        const Vec2 u = x ? Vec2{1, 0} : Vec2{0, 1};
        const Vec2 anchor = mid(s->first, s->second);
        // Horizontal: above the higher point; vertical: left of the leftmost.
        Vec2 fallback = x ? Vec2{0, std::max(s->first.y, s->second.y) - anchor.y + gap * 2}
                          : Vec2{std::min(s->first.x, s->second.x) - anchor.x - gap * 2, 0};
        const Vec2 label = add(anchor, labelOffset.value_or(fallback));
        return linear(s->first, s->second, u, label, gap);
    }
    case ConstraintType::Radius:
    case ConstraintType::Diameter: {
        if (!r.is({Kind::Circular}))
            return std::nullopt;
        const ViewCurve* cc = view.curve(r.ids[0]);
        Vec2 offset = labelOffset.value_or(Vec2{});
        if (!labelOffset) {
            Vec2 dir = kDiagonal;
            if (cc->kind == ViewCurve::Kind::Arc) // the middle of the arc
                dir = polar({0, 0}, 1, cc->start + (cc->ccw ? 1 : -1) * cc->sweep * 0.5);
            offset = mul(dir, cc->r + gap * 2);
        }
        Vec2 dir = unit(offset);
        if (!(length(dir) > 0))
            dir = kDiagonal;
        DimensionShape s;
        s.anchor = cc->c;
        s.label = add(cc->c, offset);
        const double reach = length(offset);
        const Vec2 tip = add(cc->c, mul(dir, cc->r));
        const Vec2 end = reach > cc->r ? s.label : tip;
        if (c.type == ConstraintType::Radius) {
            s.lines.push_back({cc->c, end});
            s.arrows.push_back({tip, dir});
        } else {
            const Vec2 tip2 = sub(cc->c, mul(dir, cc->r));
            s.lines.push_back({tip2, end});
            s.arrows.push_back({tip, dir});
            s.arrows.push_back({tip2, mul(dir, -1)});
        }
        return s;
    }
    case ConstraintType::Angle: {
        Vec2 o, d1, d2;
        std::vector<std::pair<Vec2, Vec2>> segments; // the lines' segments, for extensions
        if (r.is({Kind::Line})) {
            const ViewCurve* l = view.curve(r.ids[0]);
            o = l->a;
            d1 = {1, 0};
            d2 = unit(sub(l->b, l->a));
            segments.push_back({l->a, l->a});
            segments.push_back({l->a, l->b});
        } else if (r.is({Kind::Line, Kind::Line})) {
            const ViewCurve* l1 = view.curve(r.ids[0]);
            const ViewCurve* l2 = view.curve(r.ids[1]);
            d1 = unit(sub(l1->b, l1->a));
            d2 = unit(sub(l2->b, l2->a));
            const auto x = carrierCrossing(l1->a, d1, l2->a, d2);
            o = x ? *x : mid(mid(l1->a, l1->b), mid(l2->a, l2->b));
            segments.push_back({l1->a, l1->b});
            segments.push_back({l2->a, l2->b});
        } else {
            return std::nullopt;
        }
        if (!(length(d1) > 0) || !(length(d2) > 0))
            return std::nullopt;
        const double a = signedAngle(d1, d2);
        Vec2 bisector = unit(add(d1, d2));
        if (!(length(bisector) > 0))
            bisector = mul(perpLeft(d1), a >= 0 ? 1 : -1);
        const Vec2 offset = labelOffset.value_or(mul(bisector, gap * 5));
        const Vec2 label = add(o, offset);
        // The sector holding the label: (d1, d2) or the opposite one (the same angle).
        const bool flip = dot(offset, bisector) < 0;
        const Vec2 s1 = flip ? mul(d1, -1) : d1;
        const Vec2 s2 = flip ? mul(d2, -1) : d2;
        const double radius = std::max(length(offset), gap);
        DimensionShape s;
        s.anchor = o;
        s.label = label;
        s.lines.push_back(arcPolyline(o, radius, angleOf(s1), a));
        const double sigma = a >= 0 ? 1 : -1;
        const Vec2 p1 = add(o, mul(s1, radius));
        const Vec2 p2 = add(o, mul(s2, radius));
        if (std::abs(a) > 1e-9) {
            s.arrows.push_back({p1, mul(perpLeft(s1), -sigma)});
            s.arrows.push_back({p2, mul(perpLeft(s2), sigma)});
        }
        // Extension lines from each segment out to the arc end when the arc end is beyond it.
        for (std::size_t i = 0; i < 2; ++i) {
            const auto [sa, sb] = segments[i];
            const Vec2 p = i == 0 ? p1 : p2;
            const Vec2 d = sub(sb, sa);
            const double l2 = dot(d, d);
            if (!(l2 > 0)) {
                s.lines.push_back({sa, p}); // the x axis reference of a one-line angle
                continue;
            }
            const double t = dot(sub(p, sa), d) / l2;
            if (t < 0 || t > 1)
                s.lines.push_back({t < 0 ? sa : sb, p});
        }
        return s;
    }
    default:
        return std::nullopt;
    }
}

std::optional<DimensionPick> pickDimension(const SketchView& view, const std::vector<SketchEntId>& picks, Vec2 cursor)
{
    if (picks.empty() || picks.size() > 2)
        return std::nullopt;
    std::vector<Kind> kinds;
    for (const SketchEntId& id : picks)
        kinds.push_back(kindOf(view, id));
    const auto make = [&](ConstraintType type, std::vector<SketchEntId> refs) -> std::optional<DimensionPick> {
        sketch::Constraint c;
        c.type = type;
        c.refs = refs;
        const auto v = measureSigned(view, c);
        if (!v)
            return std::nullopt;
        return DimensionPick{type, std::move(refs), *v};
    };
    if (picks.size() == 1) {
        const ViewCurve* c = view.curve(picks[0]);
        if (kinds[0] == Kind::Line)
            return make(ConstraintType::Distance, {picks[0]});
        if (kinds[0] == Kind::Circular)
            return make(c->kind == ViewCurve::Kind::Circle ? ConstraintType::Diameter : ConstraintType::Radius,
                        {picks[0]});
        return std::nullopt;
    }
    const SketchEntId a = picks[0], b = picks[1];
    if (a == b)
        return std::nullopt;
    if (kinds[0] == Kind::Point && kinds[1] == Kind::Point) {
        const Vec2 p = *view.point(a), q = *view.point(b);
        const double minX = std::min(p.x, q.x), maxX = std::max(p.x, q.x);
        const double minY = std::min(p.y, q.y), maxY = std::max(p.y, q.y);
        const bool besideX = cursor.x < minX || cursor.x > maxX; // left or right of the box
        const bool besideY = cursor.y < minY || cursor.y > maxY; // above or below it
        // Ordered so the stored difference is positive.
        if (besideY && !besideX && std::abs(q.x - p.x) > 1e-9)
            return make(ConstraintType::DistanceX, p.x <= q.x ? std::vector{a, b} : std::vector{b, a});
        if (besideX && !besideY && std::abs(q.y - p.y) > 1e-9)
            return make(ConstraintType::DistanceY, p.y <= q.y ? std::vector{a, b} : std::vector{b, a});
        if (distance(p, q) < 1e-9)
            return std::nullopt;
        return make(ConstraintType::Distance, {a, b});
    }
    for (int i = 0; i < 2; ++i) {
        const SketchEntId p = i == 0 ? a : b, l = i == 0 ? b : a;
        const Kind kp = kinds[static_cast<std::size_t>(i)], kl = kinds[static_cast<std::size_t>(1 - i)];
        if (kp == Kind::Point && kl == Kind::Line) {
            const ViewCurve* line = view.curve(l);
            if (std::find(line->points.begin(), line->points.end(), p) != line->points.end())
                return make(ConstraintType::Distance, {l}); // its own end: the length
            if (lineDistance(*view.point(p), line->a, line->b) < 1e-9)
                return std::nullopt;
            return make(ConstraintType::Distance, {p, l});
        }
        if (kp == Kind::Point && kl == Kind::Circular) {
            const ViewCurve* circle = view.curve(l);
            if (!circle->points.empty() && circle->points[0] == p)
                return make(circle->kind == ViewCurve::Kind::Circle ? ConstraintType::Diameter : ConstraintType::Radius,
                            {l}); // its centre
            return make(ConstraintType::Distance, {p, l});
        }
        if (kp == Kind::Circular && kl == Kind::Line)
            return make(ConstraintType::Distance, {p, l});
    }
    if (kinds[0] == Kind::Line && kinds[1] == Kind::Line) {
        const ViewCurve* l1 = view.curve(a);
        const ViewCurve* l2 = view.curve(b);
        const Vec2 d1 = unit(sub(l1->b, l1->a)), d2 = unit(sub(l2->b, l2->a));
        if (std::abs(cross(d1, d2)) < 1e-9) {
            // Parallel: the distance from the second line's start to the first line.
            if (lineDistance(l2->a, l1->a, l1->b) < 1e-9 || l2->points.empty())
                return std::nullopt;
            return make(ConstraintType::Distance, {l2->points[0], a});
        }
        return make(ConstraintType::Angle, {a, b});
    }
    if (kinds[0] == Kind::Circular && kinds[1] == Kind::Circular) {
        const ViewCurve* c1 = view.curve(a);
        const ViewCurve* c2 = view.curve(b);
        if (c1->points.empty() || c2->points.empty() || c1->points[0] == c2->points[0])
            return std::nullopt;
        return make(ConstraintType::Distance, {c1->points[0], c2->points[0]}); // centre to centre
    }
    return std::nullopt;
}

double signedValue(ConstraintType type, double current, double typed)
{
    const bool signedType =
        type == ConstraintType::DistanceX || type == ConstraintType::DistanceY || type == ConstraintType::Angle;
    return signedType && current < 0 ? -std::abs(typed) : std::abs(typed);
}

double shownValue(ConstraintType, double value)
{
    return std::abs(value);
}

} // namespace ray::ui::sketchui
