// Translation of a sketch into PlaneGCS objects (task T-090). See solver_translate.h.
#include "solver_translate.h"

#include "solver_system.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <string>

namespace ray::sketch::detail {
namespace {

Error invalid(std::string message)
{
    return Error{ErrorCode::InvalidArgument, std::move(message), {"sketch solver"}};
}

bool isCircular(EntityKind k)
{
    return k == EntityKind::Circle || k == EntityKind::Arc;
}

// Emits the objects of one sketch constraint.
class Emitter {
public:
    Emitter(GcsModel& m, const ConstraintSink& sink) : m_(m), sink_(sink) {}

    void arcRules(GCS::Arc& a)
    {
        // GCS::System::addConstraintArcRules -> two addConstraintCurveValue calls, x then y.
        tag_ = 0;
        add(new GCS::ConstraintCurveValue(a.start, a.start.x, a, a.startAngle));
        add(new GCS::ConstraintCurveValue(a.start, a.start.y, a, a.startAngle));
        add(new GCS::ConstraintCurveValue(a.end, a.end.x, a, a.endAngle));
        add(new GCS::ConstraintCurveValue(a.end, a.end.y, a, a.endAngle));
    }

    Status constraint(int tag, const Constraint& c)
    {
        tag_ = tag;
        c_ = &c;
        refs_.clear();
        for (SketchEntId r : c.refs) {
            const int i = EntityIndex(m_.sketch).indexOf(r);
            refs_.push_back(i);
        }
        switch (c.type) {
        case ConstraintType::Coincident:
            if (is({EntityKind::Point, EntityKind::Point})) {
                coincident(pt(0), pt(1));
                return {};
            }
            break;
        case ConstraintType::Horizontal:
        case ConstraintType::Vertical: {
            const bool h = c.type == ConstraintType::Horizontal;
            if (is({EntityKind::Line})) {
                GCS::Line& l = line(0);
                equal(h ? l.p1.y : l.p1.x, h ? l.p2.y : l.p2.x);
                return {};
            }
            if (is({EntityKind::Point, EntityKind::Point})) {
                equal(h ? pt(0).y : pt(0).x, h ? pt(1).y : pt(1).x);
                return {};
            }
            break;
        }
        case ConstraintType::Parallel:
            if (is({EntityKind::Line, EntityKind::Line})) {
                add(new GCS::ConstraintParallel(line(0), line(1)));
                return {};
            }
            break;
        case ConstraintType::Perpendicular:
            if (is({EntityKind::Line, EntityKind::Line})) {
                add(new GCS::ConstraintPerpendicular(line(0), line(1)));
                return {};
            }
            break;
        case ConstraintType::Tangent:
            return tangent();
        case ConstraintType::Equal:
            if (is({EntityKind::Line, EntityKind::Line})) {
                add(new GCS::ConstraintEqualLineLength(line(0), line(1)));
                return {};
            }
            if (count(2) && isCircular(kind(0)) && isCircular(kind(1))) {
                equal(circle(0).rad, circle(1).rad);
                return {};
            }
            break;
        case ConstraintType::Symmetric:
            if (is({EntityKind::Point, EntityKind::Point, EntityKind::Line})) {
                // addConstraintP2PSymmetric(Point&, Point&, Line&)
                GCS::Line& l = line(2);
                add(new GCS::ConstraintPerpendicular(pt(0), pt(1), l.p1, l.p2));
                add(new GCS::ConstraintMidpointOnLine(pt(0), pt(1), l.p1, l.p2));
                return {};
            }
            if (is({EntityKind::Point, EntityKind::Point, EntityKind::Point})) {
                symmetricAboutPoint(pt(0), pt(1), pt(2));
                return {};
            }
            break;
        case ConstraintType::Concentric:
            if (count(2) && isCircular(kind(0)) && isCircular(kind(1))) {
                coincident(circle(0).center, circle(1).center);
                return {};
            }
            break;
        case ConstraintType::Midpoint:
            for (int i = 0; i < 2; ++i) {
                if (count(2) && kind(i) == EntityKind::Point && kind(1 - i) == EntityKind::Line) {
                    GCS::Line& l = line(1 - i);
                    symmetricAboutPoint(l.p1, l.p2, pt(i));
                    return {};
                }
            }
            break;
        case ConstraintType::PointOnCurve:
            for (int i = 0; i < 2; ++i) {
                if (!count(2) || kind(i) != EntityKind::Point) {
                    continue;
                }
                if (kind(1 - i) == EntityKind::Line) {
                    add(new GCS::ConstraintPointOnLine(pt(i), line(1 - i)));
                    return {};
                }
                if (isCircular(kind(1 - i))) {
                    // addConstraintPointOnCircle / addConstraintPointOnArc: a P2PDistance to the centre.
                    GCS::Circle& cl = circle(1 - i);
                    add(new GCS::ConstraintP2PDistance(pt(i), cl.center, cl.rad));
                    return {};
                }
            }
            break;
        case ConstraintType::Fix:
            return fix();
        case ConstraintType::Distance:
            return distance();
        case ConstraintType::DistanceX:
        case ConstraintType::DistanceY: {
            const bool x = c.type == ConstraintType::DistanceX;
            auto coord = [x](GCS::Point& p) {
                return x ? p.x : p.y;
            };
            if (is({EntityKind::Point})) {
                equal(coord(pt(0)), value()); // addConstraintCoordinateX / Y
                return {};
            }
            if (is({EntityKind::Point, EntityKind::Point})) {
                add(new GCS::ConstraintDifference(coord(pt(0)), coord(pt(1)), value()));
                return {};
            }
            if (is({EntityKind::Line})) {
                GCS::Line& l = line(0);
                add(new GCS::ConstraintDifference(coord(l.p1), coord(l.p2), value()));
                return {};
            }
            break;
        }
        case ConstraintType::Radius:
        case ConstraintType::Diameter:
            if (count(1) && isCircular(kind(0))) {
                if (!(c.value > 0)) {
                    return tl::unexpected(invalid(what() + " needs a positive value"));
                }
                // addConstraintCircleRadius / addConstraintCircleDiameter (a proportional Equal, ratio 0.5).
                add(new GCS::ConstraintEqual(circle(0).rad, value(), c.type == ConstraintType::Radius ? 1.0 : 0.5));
                return {};
            }
            break;
        case ConstraintType::Angle:
            if (is({EntityKind::Line})) {
                GCS::Line& l = line(0);
                add(new GCS::ConstraintP2PAngle(l.p1, l.p2, value()));
                return {};
            }
            if (is({EntityKind::Line, EntityKind::Line})) {
                add(new GCS::ConstraintL2LAngle(line(0), line(1), value()));
                return {};
            }
            break;
        }
        return unsupported();
    }

private:
    GcsModel& m_;
    const ConstraintSink& sink_;
    int tag_ = 0;
    const Constraint* c_ = nullptr;
    std::vector<int> refs_;

