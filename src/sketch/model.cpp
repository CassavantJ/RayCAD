// Sketch model helpers (task T-090). See model.h.
#include "model.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <type_traits>

namespace ray::sketch {
namespace {

Error invalid(std::string message)
{
    return Error{ErrorCode::InvalidArgument, std::move(message), {"sketch"}};
}

bool finite(double v)
{
    return std::isfinite(v);
}

double distance(Vec2 a, Vec2 b)
{
    return dmath::hypot(b.x - a.x, b.y - a.y);
}

// Distance from p to the infinite line through a and b (as PlaneGCS's P2LDistance computes it).
double pointLineDistance(Vec2 p, Vec2 a, Vec2 b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double d = dmath::sqrt(dx * dx + dy * dy);
    const double area = std::fabs(-p.x * dy + p.y * dx + a.x * b.y - b.x * a.y);
    return area / d;
}

// The angle of the direction a->b, in (-pi, pi].
double direction(Vec2 a, Vec2 b)
{
    return dmath::atan2(b.y - a.y, b.x - a.x);
}

double wrapPi(double a)
{
    constexpr double pi = std::numbers::pi;
    while (a > pi) {
        a -= 2 * pi;
    }
    while (a <= -pi) {
        a += 2 * pi;
    }
    return a;
}

struct CircleLike {
    Vec2 center;
    double radius;
};

std::optional<CircleLike> circleOf(const EntityIndex& index, const Entity& e)
{
    if (const auto* c = std::get_if<Circle>(&e.geom)) {
        const auto center = pointOf(index, c->center);
        if (!center) {
            return std::nullopt;
        }
        return CircleLike{*center, c->radius};
    }
    if (const auto* a = std::get_if<Arc>(&e.geom)) {
        const auto center = pointOf(index, a->center);
        const auto start = pointOf(index, a->start);
        if (!center || !start) {
            return std::nullopt;
        }
        return CircleLike{*center, distance(*center, *start)};
    }
    return std::nullopt;
}

struct Segment {
    Vec2 a, b;
};

std::optional<Segment> lineOf(const EntityIndex& index, const Entity& e)
{
    const auto* l = std::get_if<Line>(&e.geom);
    if (!l) {
        return std::nullopt;
    }
    const auto a = pointOf(index, l->a);
    const auto b = pointOf(index, l->b);
    if (!a || !b) {
        return std::nullopt;
    }
    return Segment{*a, *b};
}

} // namespace

EntityKind kindOf(const Entity& e)
{
    return static_cast<EntityKind>(e.geom.index());
}

const char* kindName(EntityKind k)
{
    switch (k) {
    case EntityKind::Point:
        return "point";
    case EntityKind::Line:
        return "line";
    case EntityKind::Arc:
        return "arc";
    case EntityKind::Circle:
        return "circle";
    case EntityKind::Spline:
        return "spline";
    case EntityKind::Slot:
        return "slot";
    }
    return "?";
}

const char* constraintTypeName(ConstraintType t)
{
    switch (t) {
    case ConstraintType::Coincident:
        return "coincident";
    case ConstraintType::Horizontal:
        return "horizontal";
    case ConstraintType::Vertical:
        return "vertical";
    case ConstraintType::Parallel:
        return "parallel";
    case ConstraintType::Perpendicular:
        return "perpendicular";
    case ConstraintType::Tangent:
        return "tangent";
    case ConstraintType::Equal:
        return "equal";
    case ConstraintType::Symmetric:
        return "symmetric";
    case ConstraintType::Concentric:
        return "concentric";
    case ConstraintType::Midpoint:
        return "midpoint";
    case ConstraintType::PointOnCurve:
        return "point on curve";
    case ConstraintType::Fix:
        return "fix";
    case ConstraintType::Distance:
        return "distance";
    case ConstraintType::DistanceX:
        return "horizontal distance";
    case ConstraintType::DistanceY:
        return "vertical distance";
    case ConstraintType::Radius:
        return "radius";
    case ConstraintType::Diameter:
        return "diameter";
    case ConstraintType::Angle:
        return "angle";
    }
    return "?";
}

std::string idText(const Uuid& id)
{
    return id.toString();
}

Sketch canonicalSketch(Sketch s)
{
    std::stable_sort(s.entities.begin(), s.entities.end(),
                     [](const Entity& a, const Entity& b) { return a.id < b.id; });
    std::stable_sort(s.constraints.begin(), s.constraints.end(),
                     [](const Constraint& a, const Constraint& b) { return a.id < b.id; });
    return s;
}

EntityIndex::EntityIndex(const Sketch& canonical) : sketch_(&canonical) {}

int EntityIndex::indexOf(SketchEntId id) const
{
    const auto& es = sketch_->entities;
    const auto it =
        std::lower_bound(es.begin(), es.end(), id, [](const Entity& e, const SketchEntId& v) { return e.id < v; });
    if (it == es.end() || it->id != id) {
        return -1;
    }
    return static_cast<int>(it - es.begin());
}

const Entity* EntityIndex::find(SketchEntId id) const
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : &sketch_->entities[static_cast<std::size_t>(i)];
}

