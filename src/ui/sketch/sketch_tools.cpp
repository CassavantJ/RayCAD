#include "sketch_tools.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace ray::ui::sketchui {

namespace {

using sketch::ConstraintType;

constexpr double kTiny = 1e-9;

struct NamedTool {
    ToolKind kind;
    const char* name;
};
constexpr NamedTool kTools[] = {
    {ToolKind::Select, "select"},       {ToolKind::Point, "point"},
    {ToolKind::Line, "line"},           {ToolKind::Rectangle, "rectangle"},
    {ToolKind::Circle, "circle"},       {ToolKind::Arc3, "arc3"},
    {ToolKind::ArcCenter, "arcCenter"}, {ToolKind::ArcTangent, "arcTangent"},
    {ToolKind::Slot, "slot"},           {ToolKind::Spline, "spline"},
    {ToolKind::Trim, "trim"},           {ToolKind::Extend, "extend"},
    {ToolKind::Offset, "offset"},       {ToolKind::Fillet, "fillet"},
};

bool chainTool(ToolKind k)
{
    return k == ToolKind::Line || k == ToolKind::ArcTangent;
}

} // namespace

ToolKind toolFromName(const std::string& name)
{
    for (const NamedTool& t : kTools) {
        if (name == t.name)
            return t.kind;
    }
    return ToolKind::Select;
}

std::string toolName(ToolKind kind)
{
    for (const NamedTool& t : kTools) {
        if (kind == t.kind)
            return t.name;
    }
    return "select";
}

const char* glyphName(Glyph::Kind kind)
{
    switch (kind) {
    case Glyph::Kind::Coincident:
        return "coincident";
    case Glyph::Kind::Center:
        return "center";
    case Glyph::Kind::Midpoint:
        return "midpoint";
    case Glyph::Kind::OnCurve:
        return "onCurve";
    case Glyph::Kind::Horizontal:
        return "horizontal";
    case Glyph::Kind::Vertical:
        return "vertical";
    case Glyph::Kind::Tangent:
        return "tangent";
    case Glyph::Kind::Dimension:
        return "dimension";
    }
    return "";
}

std::string jsonNumber(double v)
{
    if (!std::isfinite(v))
        v = 0;
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

std::string jsonPoint(Vec2 p)
{
    return "{\"x\":" + jsonNumber(p.x) + ",\"y\":" + jsonNumber(p.y) + "}";
}

// ---------- snapping ----------
Snap snapAt(const SketchView& view, Vec2 q, double tol, const SnapOptions& options)
{
    Snap s;
    s.p = q;
    if (!options.enabled)
        return s;
    // 1. An existing point.
    const SketchView::Hit hp = view.hitPoint(q, tol, options.exclude);
    if (hp.kind == SketchView::Hit::Kind::Point) {
        s.kind = Snap::Kind::Point;
        s.p = hp.at;
        s.ref = hp.id;
        const ViewPoint* vp = view.pointEntry(hp.id);
        s.center = vp != nullptr && vp->center;
        return s;
    }
    // 2. The midpoint of a line.
    for (const ViewCurve& c : view.curves()) {
        if (c.kind != ViewCurve::Kind::Line || options.exclude.count(c.id))
            continue;
        const Vec2 m = mul(add(c.a, c.b), 0.5);
        if (distance(m, q) <= tol) {
            s.kind = Snap::Kind::Midpoint;
            s.p = m;
            s.ref = c.id;
            return s;
        }
    }
    // 3. Horizontal / vertical from the anchor.
    Vec2 p = q;
    if (options.anchor) {
        const Vec2 d = sub(q, *options.anchor);
        if (std::abs(d.y) <= tol && std::abs(d.x) > tol) {
            s.axis = Snap::Axis::Horizontal;
            p = {q.x, options.anchor->y};
        } else if (std::abs(d.x) <= tol && std::abs(d.y) > tol) {
            s.axis = Snap::Axis::Vertical;
            p = {options.anchor->x, q.y};
        }
    }
    // 4. A curve (with an axis: where the axis line crosses it).
    const SketchView::Hit hc = view.hitCurve(p, tol, options.exclude);
    if (hc.kind == SketchView::Hit::Kind::Curve) {
        const ViewCurve* c = view.curve(hc.id);
        std::optional<Vec2> on;
        if (s.axis == Snap::Axis::None) {
            on = hc.at;
        } else if (c != nullptr) {
            const bool h = s.axis == Snap::Axis::Horizontal;
            for (const Vec2& x : crossAxisLine(*c, h, h ? p.y : p.x)) {
                if (distance(x, p) <= tol && (!on || distance(x, p) < distance(*on, p)))
                    on = x;
            }
        }
        if (on) {
            s.kind = Snap::Kind::OnCurve;
            s.p = *on;
            s.ref = hc.id;
            s.curveConstrains = c != nullptr && (c->kind == ViewCurve::Kind::Line || c->kind == ViewCurve::Kind::Arc ||
                                                 c->kind == ViewCurve::Kind::Circle);
            return s;
        }
    }
    s.p = p;
    if (s.axis != Snap::Axis::None)
        return s;
    // 5. Lined up with another point.
    double bestX = tol, bestY = tol;
    const ViewPoint* atX = nullptr;
    const ViewPoint* atY = nullptr;
    for (const ViewPoint& vp : view.points()) {
        if (options.exclude.count(vp.id))
            continue;
        const double dx = std::abs(vp.p.x - q.x), dy = std::abs(vp.p.y - q.y);
        if (dx < bestX) {
            bestX = dx;
            atX = &vp;
        }
        if (dy < bestY) {
            bestY = dy;
            atY = &vp;
        }
    }
    if (atX != nullptr) {
        s.p.x = atX->p.x;
        s.alignV = atX->id;
    }
    if (atY != nullptr) {
        s.p.y = atY->p.y;
        s.alignH = atY->id;
    }
    if (atX != nullptr)
        s.guides.emplace_back(atX->p, s.p);
    if (atY != nullptr)
        s.guides.emplace_back(atY->p, s.p);
    return s;
}

std::vector<SketchEntId> connectedChain(const SketchView& view, const SketchEntId& seed)
{
    const ViewCurve* first = view.curve(seed);
    if (first == nullptr)
        return {};
    if (first->kind != ViewCurve::Kind::Line && first->kind != ViewCurve::Kind::Arc)
        return {seed};
    const auto ends = [](const ViewCurve& c) {
        return c.kind == ViewCurve::Kind::Line ? std::vector<SketchEntId>{c.points[0], c.points[1]}
                                               : std::vector<SketchEntId>{c.points[1], c.points[2]};
    };
    std::vector<SketchEntId> out{seed};
    std::set<SketchEntId> seen{seed};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const ViewCurve* c = view.curve(out[i]);
        for (const SketchEntId& p : ends(*c)) {
            for (const ViewCurve* n : view.curvesAt(p)) {
                if ((n->kind != ViewCurve::Kind::Line && n->kind != ViewCurve::Kind::Arc) || seen.count(n->id))
                    continue;
                const auto e = ends(*n);
                if (std::find(e.begin(), e.end(), p) == e.end())
                    continue; // only the centre of an arc
                seen.insert(n->id);
                out.push_back(n->id);
            }
        }
    }
    return out;
}