    std::string what() const
    {
        return std::string("Sketch ") + constraintTypeName(c_->type) + " constraint " + idText(c_->id);
    }
    Status unsupported() const
    {
        std::string kinds;
        for (std::size_t i = 0; i < refs_.size(); ++i) {
            kinds += (i ? ", " : "") + std::string(kindName(kind(static_cast<int>(i))));
        }
        return tl::unexpected(invalid(what() + " cannot be applied to (" + kinds + ")"));
    }
    bool count(std::size_t n) const { return refs_.size() == n; }
    EntityKind kind(int i) const { return m_.geo[static_cast<std::size_t>(refs_[static_cast<std::size_t>(i)])].kind; }
    bool is(std::initializer_list<EntityKind> kinds) const
    {
        if (kinds.size() != refs_.size()) {
            return false;
        }
        int i = 0;
        for (EntityKind k : kinds) {
            if (kind(i++) != k) {
                return false;
            }
        }
        return true;
    }
    EntityGeo& g(int i) { return m_.geo[static_cast<std::size_t>(refs_[static_cast<std::size_t>(i)])]; }
    GCS::Point& pt(int i) { return g(i).point; }
    GCS::Line& line(int i) { return g(i).line; }
    GCS::Circle& circle(int i)
    {
        EntityGeo& e = g(i);
        return e.kind == EntityKind::Arc ? static_cast<GCS::Circle&>(e.arc) : e.circle;
    }
    GCS::Curve& curve(int i)
    {
        EntityGeo& e = g(i);
        if (e.kind == EntityKind::Line) {
            return e.line;
        }
        if (e.kind == EntityKind::Arc) {
            return e.arc;
        }
        return e.circle;
    }
    double* constant(double v)
    {
        m_.constants.push_back(v);
        return &m_.constants.back();
    }
    double* value() { return constant(c_->value); }