std::optional<Vec2> pointOf(const EntityIndex& index, SketchEntId id)
{
    const Entity* e = index.find(id);
    if (!e) {
        return std::nullopt;
    }
    const auto* p = std::get_if<Point>(&e->geom);
    if (!p) {
        return std::nullopt;
    }
    return p->p;
}

Status validateSketch(const Sketch& input)
{
    const Sketch s = canonicalSketch(input);
    const EntityIndex index(s);
    for (std::size_t i = 1; i < s.entities.size(); ++i) {
        if (s.entities[i].id == s.entities[i - 1].id) {
            return tl::unexpected(invalid("Sketch entity " + idText(s.entities[i].id.value) + " appears twice"));
        }
    }
    for (std::size_t i = 1; i < s.constraints.size(); ++i) {
        if (s.constraints[i].id == s.constraints[i - 1].id) {
            return tl::unexpected(invalid("Sketch constraint " + idText(s.constraints[i].id) + " appears twice"));
        }
    }
    auto needPoint = [&](const Entity& owner, SketchEntId ref) -> Status {
        const Entity* e = index.find(ref);
        if (!e) {
            return tl::unexpected(invalid("Sketch " + std::string(kindName(kindOf(owner))) + " " +
                                          idText(owner.id.value) + " refers to a missing entity " + idText(ref.value)));
        }
        if (kindOf(*e) != EntityKind::Point) {
            return tl::unexpected(invalid("Sketch " + std::string(kindName(kindOf(owner))) + " " +
                                          idText(owner.id.value) + " refers to " + kindName(kindOf(*e)) + " " +
                                          idText(ref.value) + " where a point is needed"));
        }
        return {};
    };
    for (const Entity& e : s.entities) {
        const std::string what = std::string(kindName(kindOf(e))) + " " + idText(e.id.value);
        Status st = std::visit(
            [&](const auto& g) -> Status {
                using T = std::decay_t<decltype(g)>;
                if constexpr (std::is_same_v<T, Point>) {
                    if (!finite(g.p.x) || !finite(g.p.y)) {
                        return tl::unexpected(invalid("Sketch " + what + " has a coordinate that is not finite"));
                    }
                } else if constexpr (std::is_same_v<T, Line>) {
                    if (g.a == g.b) {
                        return tl::unexpected(invalid("Sketch " + what + " starts and ends at the same point"));
                    }
                    for (SketchEntId r : {g.a, g.b}) {
                        if (auto ok = needPoint(e, r); !ok) {
                            return ok;
                        }
                    }
                } else if constexpr (std::is_same_v<T, Arc>) {
                    if (g.center == g.start || g.center == g.end || g.start == g.end) {
                        return tl::unexpected(invalid("Sketch " + what + " needs three different points"));
                    }
                    for (SketchEntId r : {g.center, g.start, g.end}) {
                        if (auto ok = needPoint(e, r); !ok) {
                            return ok;
                        }
                    }
                } else if constexpr (std::is_same_v<T, Circle>) {
                    if (!finite(g.radius) || g.radius <= 0) {
                        return tl::unexpected(invalid("Sketch " + what + " needs a positive radius"));
                    }
                    if (auto ok = needPoint(e, g.center); !ok) {
                        return ok;
                    }
                } else if constexpr (std::is_same_v<T, Spline>) {
                    if (g.degree < 1 || g.controlPoints.size() < static_cast<std::size_t>(g.degree) + 1) {
                        return tl::unexpected(invalid(
                            "Sketch " + what + " needs degree 1 or more and at least degree + 1 control points"));
                    }
                    for (SketchEntId r : g.controlPoints) {
                        if (auto ok = needPoint(e, r); !ok) {
                            return ok;
                        }
                    }
                } else if constexpr (std::is_same_v<T, Slot>) {
                    if (!finite(g.width) || g.width <= 0) {
                        return tl::unexpected(invalid("Sketch " + what + " needs a positive width"));
                    }
                    if (g.a == g.b) {
                        return tl::unexpected(invalid("Sketch " + what + " needs two different centre points"));
                    }
                    for (SketchEntId r : {g.a, g.b}) {
                        if (auto ok = needPoint(e, r); !ok) {
                            return ok;
                        }
                    }
                }
                return {};
            },
            e.geom);
        if (!st) {
            return st;
        }
    }
    for (const Constraint& c : s.constraints) {
        const std::string what = std::string(constraintTypeName(c.type)) + " constraint " + idText(c.id);
        if (c.refs.empty() || c.refs.size() > 3) {
            return tl::unexpected(invalid("Sketch " + what + " must refer to 1 to 3 entities"));
        }
        for (SketchEntId r : c.refs) {
            if (!index.find(r)) {
                return tl::unexpected(invalid("Sketch " + what + " refers to a missing entity " + idText(r.value)));
            }
        }
        if (!finite(c.value)) {
            return tl::unexpected(invalid("Sketch " + what + " has a value that is not finite"));
        }
    }
    return {};
}