// ---------- the tool engine ----------
void ToolEngine::setKind(ToolKind kind)
{
    const bool keepChain = chainTool(kind_) && chainTool(kind) && placed_.size() == 1 && placed_[0].committed;
    kind_ = kind;
    if (!keepChain) {
        placed_.clear();
        chainStart_.reset();
        lastCurve_.reset();
    }
    offsetChain_.clear();
    tangentFrom_.reset();
    hovered_ = {};
    arcWinding_ = 0;
    resetTyped();
    draft_ = {};
}

bool ToolEngine::busy() const
{
    return !placed_.empty() || !offsetChain_.empty();
}

bool ToolEngine::cancel()
{
    const bool was = busy() || !typed_.empty();
    placed_.clear();
    chainStart_.reset();
    lastCurve_.reset();
    offsetChain_.clear();
    tangentFrom_.reset();
    arcWinding_ = 0;
    resetTyped();
    draft_ = {};
    return was;
}

void ToolEngine::modelChanged(const SketchView& view)
{
    // A committed point that is gone (undo) ends the operation.
    for (const Placed& p : placed_) {
        if (p.committed && !view.point(p.id)) {
            cancel();
            return;
        }
    }
    if (lastCurve_ && view.curve(*lastCurve_) == nullptr)
        lastCurve_.reset();
    for (const SketchEntId& id : offsetChain_) {
        if (view.curve(id) == nullptr) {
            offsetChain_.clear();
            break;
        }
    }
}

SketchEntId ToolEngine::pointFor(const Placed& p, Batch& b) const
{
    if (p.committed)
        return p.id;
    if (p.snap.kind == Snap::Kind::Point)
        return p.snap.ref;
    b.entities.push_back(sketch::Entity{p.id, sketch::Point{p.snap.p}, false});
    if (p.snap.kind == Snap::Kind::Midpoint)
        addConstraint(b, ConstraintType::Midpoint, {p.id, p.snap.ref});
    if (p.snap.kind == Snap::Kind::OnCurve && p.snap.curveConstrains)
        addConstraint(b, ConstraintType::PointOnCurve, {p.id, p.snap.ref});
    if (p.snap.alignH)
        addConstraint(b, ConstraintType::Horizontal, {p.id, *p.snap.alignH});
    if (p.snap.alignV)
        addConstraint(b, ConstraintType::Vertical, {p.id, *p.snap.alignV});
    return p.id;
}

SketchEntId ToolEngine::addCurve(Batch& b, sketch::Entity e) const
{
    e.id = SketchEntId{Uuid::generate()};
    e.construction = construction_;
    b.entities.push_back(e);
    return e.id;
}

void ToolEngine::addConstraint(Batch& b, ConstraintType type, std::vector<SketchEntId> refs, double value) const
{
    sketch::Constraint c;
    c.id = Uuid::generate();
    c.type = type;
    c.refs = std::move(refs);
    c.value = value;
    b.constraints.push_back(std::move(c));
}

void ToolEngine::glyphsFor(const Snap& s, std::vector<Glyph>& out) const
{
    switch (s.kind) {
    case Snap::Kind::Point:
        out.push_back({s.center ? Glyph::Kind::Center : Glyph::Kind::Coincident, s.p});
        break;
    case Snap::Kind::Midpoint:
        out.push_back({Glyph::Kind::Midpoint, s.p});
        break;
    case Snap::Kind::OnCurve:
        out.push_back({Glyph::Kind::OnCurve, s.p});
        break;
    case Snap::Kind::Free:
        break;
    }
    if (s.alignH)
        out.push_back({Glyph::Kind::Horizontal, s.p});
    if (s.alignV)
        out.push_back({Glyph::Kind::Vertical, s.p});
}