    void add(GCS::Constraint* o)
    {
        o->setTag(tag_);
        o->setDriving(true);
        sink_(o);
    }
    void equal(double* a, double* b) { add(new GCS::ConstraintEqual(a, b)); }
    void coincident(GCS::Point& a, GCS::Point& b)
    {
        // addConstraintP2PCoincident
        equal(a.x, b.x);
        equal(a.y, b.y);
    }
    // addConstraintP2PSymmetric(Point& p1, Point& p2, Point& p)
    void symmetricAboutPoint(GCS::Point& p1, GCS::Point& p2, GCS::Point& p)
    {
        add(new GCS::ConstraintPointOnPerpBisector(p, p1, p2));
        add(new GCS::ConstraintPointOnLine(p, p1, p2));
    }

    Status tangent()
    {
        if (count(2)) {
            for (int i = 0; i < 2; ++i) {
                if (kind(i) == EntityKind::Line && isCircular(kind(1 - i))) {
                    // addConstraintTangent(Line&, Circle&) / (Line&, Arc&): centre-to-line distance = radius.
                    GCS::Circle& cl = circle(1 - i);
                    add(new GCS::ConstraintP2LDistance(cl.center, line(i), cl.rad));
                    return {};
                }
            }
            if (isCircular(kind(0)) && isCircular(kind(1))) {
                // addConstraintTangent(Circle&, Circle&) and the arc variants: internal or external tangency, chosen
                // from the current geometry.
                GCS::Circle& c1 = circle(0);
                GCS::Circle& c2 = circle(1);
                const double dx = *c2.center.x - *c1.center.x;
                const double dy = *c2.center.y - *c1.center.y;
                const double d = std::sqrt(dx * dx + dy * dy);
                add(new GCS::ConstraintTangentCircumf(c1.center, c2.center, c1.rad, c2.rad,
                                                      d < *c1.rad || d < *c2.rad));
                return {};
            }
        }
        if (count(3) && kind(2) == EntityKind::Point) {
            const auto curveKind = [](EntityKind k) {
                return k == EntityKind::Line || isCircular(k);
            };
            if (curveKind(kind(0)) && curveKind(kind(1))) {
                // Tangency at a point the two curves share (FreeCAD's endpoint-to-endpoint tangency): the angle
                // between the curves' normals at the point is 0 or pi, whichever is nearer now.
                const std::unique_ptr<GCS::System> calc = newSystem();
                double angle = calc->calculateAngleViaPoint(curve(0), curve(1), pt(2));
                constexpr double pi = std::numbers::pi;
                while (angle > pi) {
                    angle -= 2 * pi;
                }
                while (angle < -pi) {
                    angle += 2 * pi;
                }
                add(new GCS::ConstraintAngleViaPoint(curve(0), curve(1), pt(2),
                                                     constant(std::fabs(angle) > pi / 2 ? pi : 0.0)));
                return {};
            }
        }
        return unsupported();
    }

    Status fix()
    {
        // Every unknown that defines the referenced entities is held at its current value (Equal to a constant, as
        // addConstraintCoordinateX/Y do for a point).
        auto fixPoint = [&](GCS::Point& p) {
            equal(p.x, constant(*p.x));
            equal(p.y, constant(*p.y));
        };
        for (std::size_t i = 0; i < refs_.size(); ++i) {
            EntityGeo& e = g(static_cast<int>(i));
            const Entity& ent = m_.sketch.entities[static_cast<std::size_t>(refs_[i])];
            switch (e.kind) {
            case EntityKind::Point:
                fixPoint(e.point);
                break;
            case EntityKind::Line:
                fixPoint(e.line.p1);
                fixPoint(e.line.p2);
                break;
            case EntityKind::Circle:
                fixPoint(e.circle.center);
                equal(e.circle.rad, constant(*e.circle.rad));
                break;
            case EntityKind::Arc:
                // The centre and the arc's own radius and angles: exactly its 5 degrees of freedom. Fixing the end
                // points as well would make one of these rows redundant with the arc rules.
                fixPoint(e.arc.center);
                equal(e.arc.rad, constant(*e.arc.rad));
                equal(e.arc.startAngle, constant(*e.arc.startAngle));
                equal(e.arc.endAngle, constant(*e.arc.endAngle));
                break;
            case EntityKind::Spline:
                for (SketchEntId id : std::get<Spline>(ent.geom).controlPoints) {
                    fixPoint(m_.geo[static_cast<std::size_t>(m_.pointIndex(id))].point);
                }
                break;
            case EntityKind::Slot: {
                const auto& s = std::get<Slot>(ent.geom);
                fixPoint(m_.geo[static_cast<std::size_t>(m_.pointIndex(s.a))].point);
                fixPoint(m_.geo[static_cast<std::size_t>(m_.pointIndex(s.b))].point);
                break;
            }
            }
        }
        return {};
    }

