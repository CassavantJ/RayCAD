// Auto-constraint suggestions while drawing (task T-091): sketch::inferConstraints.
//
// For the entity just drawn, proposes only what the geometry already shows within `tolerance` (mm):
//   Coincident     an end point (a line's ends, an arc's ends and centre, a circle's centre, a spline's ends, a
//                  slot's centres, or the point itself) within tolerance of another point; one proposal per end
//                  point, to the nearest point, and never two end points to the same point.
//   Horizontal /   a line whose ends differ by at most tolerance in y (x), and is longer than tolerance.
//   Vertical
//   Parallel /     a line (not proposed horizontal or vertical) whose far end is at most tolerance off being
//   Perpendicular  parallel or perpendicular to another line, measured over the new line's length; only the best
//                  single match, so a set of parallel lines does not give redundant proposals.
//   Tangent        at a joint: a line and an arc, or two arcs/circles, meeting at an end point (shared, or within
//                  tolerance) whose directions there differ by at most tolerance over the shorter curve's length
//                  (3 references: curve, curve, joint point). Elsewhere: a line touching a circle or arc, or two
//                  circles/arcs touching, within tolerance, at a point inside both (2 references).
// Nothing is proposed that the sketch already has. Ids are derived from the new entity's id, so the same call gives
// the same proposals.
#include "loops_curves.h"
#include "model.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace ray::sketch {

using namespace detail;