namespace {

std::optional<double> typedValue(const std::map<std::string, double>& typed, const char* key)
{
    const auto it = typed.find(key);
    return it == typed.end() ? std::nullopt : std::optional<double>(it->second);
}

} // namespace

Vec2 ToolEngine::target() const
{
    if (kind_ == ToolKind::Point) {
        const auto x = typedValue(typed_, "x");
        const auto y = typedValue(typed_, "y");
        return {x ? *x : cursor_.x, y ? *y : cursor_.y};
    }
    if (placed_.empty())
        return cursor_;
    const Vec2 s = placed_.back().snap.p;
    const Vec2 d = sub(cursor_, s);
    const auto lengthAngle = [&]() {
        const auto len = typedValue(typed_, "length");
        const auto ang = typedValue(typed_, "angle");
        if (!len && !ang)
            return cursor_;
        const double a = ang ? *ang : angleOf(d);
        const double l = len ? *len : std::max(0.0, dot(d, Vec2{std::cos(a), std::sin(a)}));
        return polar(s, l, a);
    };
    switch (kind_) {
    case ToolKind::Line:
        return lengthAngle();
    case ToolKind::Slot:
        return placed_.size() == 1 ? lengthAngle() : cursor_;
    case ToolKind::Rectangle: {
        const auto w = typedValue(typed_, "width");
        const auto h = typedValue(typed_, "height");
        return {w ? s.x + (d.x < 0 ? -*w : *w) : cursor_.x, h ? s.y + (d.y < 0 ? -*h : *h) : cursor_.y};
    }
    case ToolKind::Circle: {
        const auto dia = typedValue(typed_, "diameter");
        if (!dia)
            return cursor_;
        const Vec2 u = length(d) > kTiny ? unit(d) : Vec2{1, 0};
        return add(s, mul(u, *dia / 2));
    }
    case ToolKind::ArcCenter: {
        const auto r = typedValue(typed_, "radius");
        if (!r || placed_.size() != 1)
            return cursor_;
        const Vec2 u = length(d) > kTiny ? unit(d) : Vec2{1, 0};
        return add(s, mul(u, *r));
    }
    default:
        return cursor_;
    }
}

std::vector<FieldSpec> ToolEngine::fields() const
{
    std::vector<FieldSpec> out;
    const Vec2 t = target();
    const Vec2 s = placed_.empty() ? Vec2{} : placed_.back().snap.p;
    const Vec2 d = sub(t, s);
    switch (kind_) {
    case ToolKind::Point:
        out.push_back({"x", "X", false, t.x});
        out.push_back({"y", "Y", false, t.y});
        break;
    case ToolKind::Line:
        if (placed_.size() == 1) {
            out.push_back({"length", "Length", false, length(d)});
            out.push_back({"angle", "Angle", true, angleOf(d)});
        }
        break;
    case ToolKind::Rectangle:
        if (placed_.size() == 1) {
            out.push_back({"width", "Width", false, std::abs(d.x)});
            out.push_back({"height", "Height", false, std::abs(d.y)});
        }
        break;
    case ToolKind::Circle:
        if (placed_.size() == 1)
            out.push_back({"diameter", "Diameter", false, 2 * length(d)});
        break;
    case ToolKind::ArcCenter:
        if (placed_.size() == 1)
            out.push_back({"radius", "Radius", false, length(d)});
        break;
    case ToolKind::Slot:
        if (placed_.size() == 1) {
            out.push_back({"length", "Length", false, length(d)});
            out.push_back({"angle", "Angle", true, angleOf(d)});
        } else if (placed_.size() == 2) {
            const auto w = typedValue(typed_, "width");
            const Vec2 a = placed_[0].snap.p, b = placed_[1].snap.p;
            const double dist = std::abs(cross(unit(sub(b, a)), sub(cursor_, a)));
            out.push_back({"width", "Width", false, w ? *w : 2 * dist});
        }
        break;
    case ToolKind::Fillet:
        out.push_back({"radius", "Radius", false, filletRadius_});
        break;
    case ToolKind::Offset:
        if (!offsetChain_.empty()) {
            const auto dd = typedValue(typed_, "distance");
            out.push_back({"distance", "Distance", false, dd ? *dd : offsetDistance_});
        }
        break;
    default:
        break;
    }
    // Typed values show as typed.
    for (FieldSpec& f : out) {
        if (const auto v = typedValue(typed_, f.key.c_str()))
            f.value = *v;
    }
    return out;
}

void ToolEngine::setTyped(const std::string& key, std::optional<double> value)
{
    if (value) {
        typed_[key] = *value;
        if (kind_ == ToolKind::Fillet && key == "radius" && *value > 0)
            filletRadius_ = *value;
    } else {
        typed_.erase(key);
    }
}