    Status distance()
    {
        const double v = c_->value;
        if (is({EntityKind::Line}) || is({EntityKind::Point, EntityKind::Point})) {
            if (!(v > 0)) {
                return tl::unexpected(invalid(what() + " needs a positive value (use a coincident constraint for 0)"));
            }
            if (count(1)) {
                GCS::Line& l = line(0);
                add(new GCS::ConstraintP2PDistance(l.p1, l.p2, value()));
            } else {
                add(new GCS::ConstraintP2PDistance(pt(0), pt(1), value()));
            }
            return {};
        }
        if (!(v >= 0)) {
            return tl::unexpected(invalid(what() + " needs a value of 0 or more"));
        }
        if (count(2)) {
            for (int i = 0; i < 2; ++i) {
                const int j = 1 - i;
                if (kind(i) == EntityKind::Point && kind(j) == EntityKind::Line) {
                    add(new GCS::ConstraintP2LDistance(pt(i), line(j), value()));
                    return {};
                }
                if (kind(i) == EntityKind::Point && isCircular(kind(j))) {
                    add(new GCS::ConstraintP2CDistance(pt(i), circle(j), value()));
                    return {};
                }
                if (isCircular(kind(i)) && kind(j) == EntityKind::Line) {
                    add(new GCS::ConstraintC2LDistance(circle(i), line(j), value()));
                    return {};
                }
            }
            if (isCircular(kind(0)) && isCircular(kind(1))) {
                add(new GCS::ConstraintC2CDistance(circle(0), circle(1), value()));
                return {};
            }
        }
        return unsupported();
    }
};

} // namespace

int GcsModel::pointIndex(SketchEntId id) const
{
    return EntityIndex(sketch).indexOf(id);
}

Result<std::unique_ptr<GcsModel>> buildModel(const Sketch& input)
{
    if (auto ok = validateSketch(input); !ok) {
        return tl::unexpected(ok.error());
    }
    auto m = std::make_unique<GcsModel>();
    m->sketch = canonicalSketch(input);
    const Sketch& s = m->sketch;
    const EntityIndex index(s);

    // Unknowns, in canonical entity order.
    std::size_t count = 0;
    for (const Entity& e : s.entities) {
        switch (kindOf(e)) {
        case EntityKind::Point:
            count += 2;
            break;
        case EntityKind::Circle:
            count += 1;
            break;
        case EntityKind::Arc:
            count += 3;
            break;
        default:
            break;
        }
    }
    m->params.assign(count, 0.0);
    m->geo.resize(s.entities.size());
    std::size_t next = 0;
    for (std::size_t i = 0; i < s.entities.size(); ++i) {
        const Entity& e = s.entities[i];
        EntityGeo& g = m->geo[i];
        g.kind = kindOf(e);
        if (const auto* p = std::get_if<Point>(&e.geom)) {
            g.firstParam = static_cast<int>(next);
            m->params[next] = p->p.x;
            m->params[next + 1] = p->p.y;
            g.point = GCS::Point(&m->params[next], &m->params[next + 1]);
            next += 2;
        } else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            g.firstParam = static_cast<int>(next);
            m->params[next] = c->radius;
            next += 1;
        } else if (std::holds_alternative<Arc>(e.geom)) {
            g.firstParam = static_cast<int>(next);
            next += 3;
        }
    }
    // Geometry referring to the points (second pass: every point has its parameters now).
    auto pointGeo = [&](SketchEntId id) {
        return m->geo[static_cast<std::size_t>(index.indexOf(id))].point;
    };
    for (std::size_t i = 0; i < s.entities.size(); ++i) {
        const Entity& e = s.entities[i];
        EntityGeo& g = m->geo[i];
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            g.line.p1 = pointGeo(l->a);
            g.line.p2 = pointGeo(l->b);
        } else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            g.circle.center = pointGeo(c->center);
            g.circle.rad = &m->params[static_cast<std::size_t>(g.firstParam)];
        } else if (const auto* a = std::get_if<Arc>(&e.geom)) {
            // PlaneGCS arcs run counter-clockwise from start to end; a clockwise arc is the same arc reversed.
            const SketchEntId from = a->ccw ? a->start : a->end;
            const SketchEntId to = a->ccw ? a->end : a->start;
            g.arc.center = pointGeo(a->center);
            g.arc.start = pointGeo(from);
            g.arc.end = pointGeo(to);
            double* own = &m->params[static_cast<std::size_t>(g.firstParam)];
            g.arc.rad = own;
            g.arc.startAngle = own + 1;
            g.arc.endAngle = own + 2;
            const double cx = *g.arc.center.x;
            const double cy = *g.arc.center.y;
            const double sx = *g.arc.start.x - cx;
            const double sy = *g.arc.start.y - cy;
            const double ex = *g.arc.end.x - cx;
            const double ey = *g.arc.end.y - cy;
            const double a0 = dmath::atan2(sy, sx);
            double a1 = dmath::atan2(ey, ex);
            while (a1 <= a0) {
                a1 += 2 * std::numbers::pi;
            }
            own[0] = dmath::sqrt(sx * sx + sy * sy);
            own[1] = a0;
            own[2] = a1;
        }
    }
    m->unknowns.reserve(m->params.size());
    for (double& p : m->params) {
        m->unknowns.push_back(&p);
    }
    return m;
}