namespace {

constexpr double kTwoPi = 2 * std::numbers::pi;

struct Ctx {
    const Sketch& s;
    const EntityIndex& index;
    double tol;
};

std::optional<Vec2> pt(const Ctx& c, SketchEntId id)
{
    return pointOf(c.index, id);
}

// The point entities a curve uses as end points (where it can join another curve or point).
std::vector<SketchEntId> endPoints(const Entity& e)
{
    if (std::holds_alternative<Point>(e.geom)) {
        return {e.id};
    }
    if (const auto* l = std::get_if<Line>(&e.geom)) {
        return {l->a, l->b};
    }
    if (const auto* a = std::get_if<Arc>(&e.geom)) {
        return {a->start, a->end, a->center};
    }
    if (const auto* c = std::get_if<Circle>(&e.geom)) {
        return {c->center};
    }
    if (const auto* sp = std::get_if<Spline>(&e.geom)) {
        if (sp->closed || sp->controlPoints.empty()) {
            return {};
        }
        return {sp->controlPoints.front(), sp->controlPoints.back()};
    }
    if (const auto* sl = std::get_if<Slot>(&e.geom)) {
        return {sl->a, sl->b};
    }
    return {};
}

// Every point entity the entity refers to, and the entity itself.
std::set<SketchEntId> ownPoints(const Entity& e)
{
    std::set<SketchEntId> out{e.id};
    for (SketchEntId p : endPoints(e)) {
        out.insert(p);
    }
    if (const auto* sp = std::get_if<Spline>(&e.geom)) {
        out.insert(sp->controlPoints.begin(), sp->controlPoints.end());
    }
    return out;
}

bool hasConstraint(const Sketch& s, ConstraintType t, std::vector<SketchEntId> refs)
{
    std::sort(refs.begin(), refs.end());
    for (const Constraint& c : s.constraints) {
        if (c.type != t || c.refs.size() != refs.size()) {
            continue;
        }
        std::vector<SketchEntId> r = c.refs;
        std::sort(r.begin(), r.end());
        if (r == refs) {
            return true;
        }
    }
    return false;
}

bool hasConstraintOn(const Sketch& s, ConstraintType t, SketchEntId a, SketchEntId b)
{
    for (const Constraint& c : s.constraints) {
        if (c.type == t && std::find(c.refs.begin(), c.refs.end(), a) != c.refs.end() &&
            std::find(c.refs.begin(), c.refs.end(), b) != c.refs.end()) {
            return true;
        }
    }
    return false;
}

struct LineGeo {
    Vec2 a, b;
    double len() const { return dist(a, b); }
    Vec2 unit() const { return (1 / len()) * (b - a); }
};
struct CircleGeo {
    Vec2 c;
    double r;
    bool isArc = false;
    Vec2 start, end; // arc
    bool ccw = true;
    double sweep = kTwoPi;
};

std::optional<LineGeo> lineGeo(const Ctx& c, const Entity& e)
{
    const auto* l = std::get_if<Line>(&e.geom);
    if (!l) {
        return std::nullopt;
    }
    const auto a = pt(c, l->a);
    const auto b = pt(c, l->b);
    if (!a || !b || !(dist(*a, *b) > c.tol)) {
        return std::nullopt;
    }
    return LineGeo{*a, *b};
}

double arcAngle(Vec2 centre, Vec2 from, Vec2 to, bool ccw)
{
    const Vec2 u = from - centre;
    const Vec2 v = to - centre;
    double ang = dmath::atan2(cross(u, v), dot(u, v));
    if (!ccw) {
        ang = -ang;
    }
    if (ang < 0) {
        ang += kTwoPi;
    }
    return ang;
}

std::optional<CircleGeo> circleGeo(const Ctx& c, const Entity& e)
{
    if (const auto* ci = std::get_if<Circle>(&e.geom)) {
        const auto centre = pt(c, ci->center);
        if (!centre || !(ci->radius > 0)) {
            return std::nullopt;
        }
        CircleGeo g;
        g.c = *centre;
        g.r = ci->radius;
        return g;
    }
    if (const auto* a = std::get_if<Arc>(&e.geom)) {
        const auto centre = pt(c, a->center);
        const auto s = pt(c, a->start);
        const auto t = pt(c, a->end);
        if (!centre || !s || !t) {
            return std::nullopt;
        }
        CircleGeo g{*centre, dist(*centre, *s), true, *s, *t, a->ccw, 0};
        if (!(g.r > 0)) {
            return std::nullopt;
        }
        g.sweep = arcAngle(g.c, g.start, g.end, g.ccw);
        return g;
    }
    return std::nullopt;
}

bool onCircle(const CircleGeo& g, Vec2 p)
{
    return !g.isArc || arcAngle(g.c, g.start, p, g.ccw) <= g.sweep;
}

double curveLength(const Ctx& c, const Entity& e)
{
    if (auto l = lineGeo(c, e)) {
        return l->len();
    }
    if (auto g = circleGeo(c, e)) {
        return g->r * g->sweep;
    }
    return 0;
}

// The unit direction in which entity e runs at its end point `at` (a line's or arc's own direction).
std::optional<Vec2> directionAt(const Ctx& c, const Entity& e, SketchEntId at)
{
    if (auto l = lineGeo(c, e)) {
        return l->unit();
    }
    const auto* a = std::get_if<Arc>(&e.geom);
    if (!a || (at != a->start && at != a->end)) {
        return std::nullopt;
    }
    const auto g = circleGeo(c, e);
    const auto p = pt(c, at);
    if (!g || !p) {
        return std::nullopt;
    }
    const Vec2 radial = (1 / g->r) * (*p - g->c);
    return a->ccw ? perpLeft(radial) : -1.0 * perpLeft(radial);
}

class Proposals {
public:
    Proposals(const Sketch& s, SketchEntId owner) : s_(s), owner_(owner) {}

    void add(ConstraintType t, std::vector<SketchEntId> refs)
    {
        if (hasConstraint(s_, t, refs)) {
            return;
        }
        for (const Constraint& c : out_) {
            std::vector<SketchEntId> a = c.refs;
            std::vector<SketchEntId> b = refs;
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (c.type == t && a == b) {
                return;
            }
        }
        Constraint c;
        Hasher h;
        h.str("raycad.sketch.infer");
        h.bytes(std::as_bytes(std::span(owner_.value.bytes)));
        h.i64(static_cast<std::int64_t>(out_.size()));
        const Hash256 hash = h.finish();
        std::copy_n(hash.bytes.begin(), 16, c.id.bytes.begin());
        c.id.bytes[6] = static_cast<std::uint8_t>((c.id.bytes[6] & 0x0F) | 0x80); // version 8: derived
        c.id.bytes[8] = static_cast<std::uint8_t>((c.id.bytes[8] & 0x3F) | 0x80); // RFC 4122 variant
        c.type = t;
        c.refs = std::move(refs);
        out_.push_back(std::move(c));
    }
    std::vector<Constraint> take() { return std::move(out_); }

private:
    const Sketch& s_;
    SketchEntId owner_;
    std::vector<Constraint> out_;
};

} // namespace