void ToolEngine::hover(const SketchView& view, Vec2 q, double tol, bool snapping)
{
    raw_ = q;
    tol_ = tol;
    tangentFrom_.reset();
    hovered_ = {};
    snap_ = {};
    snap_.p = q;
    switch (kind_) {
    case ToolKind::Select:
        break;
    case ToolKind::Trim:
    case ToolKind::Extend:
        hovered_ = view.hitCurve(q, tol);
        if (hovered_.kind != SketchView::Hit::Kind::None) {
            const ViewCurve* c = view.curve(hovered_.id);
            const bool ok = kind_ == ToolKind::Trim
                                ? (c->kind == ViewCurve::Kind::Line || c->kind == ViewCurve::Kind::Arc ||
                                   c->kind == ViewCurve::Kind::Circle)
                                : (c->kind == ViewCurve::Kind::Line || c->kind == ViewCurve::Kind::Arc);
            if (!ok)
                hovered_ = {};
        }
        break;
    case ToolKind::Fillet:
        hovered_ = view.hitPoint(q, tol);
        if (hovered_.kind != SketchView::Hit::Kind::None) {
            const auto at = view.curvesAt(hovered_.id);
            const bool corner =
                at.size() == 2 && at[0]->kind == ViewCurve::Kind::Line && at[1]->kind == ViewCurve::Kind::Line;
            if (!corner)
                hovered_ = {};
        }
        break;
    case ToolKind::Offset:
        if (offsetChain_.empty()) {
            hovered_ = view.hitCurve(q, tol);
            if (hovered_.kind != SketchView::Hit::Kind::None) {
                const ViewCurve* c = view.curve(hovered_.id);
                if (c->kind != ViewCurve::Kind::Line && c->kind != ViewCurve::Kind::Arc &&
                    c->kind != ViewCurve::Kind::Circle)
                    hovered_ = {};
            }
        }
        break;
    default: {
        SnapOptions options;
        options.enabled = snapping;
        if (!placed_.empty() && kind_ != ToolKind::Spline && !(kind_ == ToolKind::Slot && placed_.size() == 2) &&
            !(kind_ == ToolKind::Arc3 && placed_.size() == 2) && kind_ != ToolKind::ArcTangent)
            options.anchor = placed_.back().snap.p;
        if (kind_ == ToolKind::Circle || kind_ == ToolKind::ArcCenter)
            options.anchor.reset();
        snap_ = snapAt(view, q, tol, options);
        // A line leaving an arc or line end along its tangent.
        if (kind_ == ToolKind::Line && snapping && placed_.size() == 1 && snap_.kind != Snap::Kind::Point &&
            snap_.axis == Snap::Axis::None) {
            const Placed& start = placed_[0];
            const SketchEntId sid = start.committed ? start.id : start.snap.ref;
            if (start.committed || start.snap.kind == Snap::Kind::Point) {
                for (const ViewCurve* c : view.curvesAt(sid)) {
                    const auto t = tangentOut(*c, sid);
                    if (!t)
                        continue;
                    const Vec2 v = sub(q, start.snap.p);
                    const double along = dot(v, *t);
                    if (along > tol && std::abs(cross(*t, v)) <= tol && c->kind == ViewCurve::Kind::Arc) {
                        snap_ = {};
                        snap_.p = add(start.snap.p, mul(*t, along));
                        tangentFrom_ = c->id;
                        break;
                    }
                }
            }
        }
        break;
    }
    }
    // Centre-start-end arc: follow how far the pointer turned round the centre.
    if (kind_ == ToolKind::ArcCenter && placed_.size() == 2) {
        const double a = angleOf(sub(snap_.p, placed_[0].snap.p));
        double delta = a - lastAngle_;
        while (delta > kPi)
            delta -= 2 * kPi;
        while (delta < -kPi)
            delta += 2 * kPi;
        arcWinding_ += delta;
        lastAngle_ = a;
    }
    cursor_ = snap_.p;
    rebuildDraft(view);
}