Status emitConstraints(GcsModel& m, const ConstraintSink& sink)
{
    Emitter emit(m, sink);
    for (EntityGeo& g : m.geo) {
        if (g.kind == EntityKind::Arc) {
            emit.arcRules(g.arc);
        }
    }
    for (std::size_t i = 0; i < m.sketch.constraints.size(); ++i) {
        const Constraint& c = m.sketch.constraints[i];
        if (c.driven) {
            // Not enforced; reported by writeBack. Only a dimension can be driven.
            if (!measureConstraint(EntityIndex(m.sketch), c)) {
                return tl::unexpected(invalid(std::string("Sketch ") + constraintTypeName(c.type) + " constraint " +
                                              idText(c.id) + " cannot be a driven (reference) dimension"));
            }
            continue;
        }
        if (auto ok = emit.constraint(static_cast<int>(i) + 1, c); !ok) {
            return ok;
        }
    }
    return {};
}

Uuid tagId(const GcsModel& m, int tag)
{
    return m.sketch.constraints[static_cast<std::size_t>(tag - 1)].id;
}

std::vector<Uuid> tagIds(const GcsModel& m, const std::vector<int>& tags)
{
    std::set<int> sorted;
    for (int t : tags) {
        if (t >= 1 && static_cast<std::size_t>(t) <= m.sketch.constraints.size()) {
            sorted.insert(t);
        }
    }
    std::vector<Uuid> out;
    out.reserve(sorted.size());
    for (int t : sorted) {
        out.push_back(tagId(m, t));
    }
    return out;
}

Sketch writeBack(const GcsModel& m)
{
    Sketch out = m.sketch;
    for (std::size_t i = 0; i < out.entities.size(); ++i) {
        Entity& e = out.entities[i];
        const EntityGeo& g = m.geo[i];
        if (auto* p = std::get_if<Point>(&e.geom)) {
            p->p.x = *g.point.x;
            p->p.y = *g.point.y;
        } else if (auto* c = std::get_if<Circle>(&e.geom)) {
            c->radius = *g.circle.rad;
        }
    }
    const EntityIndex index(out);
    for (Constraint& c : out.constraints) {
        if (c.driven) {
            if (const auto v = measureConstraint(index, c)) {
                c.value = *v;
            }
        }
    }
    return out;
}

} // namespace ray::sketch::detail