std::vector<Constraint> inferConstraints(const Sketch& input, SketchEntId newEntity, double tolerance)
{
    if (!(tolerance > 0) || !std::isfinite(tolerance)) {
        return {};
    }
    const Sketch s = canonicalSketch(input);
    const EntityIndex index(s);
    const Entity* e = index.find(newEntity);
    if (!e) {
        return {};
    }
    const Ctx c{s, index, tolerance};
    Proposals out(s, newEntity);
    const std::set<SketchEntId> own = ownPoints(*e);

    // Coincident: each end point to the nearest other point.
    std::set<SketchEntId> taken;
    std::vector<std::pair<SketchEntId, SketchEntId>> joined; // (own end point, other point)
    for (SketchEntId p : endPoints(*e)) {
        const auto pp = pt(c, p);
        if (!pp) {
            continue;
        }
        const Entity* best = nullptr;
        double bestD = tolerance;
        for (const Entity& q : s.entities) {
            if (!std::holds_alternative<Point>(q.geom) || own.count(q.id) || taken.count(q.id)) {
                continue;
            }
            const double d = dist(*pp, std::get<Point>(q.geom).p);
            if (d <= bestD && (!best || d < bestD)) {
                best = &q;
                bestD = d;
            }
        }
        if (best) {
            taken.insert(best->id);
            joined.emplace_back(p, best->id);
            out.add(ConstraintType::Coincident, {p, best->id});
        }
    }

    // Horizontal, vertical, parallel, perpendicular.
    if (const auto line = lineGeo(c, *e)) {
        const double dx = std::fabs(line->b.x - line->a.x);
        const double dy = std::fabs(line->b.y - line->a.y);
        bool axis = false;
        if (dy <= tolerance && dy <= dx) {
            axis = true;
            out.add(ConstraintType::Horizontal, {e->id});
        } else if (dx <= tolerance && dx < dy) {
            axis = true;
            out.add(ConstraintType::Vertical, {e->id});
        }
        if (!axis) {
            const Vec2 u = line->unit();
            const double len = line->len();
            const Entity* best = nullptr;
            ConstraintType bestType = ConstraintType::Parallel;
            double bestDev = tolerance;
            for (const Entity& o : s.entities) {
                if (o.id == e->id) {
                    continue;
                }
                const auto other = lineGeo(c, o);
                if (!other) {
                    continue;
                }
                const Vec2 v = other->unit();
                const double par = std::fabs(cross(u, v)) * len;
                const double perp = std::fabs(dot(u, v)) * len;
                for (const auto& [dev, type] :
                     {std::pair{par, ConstraintType::Parallel}, std::pair{perp, ConstraintType::Perpendicular}}) {
                    if (dev <= bestDev && (!best || dev < bestDev)) {
                        best = &o;
                        bestDev = dev;
                        bestType = type;
                    }
                }
            }
            if (best && !hasConstraintOn(s, ConstraintType::Parallel, e->id, best->id) &&
                !hasConstraintOn(s, ConstraintType::Perpendicular, e->id, best->id)) {
                out.add(bestType, {e->id, best->id});
            }
        }
    }

    // Tangent.
    const bool eLine = lineGeo(c, *e).has_value();
    const auto eCircle = circleGeo(c, *e);
    if (eLine || eCircle) {
        // Where the new curve's ends meet other points: shared ids and the coincidences just proposed.
        std::vector<std::pair<SketchEntId, SketchEntId>> joints = joined;
        for (SketchEntId p : endPoints(*e)) {
            joints.emplace_back(p, p);
        }
        for (const Entity& o : s.entities) {
            if (o.id == e->id) {
                continue;
            }
            const bool oLine = lineGeo(c, o).has_value();
            const auto oCircle = circleGeo(c, o);
            if ((!oLine && !oCircle) || (eLine && oLine)) {
                continue;
            }
            if (hasConstraintOn(s, ConstraintType::Tangent, e->id, o.id)) {
                continue;
            }
            // At a joint.
            bool atJoint = false;
            const std::vector<SketchEntId> oEnds = endPoints(o);
            for (const auto& [mine, theirs] : joints) {
                if (std::find(oEnds.begin(), oEnds.end(), theirs) == oEnds.end()) {
                    continue;
                }
                const auto* oa = std::get_if<Arc>(&o.geom);
                const auto* ea = std::get_if<Arc>(&e->geom);
                if ((oa && theirs == oa->center) || (ea && mine == ea->center)) {
                    continue;
                }
                if (std::holds_alternative<Circle>(o.geom) || std::holds_alternative<Circle>(e->geom)) {
                    continue;
                }
                atJoint = true;
                const auto de = directionAt(c, *e, mine);
                const auto dO = directionAt(c, o, theirs);
                if (!de || !dO) {
                    continue;
                }
                const double len = std::min(curveLength(c, *e), curveLength(c, o));
                if (std::fabs(cross(*de, *dO)) * len <= tolerance) {
                    out.add(ConstraintType::Tangent, {e->id, o.id, mine});
                }
            }
            if (atJoint) {
                continue;
            }
            // Touching elsewhere.
            std::optional<Vec2> touch;
            if (eLine != oLine) {
                const LineGeo l = eLine ? *lineGeo(c, *e) : *lineGeo(c, o);
                const CircleGeo g = eLine ? *circleGeo(c, o) : *eCircle;
                const Vec2 u = l.unit();
                const double t = dot(g.c - l.a, u);
                const Vec2 foot = l.a + t * u;
                if (std::fabs(dist(foot, g.c) - g.r) <= tolerance && t > tolerance && t < l.len() - tolerance &&
                    dist(foot, g.c) > 0) {
                    const Vec2 p = g.c + (g.r / dist(foot, g.c)) * (foot - g.c);
                    if (onCircle(g, p)) {
                        touch = p;
                    }
                }
            } else {
                const CircleGeo g1 = *eCircle;
                const CircleGeo g2 = *circleGeo(c, o);
                const double d = dist(g1.c, g2.c);
                if (d > tolerance) {
                    const Vec2 u = (1 / d) * (g2.c - g1.c);
                    std::optional<Vec2> p;
                    if (std::fabs(d - (g1.r + g2.r)) <= tolerance) {
                        p = g1.c + g1.r * u;
                    } else if (std::fabs(d - std::fabs(g1.r - g2.r)) <= tolerance) {
                        p = g1.r >= g2.r ? g1.c + g1.r * u : g1.c - g1.r * u;
                    }
                    if (p && onCircle(g1, *p) && onCircle(g2, *p)) {
                        touch = p;
                    }
                }
            }
            if (touch) {
                // Not where either curve ends: that is a joint, not a touch.
                bool nearEnd = false;
                for (const Entity* x : {e, &o}) {
                    for (SketchEntId p : endPoints(*x)) {
                        const auto pp = pt(c, p);
                        const auto* xa = std::get_if<Arc>(&x->geom);
                        if (pp && !(xa && p == xa->center) && !std::holds_alternative<Circle>(x->geom) &&
                            dist(*pp, *touch) <= tolerance) {
                            nearEnd = true;
                        }
                    }
                }
                if (!nearEnd) {
                    out.add(ConstraintType::Tangent, {e->id, o.id});
                }
            }
        }
    }
    return out.take();
}

} // namespace ray::sketch