void ToolEngine::rebuildDraft(const SketchView& view)
{
    Draft d;
    const Vec2 t = target();
    std::vector<Vec2> pts;
    for (const Placed& p : placed_)
        pts.push_back(p.snap.p);
    const bool typedAny = !typed_.empty();
    const auto snapGlyphs = [&] {
        if (!typedAny)
            glyphsFor(snap_, d.glyphs);
        d.guides.insert(d.guides.end(), snap_.guides.begin(), snap_.guides.end());
    };
    switch (kind_) {
    case ToolKind::Select:
        break;
    case ToolKind::Point:
        snapGlyphs();
        d.points.push_back(t);
        d.labelAt = t;
        break;
    case ToolKind::Line:
        snapGlyphs();
        if (!pts.empty()) {
            d.curves.push_back({pts.back(), t});
            const Vec2 mid = mul(add(pts.back(), t), 0.5);
            if (!typedAny && snap_.axis == Snap::Axis::Horizontal)
                d.glyphs.push_back({Glyph::Kind::Horizontal, mid});
            if (!typedAny && snap_.axis == Snap::Axis::Vertical)
                d.glyphs.push_back({Glyph::Kind::Vertical, mid});
            if (tangentFrom_)
                d.glyphs.push_back({Glyph::Kind::Tangent, pts.back()});
            d.points.push_back(pts.back());
        }
        d.points.push_back(t);
        d.labelAt = t;
        break;
    case ToolKind::Rectangle:
        snapGlyphs();
        if (!pts.empty()) {
            const Vec2 a = pts[0];
            d.curves.push_back({a, {t.x, a.y}, t, {a.x, t.y}, a});
            d.points.push_back(a);
        }
        d.points.push_back(t);
        d.labelAt = t;
        break;
    case ToolKind::Circle:
        snapGlyphs();
        if (!pts.empty()) {
            d.curves.push_back(arcPolyline(pts[0], distance(pts[0], t), 0, 2 * kPi));
            d.points.push_back(pts[0]);
            d.guides.emplace_back(pts[0], t);
        }
        d.points.push_back(t);
        d.labelAt = t;
        break;
    case ToolKind::Arc3:
        snapGlyphs();
        if (pts.size() == 1) {
            d.guides.emplace_back(pts[0], t);
        } else if (pts.size() == 2) {
            if (const auto c = circleThrough(pts[0], pts[1], t)) {
                const double as = angleOf(sub(pts[0], *c)), am = angleOf(sub(t, *c)), ae = angleOf(sub(pts[1], *c));
                const bool ccw = normAngle(am - as) < normAngle(ae - as);
                const double sweep = ccw ? normAngle(ae - as) : -normAngle(as - ae);
                d.curves.push_back(arcPolyline(*c, distance(pts[0], *c), as, sweep));
            } else {
                d.guides.emplace_back(pts[0], pts[1]);
            }
        }
        d.points.insert(d.points.end(), pts.begin(), pts.end());
        d.points.push_back(t);
        d.labelAt = t;
        break;
    case ToolKind::ArcCenter:
        snapGlyphs();
        if (pts.size() == 1) {
            d.guides.emplace_back(pts[0], t);
            // The circle the arc lies on, dashed.
            const auto circle = arcPolyline(pts[0], distance(pts[0], t), 0, 2 * kPi);
            for (std::size_t i = 0; i + 1 < circle.size(); i += 2)
                d.guides.emplace_back(circle[i], circle[i + 1]);
        } else if (pts.size() == 2) {
            const double r = distance(pts[0], pts[1]);
            const double as = angleOf(sub(pts[1], pts[0]));
            const double ae = angleOf(sub(t, pts[0]));
            const bool ccw = arcWinding_ >= 0;
            const double sweep = ccw ? normAngle(ae - as) : -normAngle(as - ae);
            d.curves.push_back(arcPolyline(pts[0], r, as, sweep));
            d.guides.emplace_back(pts[0], polar(pts[0], r, ae));
        }
        d.points.insert(d.points.end(), pts.begin(), pts.end());
        d.points.push_back(t);
        d.labelAt = t;
        break;
    case ToolKind::ArcTangent:
        snapGlyphs();
        if (pts.size() == 1) {
            const Placed& s = placed_[0];
            const SketchEntId sid = s.committed ? s.id : s.snap.ref;
            for (const ViewCurve* c : view.curvesAt(sid)) {
                const auto dir = tangentOut(*c, sid);
                if (!dir)
                    continue;
                if (const auto fit = tangentArc(pts[0], *dir, t)) {
                    const double as = angleOf(sub(pts[0], fit->center));
                    const double ae = angleOf(sub(t, fit->center));
                    const double sweep = fit->ccw ? normAngle(ae - as) : -normAngle(as - ae);
                    d.curves.push_back(arcPolyline(fit->center, fit->r, as, sweep));
                } else {
                    d.curves.push_back({pts[0], t});
                }
                d.glyphs.push_back({Glyph::Kind::Tangent, pts[0]});
                break;
            }
        }
        d.points.insert(d.points.end(), pts.begin(), pts.end());
        d.points.push_back(t);
        d.labelAt = t;
        break;
    case ToolKind::Slot:
        snapGlyphs();
        if (pts.size() == 1) {
            d.curves.push_back({pts[0], t});
        } else if (pts.size() == 2) {
            const auto w = typedValue(typed_, "width");
            const double half = w ? *w / 2 : std::abs(cross(unit(sub(pts[1], pts[0])), sub(cursor_, pts[0])));
            if (half > kTiny)
                d.curves.push_back(slotPolyline(pts[0], pts[1], half));
            d.guides.emplace_back(pts[0], pts[1]);
        }
        d.points.insert(d.points.end(), pts.begin(), pts.end());
        d.points.push_back(t);
        d.labelAt = t;
        break;
    case ToolKind::Spline: {
        snapGlyphs();
        std::vector<Vec2> cps = pts;
        cps.push_back(t);
        for (std::size_t i = 0; i + 1 < cps.size(); ++i)
            d.guides.emplace_back(cps[i], cps[i + 1]);
        if (cps.size() >= 2)
            d.curves.push_back(splinePolyline(cps, 3, false));
        d.points = cps;
        d.labelAt = t;
        break;
    }
    case ToolKind::Trim:
    case ToolKind::Extend:
        if (hovered_.kind != SketchView::Hit::Kind::None)
            d.highlight.push_back(hovered_.id);
        break;
    case ToolKind::Fillet:
        if (hovered_.kind != SketchView::Hit::Kind::None) {
            d.highlight.push_back(hovered_.id);
            d.points.push_back(hovered_.at);
            d.labelAt = hovered_.at;
        }
        break;
    case ToolKind::Offset:
        if (offsetChain_.empty()) {
            if (hovered_.kind != SketchView::Hit::Kind::None)
                d.highlight = connectedChain(view, hovered_.id);
        } else {
            d.highlight = offsetChain_;
            std::optional<Vec2> foot;
            for (const SketchEntId& id : offsetChain_) {
                if (const ViewCurve* c = view.curve(id)) {
                    const Vec2 n = nearestOn(*c, raw_);
                    if (!foot || distance(n, raw_) < distance(*foot, raw_))
                        foot = n;
                }
            }
            if (foot) {
                offsetDistance_ = distance(*foot, raw_);
                d.guides.emplace_back(*foot, raw_);
                d.labelAt = raw_;
            }
        }
        break;
    }
    draft_ = std::move(d);
}