std::optional<double> measureConstraint(const EntityIndex& index, const Constraint& c)
{
    std::vector<const Entity*> refs;
    for (SketchEntId r : c.refs) {
        const Entity* e = index.find(r);
        if (!e) {
            return std::nullopt;
        }
        refs.push_back(e);
    }
    auto kind = [&](std::size_t i) {
        return kindOf(*refs[i]);
    };
    auto point = [&](std::size_t i) {
        return pointOf(index, refs[i]->id);
    };
    auto isCircle = [&](std::size_t i) {
        return kind(i) == EntityKind::Circle || kind(i) == EntityKind::Arc;
    };
    switch (c.type) {
    case ConstraintType::Distance:
        if (refs.size() == 1 && kind(0) == EntityKind::Line) {
            const auto l = lineOf(index, *refs[0]);
            return distance(l->a, l->b);
        }
        if (refs.size() != 2) {
            return std::nullopt;
        }
        if (kind(0) == EntityKind::Point && kind(1) == EntityKind::Point) {
            return distance(*point(0), *point(1));
        }
        for (std::size_t i = 0; i < 2; ++i) {
            const std::size_t j = 1 - i;
            if (kind(i) == EntityKind::Point && kind(j) == EntityKind::Line) {
                const auto l = lineOf(index, *refs[j]);
                return pointLineDistance(*point(i), l->a, l->b);
            }
            if (kind(i) == EntityKind::Point && isCircle(j)) {
                const auto cl = circleOf(index, *refs[j]);
                return std::fabs(distance(*point(i), cl->center) - cl->radius);
            }
            if (isCircle(i) && kind(j) == EntityKind::Line) {
                const auto cl = circleOf(index, *refs[i]);
                const auto l = lineOf(index, *refs[j]);
                return std::fabs(pointLineDistance(cl->center, l->a, l->b) - cl->radius);
            }
        }
        if (isCircle(0) && isCircle(1)) {
            const auto c1 = circleOf(index, *refs[0]);
            const auto c2 = circleOf(index, *refs[1]);
            const double d = distance(c1->center, c2->center);
            if (d >= c1->radius && d >= c2->radius) {
                return d - c1->radius - c2->radius;
            }
            return std::max(c1->radius, c2->radius) - std::min(c1->radius, c2->radius) - d;
        }
        return std::nullopt;
    case ConstraintType::DistanceX:
    case ConstraintType::DistanceY: {
        const bool x = c.type == ConstraintType::DistanceX;
        if (refs.size() == 1 && kind(0) == EntityKind::Point) {
            return x ? point(0)->x : point(0)->y;
        }
        if (refs.size() == 1 && kind(0) == EntityKind::Line) {
            const auto l = lineOf(index, *refs[0]);
            return x ? l->b.x - l->a.x : l->b.y - l->a.y;
        }
        if (refs.size() == 2 && kind(0) == EntityKind::Point && kind(1) == EntityKind::Point) {
            return x ? point(1)->x - point(0)->x : point(1)->y - point(0)->y;
        }
        return std::nullopt;
    }
    case ConstraintType::Radius:
    case ConstraintType::Diameter:
        if (refs.size() == 1 && isCircle(0)) {
            const double r = circleOf(index, *refs[0])->radius;
            return c.type == ConstraintType::Radius ? r : 2 * r;
        }
        return std::nullopt;
    case ConstraintType::Angle:
        if (refs.size() == 1 && kind(0) == EntityKind::Line) {
            const auto l = lineOf(index, *refs[0]);
            return direction(l->a, l->b);
        }
        if (refs.size() == 2 && kind(0) == EntityKind::Line && kind(1) == EntityKind::Line) {
            const auto l1 = lineOf(index, *refs[0]);
            const auto l2 = lineOf(index, *refs[1]);
            return wrapPi(direction(l2->a, l2->b) - direction(l1->a, l1->b));
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

} // namespace ray::sketch
