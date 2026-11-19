#include "constraints.h"

#include <algorithm>
#include <cmath>

namespace ray::ui::sketchui {

using sketch::ConstraintType;

namespace {

struct Picked {
    std::vector<SketchEntId> points, lines, circular, other;
};

Picked classify(const SketchView& view, const std::vector<SketchEntId>& selection)
{
    Picked p;
    for (const SketchEntId& id : selection) {
        if (view.pointEntry(id) != nullptr) {
            p.points.push_back(id);
        } else if (const ViewCurve* c = view.curve(id)) {
            if (c->kind == ViewCurve::Kind::Line)
                p.lines.push_back(id);
            else if (c->kind == ViewCurve::Kind::Arc || c->kind == ViewCurve::Kind::Circle)
                p.circular.push_back(id);
            else
                p.other.push_back(id);
        }
    }
    return p;
}

std::size_t total(const Picked& p)
{
    return p.points.size() + p.lines.size() + p.circular.size() + p.other.size();
}

bool uses(const SketchView& view, const SketchEntId& curve, const SketchEntId& point)
{
    const ViewCurve* c = view.curve(curve);
    return c != nullptr && std::find(c->points.begin(), c->points.end(), point) != c->points.end();
}

// End points of a line or arc (not the centre).
std::vector<SketchEntId> endsOf(const ViewCurve& c)
{
    if (c.kind == ViewCurve::Kind::Line)
        return c.points;
    if (c.kind == ViewCurve::Kind::Arc && c.points.size() == 3)
        return {c.points[1], c.points[2]};
    return {};
}

sketch::Constraint make(ConstraintType type, std::vector<SketchEntId> refs)
{
    sketch::Constraint c;
    c.id = Uuid::generate();
    c.type = type;
    c.refs = std::move(refs);
    return c;
}

bool sameRefs(std::vector<SketchEntId> a, std::vector<SketchEntId> b)
{
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

Vec2 mid(Vec2 a, Vec2 b)
{
    return mul(add(a, b), 0.5);
}

// A place on a curve for its glyph: the middle of a line, arc or slot, the top of a circle.
std::optional<Vec2> curveSpot(const SketchView& view, const SketchEntId& id)
{
    if (const auto p = view.point(id))
        return p;
    const ViewCurve* c = view.curve(id);
    if (c == nullptr)
        return std::nullopt;
    switch (c->kind) {
    case ViewCurve::Kind::Line:
    case ViewCurve::Kind::Slot:
        return mid(c->a, c->b);
    case ViewCurve::Kind::Circle:
        return add(c->c, Vec2{0, c->r});
    case ViewCurve::Kind::Arc:
        return polar(c->c, c->r, c->start + (c->ccw ? 1 : -1) * c->sweep * 0.5);
    case ViewCurve::Kind::Spline:
        if (!c->poly.empty())
            return c->poly[c->poly.size() / 2];
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

const char* geoName(GeoKind kind)
{
    switch (kind) {
    case GeoKind::Coincident:
        return "coincident";
    case GeoKind::Horizontal:
        return "horizontal";
    case GeoKind::Vertical:
        return "vertical";
    case GeoKind::Parallel:
        return "parallel";
    case GeoKind::Perpendicular:
        return "perpendicular";
    case GeoKind::Tangent:
        return "tangent";
    case GeoKind::Equal:
        return "equal";
    case GeoKind::Midpoint:
        return "midpoint";
    case GeoKind::Concentric:
        return "concentric";
    case GeoKind::Collinear:
        return "collinear";
    case GeoKind::Symmetric:
        return "symmetric";
    case GeoKind::Fix:
        return "fix";
    }
    return "";
}

std::optional<GeoKind> geoFromName(std::string_view name)
{
    for (const GeoKind k : kGeoKinds) {
        if (name == geoName(k))
            return k;
    }
    return std::nullopt;
}

const char* geoTitle(GeoKind kind)
{
    switch (kind) {
    case GeoKind::Coincident:
        return "Coincident";
    case GeoKind::Horizontal:
        return "Horizontal";
    case GeoKind::Vertical:
        return "Vertical";
    case GeoKind::Parallel:
        return "Parallel";
    case GeoKind::Perpendicular:
        return "Perpendicular";
    case GeoKind::Tangent:
        return "Tangent";
    case GeoKind::Equal:
        return "Equal";
    case GeoKind::Midpoint:
        return "Midpoint";
    case GeoKind::Concentric:
        return "Concentric";
    case GeoKind::Collinear:
        return "Collinear";
    case GeoKind::Symmetric:
        return "Symmetric";
    case GeoKind::Fix:
        return "Fix";
    }
    return "";
}

const char* geoRequirement(GeoKind kind)
{
    switch (kind) {
    case GeoKind::Coincident:
        return "Select two points, or a point and a curve";
    case GeoKind::Horizontal:
        return "Select one or more lines, or two points";
    case GeoKind::Vertical:
        return "Select one or more lines, or two points";
    case GeoKind::Parallel:
        return "Select two or more lines";
    case GeoKind::Perpendicular:
        return "Select two lines";
    case GeoKind::Tangent:
        return "Select an arc or circle and a line, arc or circle";
    case GeoKind::Equal:
        return "Select two or more lines, or two or more arcs and circles";
    case GeoKind::Midpoint:
        return "Select a point and a line";
    case GeoKind::Concentric:
        return "Select two arcs or circles";
    case GeoKind::Collinear:
        return "Select two lines";
    case GeoKind::Symmetric:
        return "Select two points and a line, or three points";
    case GeoKind::Fix:
        return "Select the points or curves to fix";
    }
    return "";
}

std::vector<sketch::Constraint> planConstraint(GeoKind kind, const SketchView& view, const sketch::Sketch& model,
                                               const std::vector<SketchEntId>& selection, bool* already)
{
    if (already != nullptr)
        *already = false;
    const Picked p = classify(view, selection);
    const std::size_t n = total(p);
    std::vector<sketch::Constraint> out;
    switch (kind) {
    case GeoKind::Coincident:
        if (n == 2 && p.points.size() == 2) {
            out.push_back(make(ConstraintType::Coincident, {p.points[0], p.points[1]}));
        } else if (n == 2 && p.points.size() == 1 && (p.lines.size() == 1 || p.circular.size() == 1)) {
            const SketchEntId curve = p.lines.empty() ? p.circular[0] : p.lines[0];
            if (!uses(view, curve, p.points[0]))
                out.push_back(make(ConstraintType::PointOnCurve, {p.points[0], curve}));
        }
        break;
    case GeoKind::Horizontal:
    case GeoKind::Vertical: {
        const ConstraintType t = kind == GeoKind::Horizontal ? ConstraintType::Horizontal : ConstraintType::Vertical;
        if (!p.lines.empty() && n == p.lines.size()) {
            for (const SketchEntId& l : p.lines)
                out.push_back(make(t, {l}));
        } else if (n == 2 && p.points.size() == 2) {
            out.push_back(make(t, {p.points[0], p.points[1]}));
        }
        break;
    }
    case GeoKind::Parallel:
    case GeoKind::Perpendicular:
    case GeoKind::Collinear:
        if (p.lines.size() >= 2 && n == p.lines.size() && (kind == GeoKind::Parallel || n == 2)) {
            if (kind == GeoKind::Collinear) {
                const ViewCurve* first = view.curve(p.lines[0]);
                const ViewCurve* second = view.curve(p.lines[1]);
                for (const SketchEntId& e : second->points) {
                    if (std::find(first->points.begin(), first->points.end(), e) == first->points.end())
                        out.push_back(make(ConstraintType::PointOnCurve, {e, p.lines[0]}));
                }
            } else {
                const ConstraintType t =
                    kind == GeoKind::Parallel ? ConstraintType::Parallel : ConstraintType::Perpendicular;
                for (std::size_t i = 1; i < p.lines.size(); ++i)
                    out.push_back(make(t, {p.lines[0], p.lines[i]}));
            }
        }
        break;
    case GeoKind::Tangent:
        if (n == 2 && !p.circular.empty() && p.points.empty() && p.other.empty()) {
            std::vector<SketchEntId> curves = p.lines;
            curves.insert(curves.end(), p.circular.begin(), p.circular.end());
            const ViewCurve* a = view.curve(curves[0]);
            const ViewCurve* b = view.curve(curves[1]);
            std::optional<SketchEntId> shared;
            for (const SketchEntId& e : endsOf(*a)) {
                const auto be = endsOf(*b);
                if (std::find(be.begin(), be.end(), e) != be.end())
                    shared = e;
            }
            if (shared)
                out.push_back(make(ConstraintType::Tangent, {curves[0], curves[1], *shared}));
            else
                out.push_back(make(ConstraintType::Tangent, {curves[0], curves[1]}));
        }
        break;
    case GeoKind::Equal:
        if (p.lines.size() >= 2 && n == p.lines.size()) {
            for (std::size_t i = 1; i < p.lines.size(); ++i)
                out.push_back(make(ConstraintType::Equal, {p.lines[0], p.lines[i]}));
        } else if (p.circular.size() >= 2 && n == p.circular.size()) {
            for (std::size_t i = 1; i < p.circular.size(); ++i)
                out.push_back(make(ConstraintType::Equal, {p.circular[0], p.circular[i]}));
        }
        break;
    case GeoKind::Midpoint:
        if (n == 2 && p.points.size() == 1 && p.lines.size() == 1 && !uses(view, p.lines[0], p.points[0]))
            out.push_back(make(ConstraintType::Midpoint, {p.points[0], p.lines[0]}));
        break;
    case GeoKind::Concentric:
        if (n == 2 && p.circular.size() == 2) {
            const ViewCurve* a = view.curve(p.circular[0]);
            const ViewCurve* b = view.curve(p.circular[1]);
            if (a->points.empty() || b->points.empty() || a->points[0] != b->points[0])
                out.push_back(make(ConstraintType::Concentric, {p.circular[0], p.circular[1]}));
        }
        break;
    case GeoKind::Symmetric:
        if (n == 3 && p.points.size() == 2 && p.lines.size() == 1) {
            if (!uses(view, p.lines[0], p.points[0]) && !uses(view, p.lines[0], p.points[1]))
                out.push_back(make(ConstraintType::Symmetric, {p.points[0], p.points[1], p.lines[0]}));
        } else if (n == 3 && p.points.size() == 3) {
            // The centre is the point nearest the middle of the other two.
            std::size_t centre = 0;
            double best = 1e300;
            for (std::size_t i = 0; i < 3; ++i) {
                const Vec2 m = mid(*view.point(p.points[(i + 1) % 3]), *view.point(p.points[(i + 2) % 3]));
                const double d = distance(m, *view.point(p.points[i]));
                if (d < best) {
                    best = d;
                    centre = i;
                }
            }
            out.push_back(make(ConstraintType::Symmetric,
                               {p.points[(centre + 1) % 3], p.points[(centre + 2) % 3], p.points[centre]}));
        }
        break;
    case GeoKind::Fix:
        for (const auto* group : {&p.points, &p.lines, &p.circular, &p.other}) {
            for (const SketchEntId& e : *group)
                out.push_back(make(ConstraintType::Fix, {e}));
        }
        break;
    }
    // Leave out what the model has already.
    const std::size_t planned = out.size();
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const sketch::Constraint& c) {
                                 return std::any_of(model.constraints.begin(), model.constraints.end(),
                                                    [&](const sketch::Constraint& m) {
                                                        return m.type == c.type && sameRefs(m.refs, c.refs);
                                                    });
                             }),
              out.end());
    if (already != nullptr)
        *already = planned > 0 && out.empty();
    return out;
}

const char* glyphOf(ConstraintType type)
{
    switch (type) {
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
        return "onCurve";
    case ConstraintType::Fix:
        return "fix";
    default:
        return "dimension";
    }
}

const char* typeTitle(ConstraintType type)
{
    switch (type) {
    case ConstraintType::Coincident:
        return "Coincident";
    case ConstraintType::Horizontal:
        return "Horizontal";
    case ConstraintType::Vertical:
        return "Vertical";
    case ConstraintType::Parallel:
        return "Parallel";
    case ConstraintType::Perpendicular:
        return "Perpendicular";
    case ConstraintType::Tangent:
        return "Tangent";
    case ConstraintType::Equal:
        return "Equal";
    case ConstraintType::Symmetric:
        return "Symmetric";
    case ConstraintType::Concentric:
        return "Concentric";
    case ConstraintType::Midpoint:
        return "Midpoint";
    case ConstraintType::PointOnCurve:
        return "Point on curve";
    case ConstraintType::Fix:
        return "Fix";
    case ConstraintType::Distance:
        return "Distance";
    case ConstraintType::DistanceX:
        return "Horizontal distance";
    case ConstraintType::DistanceY:
        return "Vertical distance";
    case ConstraintType::Radius:
        return "Radius";
    case ConstraintType::Diameter:
        return "Diameter";
    case ConstraintType::Angle:
        return "Angle";
    }
    return "";
}

std::vector<Vec2> glyphPlaces(const SketchView& view, const sketch::Constraint& c)
{
    std::vector<Vec2> out;
    const auto spot = [&](std::size_t i) {
        return i < c.refs.size() ? curveSpot(view, c.refs[i]) : std::nullopt;
    };
    const auto push = [&](std::optional<Vec2> p) {
        if (p)
            out.push_back(*p);
    };
    switch (c.type) {
    case ConstraintType::Coincident:
    case ConstraintType::PointOnCurve:
    case ConstraintType::Midpoint:
        for (std::size_t i = 0; i < c.refs.size(); ++i) {
            if (view.pointEntry(c.refs[i]) != nullptr) {
                push(view.point(c.refs[i]));
                break;
            }
        }
        break;
    case ConstraintType::Horizontal:
    case ConstraintType::Vertical:
        if (c.refs.size() == 2 && view.point(c.refs[0]) && view.point(c.refs[1]))
            out.push_back(mid(*view.point(c.refs[0]), *view.point(c.refs[1])));
        else
            push(spot(0));
        break;
    case ConstraintType::Tangent:
        if (c.refs.size() == 3) {
            push(view.point(c.refs[2]));
        } else if (c.refs.size() == 2) {
            const ViewCurve* a = view.curve(c.refs[0]);
            const ViewCurve* b = view.curve(c.refs[1]);
            if (a != nullptr && b != nullptr) {
                const ViewCurve* line = a->kind == ViewCurve::Kind::Line   ? a
                                        : b->kind == ViewCurve::Kind::Line ? b
                                                                           : nullptr;
                const ViewCurve* round = line == a ? b : a;
                if (line != nullptr) {
                    out.push_back(nearestOn(*line, round->c));
                } else {
                    Vec2 d = unit(sub(b->c, a->c));
                    if (!(length(d) > 0))
                        d = {1, 0};
                    out.push_back(add(a->c, mul(d, a->r)));
                }
            }
        }
        break;
    case ConstraintType::Concentric:
        if (const ViewCurve* a = c.refs.empty() ? nullptr : view.curve(c.refs[0]))
            out.push_back(a->c);
        break;
    case ConstraintType::Symmetric:
        push(spot(0));
        push(spot(1));
        break;
    case ConstraintType::Parallel:
    case ConstraintType::Perpendicular:
    case ConstraintType::Equal:
    case ConstraintType::Fix:
        for (std::size_t i = 0; i < c.refs.size(); ++i)
            push(spot(i));
        break;
    default:
        break;
    }
    return out;
}

} // namespace ray::ui::sketchui