Action ToolEngine::commitLine(const SketchView& view)
{
    if (placed_.empty())
        return {};
    const Placed start = placed_[0];
    const Vec2 t = target();
    if (distance(start.snap.p, t) < kTiny)
        return {};
    Placed end;
    if (typed_.empty()) {
        end.snap = snap_;
    } else {
        end.snap.p = t;
        const auto ang = typedValue(typed_, "angle");
        if (ang) {
            const double deg = std::fmod(std::abs(*ang) * 180 / kPi, 90.0);
            if (deg < 1e-9 || 90 - deg < 1e-9)
                end.snap.axis = std::abs(std::sin(*ang)) < 0.5 ? Snap::Axis::Horizontal : Snap::Axis::Vertical;
        }
    }
    end.id = SketchEntId{Uuid::generate()};
    Action a;
    a.kind = Action::Kind::Add;
    a.label = "Line";
    Batch& b = a.batch;
    const SketchEntId s = pointFor(start, b);
    const SketchEntId e = pointFor(end, b);
    if (s == e)
        return {};
    const SketchEntId line = addCurve(b, sketch::Entity{{}, sketch::Line{s, e}, false});
    if (end.snap.axis == Snap::Axis::Horizontal)
        addConstraint(b, ConstraintType::Horizontal, {line});
    if (end.snap.axis == Snap::Axis::Vertical)
        addConstraint(b, ConstraintType::Vertical, {line});
    if (tangentFrom_ && typed_.empty())
        addConstraint(b, ConstraintType::Tangent, {*tangentFrom_, line, s});
    if (const auto len = typedValue(typed_, "length"); len && *len > 0)
        addConstraint(b, ConstraintType::Distance, {line}, *len);
    // The chain goes on from the end, unless it came back to where it started.
    const bool closed = chainStart_ && end.snap.kind == Snap::Kind::Point && end.snap.ref == *chainStart_;
    if (closed) {
        placed_.clear();
        chainStart_.reset();
        lastCurve_.reset();
    } else {
        Placed next;
        next.snap.kind = Snap::Kind::Point;
        next.snap.p = end.snap.p;
        next.snap.ref = e;
        next.id = e;
        next.committed = true;
        placed_ = {next};
        if (!chainStart_)
            chainStart_ = s;
        lastCurve_ = line;
    }
    resetTyped();
    (void)view;
    return a;
}

Action ToolEngine::click(const SketchView& view)
{
    if (kind_ == ToolKind::Select)
        return {};
    // Commands on existing geometry.
    if (kind_ == ToolKind::Trim || kind_ == ToolKind::Extend) {
        if (hovered_.kind == SketchView::Hit::Kind::None)
            return {};
        Action a;
        a.kind = Action::Kind::Command;
        a.command = kind_ == ToolKind::Trim ? "sketch.trim" : "sketch.extend";
        a.label = kind_ == ToolKind::Trim ? "Trim" : "Extend";
        a.argsJson = "{\"entity\":\"" + hovered_.id.value.toString() + "\",\"at\":" + jsonPoint(raw_) + "}";
        return a;
    }
    if (kind_ == ToolKind::Fillet) {
        if (hovered_.kind == SketchView::Hit::Kind::None)
            return {};
        Action a;
        a.kind = Action::Kind::Command;
        a.command = "sketch.fillet";
        a.label = "Fillet";
        const double r = typedValue(typed_, "radius").value_or(filletRadius_);
        a.argsJson = "{\"point\":\"" + hovered_.id.value.toString() + "\",\"radius\":" + jsonNumber(r) + "}";
        resetTyped();
        return a;
    }
    if (kind_ == ToolKind::Offset) {
        if (offsetChain_.empty()) {
            if (hovered_.kind != SketchView::Hit::Kind::None)
                offsetChain_ = connectedChain(view, hovered_.id);
            rebuildDraft(view);
            return {};
        }
        double dist = 0;
        if (const auto typedD = typedValue(typed_, "distance")) {
            dist = *typedD;
        } else {
            bool any = false;
            for (const SketchEntId& id : offsetChain_) {
                if (const ViewCurve* c = view.curve(id)) {
                    const double d = distance(nearestOn(*c, raw_), raw_);
                    if (!any || d < dist)
                        dist = d;
                    any = true;
                }
            }
        }
        if (!(dist > kTiny))
            return {};
        Action a;
        a.kind = Action::Kind::Command;
        a.command = "sketch.offset";
        a.label = "Offset";
        std::string ids;
        for (const SketchEntId& id : offsetChain_)
            ids += (ids.empty() ? "\"" : ",\"") + id.value.toString() + "\"";
        a.argsJson =
            "{\"entities\":[" + ids + "],\"distance\":" + jsonNumber(dist) + ",\"side\":" + jsonPoint(raw_) + "}";
        offsetChain_.clear();
        resetTyped();
        return a;
    }
    if (kind_ == ToolKind::Point) {
        // One point per click, with the constraints its snap infers; typed coordinates place it exactly.
        Placed p;
        p.snap = snap_;
        if (!typed_.empty()) {
            p.snap = {};
            p.snap.p = target();
        }
        if (p.snap.kind == Snap::Kind::Point)
            return {}; // a point is there already
        p.id = SketchEntId{Uuid::generate()};
        Action a;
        a.kind = Action::Kind::Add;
        a.label = "Point";
        pointFor(p, a.batch);
        for (sketch::Entity& e : a.batch.entities)
            e.construction = construction_;
        resetTyped();
        rebuildDraft(view);
        return a;
    }
    if (kind_ == ToolKind::Line) {
        if (placed_.empty()) {
            Placed p;
            p.snap = snap_;
            p.id = snap_.kind == Snap::Kind::Point ? snap_.ref : SketchEntId{Uuid::generate()};
            p.committed = false;
            placed_.push_back(p);
            chainStart_ = p.id;
            rebuildDraft(view);
            return {};
        }
        return commitLine(view);
    }
    if (kind_ == ToolKind::ArcTangent) {
        if (placed_.empty()) {
            // Must start at the end of a line or arc.
            if (snap_.kind != Snap::Kind::Point)
                return {};
            bool ok = false;
            for (const ViewCurve* c : view.curvesAt(snap_.ref))
                ok = ok || tangentOut(*c, snap_.ref).has_value();
            if (!ok)
                return {};
            Placed p;
            p.snap = snap_;
            p.id = snap_.ref;
            p.committed = true;
            placed_.push_back(p);
            rebuildDraft(view);
            return {};
        }
        return commitStep(view);
    }
    if (kind_ == ToolKind::Spline) {
        if (placed_.size() >= 2 && distance(cursor_, placed_.back().snap.p) <= tol_)
            return enter(view);
        Placed p;
        p.snap = snap_;
        p.id = snap_.kind == Snap::Kind::Point ? snap_.ref : SketchEntId{Uuid::generate()};
        placed_.push_back(p);
        rebuildDraft(view);
        return {};
    }
    // Rectangle, circle, arcs, slot: points until the last one commits.
    const std::size_t needed =
        kind_ == ToolKind::Arc3 || kind_ == ToolKind::ArcCenter || kind_ == ToolKind::Slot ? 3 : 2;
    if (placed_.size() + 1 < needed) {
        Placed p;
        p.snap = snap_;
        if (!typed_.empty()) {
            p.snap = {};
            p.snap.p = target();
        }
        p.id = p.snap.kind == Snap::Kind::Point ? p.snap.ref : SketchEntId{Uuid::generate()};
        if (kind_ == ToolKind::ArcCenter && placed_.size() == 1) {
            lastAngle_ = angleOf(sub(p.snap.p, placed_[0].snap.p));
            arcWinding_ = 0;
        }
        placed_.push_back(p);
        resetTyped();
        rebuildDraft(view);
        return {};
    }
    return commitStep(view);
}

Action ToolEngine::commitStep(const SketchView& view)
{
    Action a;
    a.kind = Action::Kind::Add;
    Batch& b = a.batch;
    const Vec2 t = target();
    Placed last;
    last.snap = snap_;
    if (!typed_.empty()) {
        last.snap = {};
        last.snap.p = t;
    }
    last.id = last.snap.kind == Snap::Kind::Point ? last.snap.ref : SketchEntId{Uuid::generate()};
    switch (kind_) {
    case ToolKind::Rectangle: {
        if (placed_.size() != 1)
            return {};
        const Vec2 p1 = placed_[0].snap.p, p3 = t;
        if (std::abs(p3.x - p1.x) < kTiny || std::abs(p3.y - p1.y) < kTiny)
            return {};
        a.label = "Rectangle";
        const SketchEntId i1 = pointFor(placed_[0], b);
        const SketchEntId i3 = pointFor(last, b);
        const SketchEntId i2{Uuid::generate()}, i4{Uuid::generate()};
        b.entities.push_back(sketch::Entity{i2, sketch::Point{{p3.x, p1.y}}, false});
        b.entities.push_back(sketch::Entity{i4, sketch::Point{{p1.x, p3.y}}, false});
        const SketchEntId bottom = addCurve(b, sketch::Entity{{}, sketch::Line{i1, i2}, false});
        const SketchEntId right = addCurve(b, sketch::Entity{{}, sketch::Line{i2, i3}, false});
        const SketchEntId top = addCurve(b, sketch::Entity{{}, sketch::Line{i3, i4}, false});
        const SketchEntId left = addCurve(b, sketch::Entity{{}, sketch::Line{i4, i1}, false});
        addConstraint(b, ConstraintType::Horizontal, {bottom});
        addConstraint(b, ConstraintType::Horizontal, {top});
        addConstraint(b, ConstraintType::Vertical, {left});
        addConstraint(b, ConstraintType::Vertical, {right});
        if (const auto w = typedValue(typed_, "width"); w && *w > 0)
            addConstraint(b, ConstraintType::Distance, {bottom}, *w);
        if (const auto h = typedValue(typed_, "height"); h && *h > 0)
            addConstraint(b, ConstraintType::Distance, {left}, *h);
        break;
    }
    case ToolKind::Circle: {
        if (placed_.size() != 1)
            return {};
        const double r = distance(placed_[0].snap.p, t);
        if (r < kTiny)
            return {};
        a.label = "Circle";
        const SketchEntId c = pointFor(placed_[0], b);
        const SketchEntId circle = addCurve(b, sketch::Entity{{}, sketch::Circle{c, r}, false});
        if (const auto dia = typedValue(typed_, "diameter"); dia && *dia > 0)
            addConstraint(b, ConstraintType::Diameter, {circle}, *dia);
        break;
    }
    case ToolKind::Arc3: {
        if (placed_.size() != 2)
            return {};
        const Vec2 s = placed_[0].snap.p, e = placed_[1].snap.p;
        const auto c = circleThrough(s, e, t);
        if (!c)
            return {};
        a.label = "Arc";
        const double as = angleOf(sub(s, *c)), am = angleOf(sub(t, *c)), ae = angleOf(sub(e, *c));
        const bool ccw = normAngle(am - as) < normAngle(ae - as);
        const SketchEntId is = pointFor(placed_[0], b);
        const SketchEntId ie = pointFor(placed_[1], b);
        const SketchEntId ic{Uuid::generate()};
        b.entities.push_back(sketch::Entity{ic, sketch::Point{*c}, false});
        addCurve(b, sketch::Entity{{}, sketch::Arc{ic, is, ie, ccw}, false});
        break;
    }
    case ToolKind::ArcCenter: {
        if (placed_.size() != 2)
            return {};
        const Vec2 c = placed_[0].snap.p, s = placed_[1].snap.p;
        const double r = distance(c, s);
        if (r < kTiny || distance(c, t) < kTiny)
            return {};
        a.label = "Arc";
        const Vec2 e = add(c, mul(unit(sub(t, c)), r));
        if (distance(e, s) < kTiny)
            return {};
        const SketchEntId ic = pointFor(placed_[0], b);
        const SketchEntId is = pointFor(placed_[1], b);
        const SketchEntId ie{Uuid::generate()};
        b.entities.push_back(sketch::Entity{ie, sketch::Point{e}, false});
        addCurve(b, sketch::Entity{{}, sketch::Arc{ic, is, ie, arcWinding_ >= 0}, false});
        if (const auto rr = typedValue(typed_, "radius"); rr && *rr > 0) {
            // The arc entity is the last one added.
            addConstraint(b, ConstraintType::Radius, {b.entities.back().id}, *rr);
        }
        break;
    }
    case ToolKind::ArcTangent: {
        if (placed_.size() != 1)
            return {};
        const Placed& start = placed_[0];
        const SketchEntId sid = start.id;
        std::optional<SketchEntId> from;
        std::optional<Vec2> dir;
        const std::vector<const ViewCurve*> at = view.curvesAt(sid);
        for (const ViewCurve* c : at) {
            if (lastCurve_ && c->id == *lastCurve_) {
                from = c->id;
                dir = tangentOut(*c, sid);
            }
        }
        for (const ViewCurve* c : at) {
            if (!dir) {
                if (const auto d = tangentOut(*c, sid)) {
                    from = c->id;
                    dir = d;
                }
            }
        }
        if (!dir)
            return {};
        const auto fit = tangentArc(start.snap.p, *dir, t);
        if (!fit || distance(start.snap.p, t) < kTiny)
            return {};
        a.label = "Tangent arc";
        const SketchEntId ie = pointFor(last, b);
        if (ie == sid)
            return {};
        const SketchEntId ic{Uuid::generate()};
        b.entities.push_back(sketch::Entity{ic, sketch::Point{fit->center}, false});
        const SketchEntId arc = addCurve(b, sketch::Entity{{}, sketch::Arc{ic, sid, ie, fit->ccw}, false});
        addConstraint(b, ConstraintType::Tangent, {*from, arc, sid});
        Placed next;
        next.snap.kind = Snap::Kind::Point;
        next.snap.p = last.snap.p;
        next.snap.ref = ie;
        next.id = ie;
        next.committed = true;
        placed_ = {next};
        lastCurve_ = arc;
        resetTyped();
        return a;
    }
    case ToolKind::Slot: {
        if (placed_.size() != 2)
            return {};
        const Vec2 pa = placed_[0].snap.p, pb = placed_[1].snap.p;
        const auto w = typedValue(typed_, "width");
        const double width = w ? *w : 2 * std::abs(cross(unit(sub(pb, pa)), sub(cursor_, pa)));
        if (!(width > kTiny) || distance(pa, pb) < kTiny)
            return {};
        a.label = "Slot";
        const SketchEntId ia = pointFor(placed_[0], b);
        const SketchEntId ib = pointFor(placed_[1], b);
        addCurve(b, sketch::Entity{{}, sketch::Slot{ia, ib, width}, false});
        if (placed_[1].snap.axis == Snap::Axis::Horizontal)
            addConstraint(b, ConstraintType::Horizontal, {ia, ib});
        if (placed_[1].snap.axis == Snap::Axis::Vertical)
            addConstraint(b, ConstraintType::Vertical, {ia, ib});
        break;
    }
    default:
        return {};
    }
    placed_.clear();
    resetTyped();
    return a;
}

Action ToolEngine::enter(const SketchView& view)
{
    if (kind_ == ToolKind::Spline) {
        if (placed_.size() < 2)
            return {};
        Action a;
        a.kind = Action::Kind::Add;
        a.label = "Spline";
        std::vector<SketchEntId> ids;
        for (const Placed& p : placed_) {
            const SketchEntId id = pointFor(p, a.batch);
            if (ids.empty() || ids.back() != id)
                ids.push_back(id);
        }
        bool closed = false;
        if (ids.size() >= 4 && ids.front() == ids.back()) {
            ids.pop_back();
            closed = true;
        }
        if (ids.size() < 2)
            return {};
        // New control points of a spline are not constraint-free in the solver: keep them plain points.
        const int degree = std::min(3, static_cast<int>(ids.size()) - 1);
        addCurve(a.batch, sketch::Entity{{}, sketch::Spline{ids, degree, closed}, false});
        placed_.clear();
        resetTyped();
        return a;
    }
    if (kind_ == ToolKind::Line)
        return commitLine(view);
    return click(view);
}

} // namespace ray::ui::sketchui
