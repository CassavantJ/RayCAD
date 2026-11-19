// The sketch editing commands (sketch_commands.h).
#include "sketch_commands.h"

#include "feature_props.h"
#include "sketch_feature.h"
#include "sketch_json.h"

#include "../commands/args.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace ray::doc::features {
namespace {

using commands::Args;
using json::Member;
using json::Value;
using namespace ray::sketch;

constexpr double kTwoPi = 6.283185307179586476925286766559;

#define RAY_TRY(var, expr)                                                                                             \
    auto var = (expr);                                                                                                 \
    if (!var) {                                                                                                        \
        return tl::unexpected(var.error());                                                                            \
    }

// ---------- The sketch feature being edited ----------

struct Editing {
    std::shared_ptr<DocState> next;
    std::size_t index = 0;
    Sketch model;
    FeatureId feature;
};

Result<Editing> open(const Args& args, const DocState& before)
{
    RAY_TRY(id, args.id("id"));
    const auto it = std::find_if(before.features.begin(), before.features.end(),
                                 [&](const Feature& f) { return f.id == FeatureId{*id}; });
    if (it == before.features.end()) {
        return args.error("id", "names no feature of this document (" + id->toString() + ")");
    }
    if (it->type != kSketchType) {
        return args.error("id", "names a " + it->type + " feature, not a sketch");
    }
    RAY_TRY(model, sketchOfFeature(*it, false));
    Editing out;
    out.next = std::make_shared<DocState>(before);
    out.index = static_cast<std::size_t>(it - before.features.begin());
    out.model = std::move(*model);
    out.feature = FeatureId{*id};
    return out;
}

// Writes `model` into the feature and checks it as the sketch type does for its own commands.
Result<DocStatePtr> commit(Editing& e, const Sketch& model, const std::string& command)
{
    Feature& feature = e.next->features[e.index];
    RAY_TRY(props, withSketch(feature, model));
    feature.propsJson = std::move(*props);
    const auto registry = builtinFeatureRegistry();
    const auto* type = registry ? dynamic_cast<const ParametricType*>(registry->find(kSketchType)) : nullptr;
    if (type == nullptr) {
        return invalid(command + ": the sketch feature type is not registered");
    }
    if (auto ok = type->validate(*e.next, e.index, feature); !ok) {
        return invalid(command + ": " + ok.error().message);
    }
    return DocStatePtr(std::move(e.next));
}

// The model with extra entities / constraints read through the sketch JSON reader, so they are checked like a stored
// sketch (unique ids, references that resolve).
Result<Sketch> merged(const Sketch& model, const Value* entities, const Value* constraints, const std::string& command)
{
    Value current = sketchToJson(model);
    Value::Array allEntities = current.find("entities")->asArray();
    Value::Array allConstraints = current.find("constraints")->asArray();
    if (entities != nullptr) {
        allEntities.insert(allEntities.end(), entities->asArray().begin(), entities->asArray().end());
    }
    if (constraints != nullptr) {
        allConstraints.insert(allConstraints.end(), constraints->asArray().begin(), constraints->asArray().end());
    }
    const Value combined = Value::object({{"schema", Value::number(1)},
                                          {"entities", Value::array(std::move(allEntities))},
                                          {"constraints", Value::array(std::move(allConstraints))}});
    const PropsReader r(command, combined);
    return sketchFromJson(r, combined, "sketch");
}

Result<std::vector<Uuid>> idList(const Args& args, std::string_view key)
{
    RAY_TRY(value, args.optValue(key));
    if (*value == nullptr || !(*value)->isArray() || (*value)->asArray().empty()) {
        return args.error(key, "must list at least one id");
    }
    std::vector<Uuid> out;
    for (const Value& v : (*value)->asArray()) {
        const Uuid id = v.isString() ? Uuid::fromString(v.asString()) : Uuid{};
        if (!v.isString() || id.toString() != v.asString() || id == Uuid{}) {
            return args.error(key, "must list lower-case UUIDs");
        }
        out.push_back(id);
    }
    return out;
}

Result<const Value*> arrayArg(const Args& args, std::string_view key)
{
    RAY_TRY(value, args.optValue(key));
    if (*value == nullptr || !(*value)->isArray() || (*value)->asArray().empty()) {
        return args.error(key, "must be a non-empty array");
    }
    return *value;
}

Result<Vec2> pointArg(const Args& args, std::string_view key)
{
    RAY_TRY(value, args.optValue(key));
    const Value* v = *value;
    if (v == nullptr || !v->isObject() || v->find("x") == nullptr || v->find("y") == nullptr ||
        !v->find("x")->isNumber() || !v->find("y")->isNumber() || v->asObject().size() != 2 ||
        !std::isfinite(v->find("x")->asNumber()) || !std::isfinite(v->find("y")->asNumber())) {
        return args.error(key, "must be a point {\"x\": <mm>, \"y\": <mm>} in the sketch plane");
    }
    return Vec2{v->find("x")->asNumber(), v->find("y")->asNumber()};
}

// ---------- Model helpers ----------

Entity* findEntity(Sketch& s, const SketchEntId& id)
{
    for (Entity& e : s.entities) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

std::optional<Vec2> pointOf(const Sketch& s, const SketchEntId& id)
{
    for (const Entity& e : s.entities) {
        if (e.id == id) {
            if (const auto* p = std::get_if<Point>(&e.geom)) {
                return p->p;
            }
        }
    }
    return std::nullopt;
}

std::vector<SketchEntId> pointsUsed(const Entity& e)
{
    if (const auto* l = std::get_if<Line>(&e.geom)) {
        return {l->a, l->b};
    }
    if (const auto* a = std::get_if<Arc>(&e.geom)) {
        return {a->center, a->start, a->end};
    }
    if (const auto* c = std::get_if<Circle>(&e.geom)) {
        return {c->center};
    }
    if (const auto* sp = std::get_if<Spline>(&e.geom)) {
        return sp->controlPoints;
    }
    if (const auto* sl = std::get_if<Slot>(&e.geom)) {
        return {sl->a, sl->b};
    }
    return {};
}

// Removes `ids`, the curves using a removed point, end points that only removed curves used, and every constraint
// on something removed.
void removeEntities(Sketch& s, std::set<SketchEntId> removed)
{
    bool grew = true;
    while (grew) {
        grew = false;
        for (const Entity& e : s.entities) {
            if (removed.count(e.id) != 0) {
                continue;
            }
            for (const SketchEntId& p : pointsUsed(e)) {
                if (removed.count(p) != 0) {
                    removed.insert(e.id);
                    grew = true;
                    break;
                }
            }
        }
    }
    std::set<SketchEntId> candidates;
    for (const Entity& e : s.entities) {
        if (removed.count(e.id) != 0) {
            for (const SketchEntId& p : pointsUsed(e)) {
                candidates.insert(p);
            }
        }
    }
    for (const Entity& e : s.entities) {
        if (removed.count(e.id) == 0) {
            for (const SketchEntId& p : pointsUsed(e)) {
                candidates.erase(p);
            }
        }
    }
    removed.insert(candidates.begin(), candidates.end());
    s.entities.erase(
        std::remove_if(s.entities.begin(), s.entities.end(), [&](const Entity& e) { return removed.count(e.id) != 0; }),
        s.entities.end());
    s.constraints.erase(std::remove_if(s.constraints.begin(), s.constraints.end(),
                                       [&](const Constraint& c) {
                                           return std::any_of(c.refs.begin(), c.refs.end(), [&](const SketchEntId& r) {
                                               return removed.count(r) != 0;
                                           });
                                       }),
                        s.constraints.end());
}

// A fresh id, derived from the feature, the entity, the purpose and the sketch's size, not used in `s`.
Uuid derivedId(const Sketch& s, const FeatureId& feature, const SketchEntId& entity, std::string_view purpose)
{
    for (std::int64_t salt = static_cast<std::int64_t>(s.entities.size());; ++salt) {
        Hasher h;
        h.str("ray.doc.sketch.derived-id/1");
        h.bytes(std::as_bytes(std::span<const std::uint8_t>(feature.value.bytes)));
        h.bytes(std::as_bytes(std::span<const std::uint8_t>(entity.value.bytes)));
        h.str(purpose);
        h.i64(salt);
        const Hash256 d = h.finish();
        Uuid id;
        std::copy_n(d.bytes.begin(), id.bytes.size(), id.bytes.begin());
        id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0F) | 0x40);
        id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3F) | 0x80);
        const bool used =
            std::any_of(s.entities.begin(), s.entities.end(), [&](const Entity& e) { return e.id.value == id; }) ||
            std::any_of(s.constraints.begin(), s.constraints.end(), [&](const Constraint& c) { return c.id == id; });
        if (!used) {
            return id;
        }
    }
}

Result<SketchEntId> newIdArg(const Args& args, std::string_view key, const Sketch& s, const FeatureId& feature,
                             const SketchEntId& entity, std::string_view purpose)
{
    RAY_TRY(value, args.optValue(key));
    if (*value == nullptr) {
        return SketchEntId{derivedId(s, feature, entity, purpose)};
    }
    RAY_TRY(id, args.id(key));
    const bool used =
        std::any_of(s.entities.begin(), s.entities.end(), [&](const Entity& e) { return e.id.value == *id; });
    if (used) {
        return args.error(key, "is already used by an entity of the sketch");
    }
    return SketchEntId{*id};
}

// ---------- Curve geometry ----------

double cross2(Vec2 a, Vec2 b)
{
    return a.x * b.y - a.y * b.x;
}
Vec2 sub2(Vec2 a, Vec2 b)
{
    return {a.x - b.x, a.y - b.y};
}
double dot2(Vec2 a, Vec2 b)
{
    return a.x * b.x + a.y * b.y;
}
double len2(Vec2 a)
{
    return std::sqrt(dot2(a, a));
}
double normAngle(double a) // to [0, 2pi)
{
    while (a < 0) {
        a += kTwoPi;
    }
    while (a >= kTwoPi) {
        a -= kTwoPi;
    }
    return a;
}

// A line, arc or circle of the sketch with its geometry.
struct Curve {
    enum class K { Line, Arc, Circle } k;
    Vec2 a, b;        // line ends; arc start and end
    Vec2 c;           // arc / circle centre
    double r = 0;     // arc / circle radius
    bool ccw = true;  // arc
    double start = 0; // arc: angle of a
    double sweep = 0; // arc: angle travelled from a to b (> 0), in the arc's direction
    double length() const { return k == K::Line ? len2(sub2(b, a)) : r * sweep; }
};

std::optional<Curve> curveOf(const Sketch& s, const Entity& e)
{
    Curve out{};
    if (const auto* l = std::get_if<Line>(&e.geom)) {
        auto a = pointOf(s, l->a);
        auto b = pointOf(s, l->b);
        if (!a || !b) {
            return std::nullopt;
        }
        out.k = Curve::K::Line;
        out.a = *a;
        out.b = *b;
        return out;
    }
    if (const auto* arc = std::get_if<Arc>(&e.geom)) {
        auto c = pointOf(s, arc->center);
        auto a = pointOf(s, arc->start);
        auto b = pointOf(s, arc->end);
        if (!c || !a || !b) {
            return std::nullopt;
        }
        out.k = Curve::K::Arc;
        out.c = *c;
        out.a = *a;
        out.b = *b;
        out.r = len2(sub2(*a, *c));
        out.ccw = arc->ccw;
        out.start = normAngle(dmath::atan2(a->y - c->y, a->x - c->x));
        const double end = normAngle(dmath::atan2(b->y - c->y, b->x - c->x));
        out.sweep = normAngle(arc->ccw ? end - out.start : out.start - end);
        if (out.sweep <= 0) {
            out.sweep = kTwoPi;
        }
        return out;
    }
    if (const auto* circle = std::get_if<Circle>(&e.geom)) {
        auto c = pointOf(s, circle->center);
        if (!c) {
            return std::nullopt;
        }
        out.k = Curve::K::Circle;
        out.c = *c;
        out.r = circle->radius;
        return out;
    }
    return std::nullopt;
}

// The parameter of p on a curve: a line from 0 to its length; an arc by arc length from its start (in its
// direction, values beyond the arc wrap); a circle by angle times radius from +x, counter-clockwise.
double paramOf(const Curve& cv, Vec2 p)
{
    if (cv.k == Curve::K::Line) {
        const Vec2 d = sub2(cv.b, cv.a);
        const double l = len2(d);
        return l > 0 ? dot2(sub2(p, cv.a), d) / l : 0.0;
    }
    const double angle = normAngle(dmath::atan2(p.y - cv.c.y, p.x - cv.c.x));
    if (cv.k == Curve::K::Circle) {
        return angle * cv.r;
    }
    const double along = normAngle(cv.ccw ? angle - cv.start : cv.start - angle);
    return along * cv.r;
}

Vec2 pointAt(const Curve& cv, double t)
{
    if (cv.k == Curve::K::Line) {
        const Vec2 d = sub2(cv.b, cv.a);
        const double l = len2(d);
        return l > 0 ? Vec2{cv.a.x + d.x * t / l, cv.a.y + d.y * t / l} : cv.a;
    }
    double angle = cv.k == Curve::K::Circle ? t / cv.r : (cv.ccw ? cv.start + t / cv.r : cv.start - t / cv.r);
    return Vec2{cv.c.x + cv.r * dmath::cos(angle), cv.c.y + cv.r * dmath::sin(angle)};
}

// True if p (on the curve's carrier) lies on the curve itself.
bool onCurve(const Curve& cv, Vec2 p, double tol)
{
    if (cv.k == Curve::K::Circle) {
        return true;
    }
    const double t = paramOf(cv, p);
    if (cv.k == Curve::K::Line) {
        return t >= -tol && t <= cv.length() + tol;
    }
    return t <= cv.length() + tol || t >= kTwoPi * cv.r - tol;
}

// Points where the carriers of two curves meet (line: infinite line; arc: its circle).
std::vector<Vec2> carrierCrossings(const Curve& p, const Curve& q)
{
    std::vector<Vec2> out;
    const bool pl = p.k == Curve::K::Line, ql = q.k == Curve::K::Line;
    if (pl && ql) {
        const Vec2 d1 = sub2(p.b, p.a), d2 = sub2(q.b, q.a);
        const double den = cross2(d1, d2);
        if (std::abs(den) < 1e-14 * len2(d1) * len2(d2)) {
            return out;
        }
        const double t = cross2(sub2(q.a, p.a), d2) / den;
        out.push_back(Vec2{p.a.x + d1.x * t, p.a.y + d1.y * t});
        return out;
    }
    if (pl || ql) {
        const Curve& line = pl ? p : q;
        const Curve& circle = pl ? q : p;
        const Vec2 d = sub2(line.b, line.a);
        const double l = len2(d);
        if (!(l > 0)) {
            return out;
        }
        const Vec2 u{d.x / l, d.y / l};
        const Vec2 f = sub2(line.a, circle.c);
        const double b = dot2(f, u);
        const double c = dot2(f, f) - circle.r * circle.r;
        const double disc = b * b - c;
        if (disc < -1e-12 * circle.r * circle.r) {
            return out;
        }
        const double root = disc > 0 ? std::sqrt(disc) : 0.0;
        for (const double t : {-b - root, -b + root}) {
            out.push_back(Vec2{line.a.x + u.x * t, line.a.y + u.y * t});
            if (root == 0.0) {
                break;
            }
        }
        return out;
    }
    const Vec2 d = sub2(q.c, p.c);
    const double dist = len2(d);
    if (!(dist > 1e-12) || dist > p.r + q.r + 1e-9 || dist < std::abs(p.r - q.r) - 1e-9) {
        return out;
    }
    const double a = (p.r * p.r - q.r * q.r + dist * dist) / (2 * dist);
    const double h2 = p.r * p.r - a * a;
    const double h = h2 > 0 ? std::sqrt(h2) : 0.0;
    const Vec2 m{p.c.x + d.x * a / dist, p.c.y + d.y * a / dist};
    out.push_back(Vec2{m.x - d.y * h / dist, m.y + d.x * h / dist});
    if (h > 0) {
        out.push_back(Vec2{m.x + d.y * h / dist, m.y - d.x * h / dist});
    }
    return out;
}

// ---------- Commands ----------

constexpr std::string_view kSet = "sketch.set";
constexpr std::string_view kEntityAdd = "sketch.entity.add";
constexpr std::string_view kEntityRemove = "sketch.entity.remove";
constexpr std::string_view kConstraintAdd = "sketch.constraint.add";
constexpr std::string_view kConstraintRemove = "sketch.constraint.remove";
constexpr std::string_view kConstraintSet = "sketch.constraint.set";
constexpr std::string_view kSplit = "sketch.split";
constexpr std::string_view kTrim = "sketch.trim";

Result<DocStatePtr> sketchSet(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kSet, argsJson, {"id", "plane", "sketch"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(sketchValue, args->optValue("sketch"));
    if (*sketchValue == nullptr) {
        return args->error("sketch", "is required");
    }
    const PropsReader r(std::string(kSet), Value::object({}));
    RAY_TRY(model, sketchFromJson(r, **sketchValue, "sketch"));
    RAY_TRY(plane, args->optValue("plane"));
    if (*plane != nullptr) {
        Feature& feature = e->next->features[e->index];
        RAY_TRY(props, PropsReader::parse(std::string(kSet), feature.propsJson));
        Value::Object members;
        for (const Member& m : props->asObject()) {
            if (m.key != "plane") {
                members.push_back(m);
            }
        }
        members.push_back({"plane", **plane});
        feature.propsJson = json::write(Value::object(std::move(members)));
    }
    return commit(*e, *model, std::string(kSet));
}

Result<DocStatePtr> entityAdd(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kEntityAdd, argsJson, {"entities", "id"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(entities, arrayArg(*args, "entities"));
    RAY_TRY(model, merged(e->model, *entities, nullptr, std::string(kEntityAdd)));
    return commit(*e, *model, std::string(kEntityAdd));
}

Result<DocStatePtr> entityRemove(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kEntityRemove, argsJson, {"entities", "id"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(ids, idList(*args, "entities"));
    std::set<SketchEntId> removed;
    for (const Uuid& id : *ids) {
        if (findEntity(e->model, SketchEntId{id}) == nullptr) {
            return args->error("entities", "names " + id.toString() + ", which is not an entity of the sketch");
        }
        removed.insert(SketchEntId{id});
    }
    Sketch model = e->model;
    removeEntities(model, removed);
    return commit(*e, model, std::string(kEntityRemove));
}

Result<DocStatePtr> constraintAdd(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kConstraintAdd, argsJson, {"constraints", "id"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(constraints, arrayArg(*args, "constraints"));
    RAY_TRY(model, merged(e->model, nullptr, *constraints, std::string(kConstraintAdd)));
    return commit(*e, *model, std::string(kConstraintAdd));
}

Result<DocStatePtr> constraintRemove(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kConstraintRemove, argsJson, {"constraints", "id"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(ids, idList(*args, "constraints"));
    Sketch model = e->model;
    for (const Uuid& id : *ids) {
        const auto it = std::find_if(model.constraints.begin(), model.constraints.end(),
                                     [&](const Constraint& c) { return c.id == id; });
        if (it == model.constraints.end()) {
            return args->error("constraints", "names " + id.toString() + ", which is not a constraint of the sketch");
        }
        model.constraints.erase(it);
    }
    return commit(*e, model, std::string(kConstraintRemove));
}

Result<DocStatePtr> constraintSet(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kConstraintSet, argsJson, {"constraint", "driven", "expression", "id", "value"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(id, args->id("constraint"));
    RAY_TRY(expression, args->optString("expression"));
    RAY_TRY(value, args->optValue("value"));
    RAY_TRY(driven, args->optBool("driven"));
    if (!*expression && *value == nullptr && !*driven) {
        return args->error("nothing to change: give 'expression', 'value' or 'driven'");
    }
    Sketch model = e->model;
    const auto it = std::find_if(model.constraints.begin(), model.constraints.end(),
                                 [&](const Constraint& c) { return c.id == *id; });
    if (it == model.constraints.end()) {
        return args->error("constraint", "names no constraint of the sketch (" + id->toString() + ")");
    }
    if (!isDimension(it->type)) {
        return args->error("constraint", "is a " + std::string(constraintTypeName(it->type)) +
                                             " constraint; only dimensions have a value");
    }
    if (*expression) {
        if ((*expression)->empty()) {
            it->expression.reset();
        } else {
            it->expression = **expression;
        }
    }
    if (*value != nullptr) {
        if (!(*value)->isNumber() || !std::isfinite((*value)->asNumber())) {
            return args->error("value", "must be a finite number (mm or radians)");
        }
        it->value = (*value)->asNumber();
        if (!*expression) {
            it->expression.reset(); // a literal value replaces the expression
        }
    }
    if (*driven) {
        it->driven = **driven;
    }
    return commit(*e, model, std::string(kConstraintSet));
}

Result<DocStatePtr> split(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kSplit, argsJson, {"at", "entity", "id", "newEntity", "newPoint"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(entityId, args->id("entity"));
    RAY_TRY(at, pointArg(*args, "at"));
    Sketch model = e->model;
    const SketchEntId id{*entityId};
    Entity* entity = findEntity(model, id);
    if (entity == nullptr) {
        return args->error("entity", "names no entity of the sketch");
    }
    const auto cv = curveOf(model, *entity);
    if (!cv || cv->k == Curve::K::Circle) {
        return args->error("entity", "must be a line or an arc (trim a circle to make it an arc first)");
    }
    const double t = std::clamp(paramOf(*cv, *at), 0.0, cv->length());
    const double eps = std::max(1e-9, cv->length() * 1e-9);
    if (cv->k == Curve::K::Arc && paramOf(*cv, *at) > cv->length()) {
        return args->error("at", "is not on the arc");
    }
    if (t <= eps || t >= cv->length() - eps) {
        return args->error("at", "is at an end of the entity; there is nothing to split");
    }
    RAY_TRY(newPoint, newIdArg(*args, "newPoint", model, e->feature, id, "split.point"));
    model.entities.push_back(Entity{*newPoint, Point{pointAt(*cv, t)}, false});
    RAY_TRY(newEntity, newIdArg(*args, "newEntity", model, e->feature, id, "split.entity"));
    entity = findEntity(model, id);
    Entity piece{*newEntity, entity->geom, entity->construction};
    if (auto* l = std::get_if<Line>(&entity->geom)) {
        std::get<Line>(piece.geom).a = *newPoint;
        l->b = *newPoint;
    } else if (auto* a = std::get_if<Arc>(&entity->geom)) {
        std::get<Arc>(piece.geom).start = *newPoint;
        a->end = *newPoint;
    }
    model.entities.push_back(std::move(piece));
    return commit(*e, model, std::string(kSplit));
}

Result<DocStatePtr> trim(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kTrim, argsJson, {"at", "entity", "id", "newEntity", "newPoints"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(entityId, args->id("entity"));
    RAY_TRY(at, pointArg(*args, "at"));
    Sketch model = e->model;
    const SketchEntId id{*entityId};
    const Entity* entity = findEntity(model, id);
    if (entity == nullptr) {
        return args->error("entity", "names no entity of the sketch");
    }
    const auto cv = curveOf(model, *entity);
    if (!cv) {
        return args->error("entity", "must be a line, an arc or a circle");
    }
    const double length = cv->k == Curve::K::Circle ? kTwoPi * cv->r : cv->length();
    const double eps = std::max(1e-7, length * 1e-9);
    // Crossings with the other curves, as parameters on this one, strictly inside it. A crossing at an end point of
    // the other curve reuses that point, so the trimmed entity stays joined to it.
    std::vector<double> params;
    std::vector<std::pair<double, SketchEntId>> joints;
    for (const Entity& other : model.entities) {
        if (other.id == id || other.construction) {
            continue;
        }
        const auto oc = curveOf(model, other);
        if (!oc) {
            continue;
        }
        for (const Vec2& p : carrierCrossings(*cv, *oc)) {
            if (!onCurve(*cv, p, eps) || !onCurve(*oc, p, eps)) {
                continue;
            }
            const double t = paramOf(*cv, p);
            if (cv->k == Curve::K::Circle || (t > eps && t < length - eps)) {
                params.push_back(t);
                for (const SketchEntId& q : pointsUsed(other)) {
                    const auto qp = pointOf(model, q);
                    if (qp && len2(sub2(*qp, p)) <= eps && !std::holds_alternative<Circle>(other.geom)) {
                        joints.emplace_back(t, q);
                    }
                }
            }
        }
    }
    std::sort(params.begin(), params.end());
    params.erase(std::unique(params.begin(), params.end(), [&](double a, double b) { return b - a <= eps; }),
                 params.end());
    const double here = paramOf(*cv, *at);
    if (cv->k != Curve::K::Circle && here > length + eps) {
        return args->error("at", "is not on the arc");
    }
    RAY_TRY(newPoints, args->optValue("newPoints"));
    std::vector<SketchEntId> givenPoints;
    if (*newPoints != nullptr) {
        RAY_TRY(ids, idList(*args, "newPoints"));
        for (const Uuid& p : *ids) {
            if (findEntity(model, SketchEntId{p}) != nullptr) {
                return args->error("newPoints", "names " + p.toString() + ", which the sketch uses already");
            }
            givenPoints.push_back(SketchEntId{p});
        }
    }
    auto addPoint = [&](double t, std::size_t k) {
        for (const auto& [at, existing] : joints) {
            if (std::abs(at - t) <= eps) {
                return existing;
            }
        }
        const Vec2 p = pointAt(*cv, t);
        const SketchEntId pid = k < givenPoints.size()
                                    ? givenPoints[k]
                                    : SketchEntId{derivedId(model, e->feature, id, "trim.point." + std::to_string(k))};
        model.entities.push_back(Entity{pid, Point{p}, false});
        return pid;
    };
    // Points the entity used before; those no curve uses afterwards are removed with their constraints.
    const std::vector<SketchEntId> oldPoints = pointsUsed(*entity);
    auto cleanup = [&]() {
        std::set<SketchEntId> orphans(oldPoints.begin(), oldPoints.end());
        for (const Entity& x : model.entities) {
            for (const SketchEntId& p : pointsUsed(x)) {
                orphans.erase(p);
            }
        }
        // Only end points: a point some constraint or nothing else names stays if it is not an orphan.
        removeEntities(model, orphans);
    };

    if (cv->k == Curve::K::Circle) {
        if (params.size() < 2) {
            removeEntities(model, {id});
            return commit(*e, model, std::string(kTrim));
        }
        // The piece around `here` between its neighbouring crossings is removed; an arc from `hi` round to `lo`
        // (counter-clockwise) stays, with the circle's id.
        double lo = params.back();
        double hi = params.front();
        for (std::size_t i = 0; i + 1 < params.size(); ++i) {
            if (params[i] <= here && here < params[i + 1]) {
                lo = params[i];
                hi = params[i + 1];
            }
        }
        const SketchEntId start = addPoint(hi, 0);
        const SketchEntId end = addPoint(lo, 1);
        Entity* circle = findEntity(model, id);
        const SketchEntId center = std::get<Circle>(circle->geom).center;
        circle->geom = Arc{center, start, end, true};
        return commit(*e, model, std::string(kTrim));
    }

    std::optional<double> lo, hi;
    for (const double t : params) {
        if (t < here) {
            lo = t;
        } else if (!hi && t > here) {
            hi = t;
        }
    }
    if (!lo && !hi) {
        removeEntities(model, {id});
        return commit(*e, model, std::string(kTrim));
    }
    auto setStart = [&](Entity& x, const SketchEntId& p) {
        if (auto* l = std::get_if<Line>(&x.geom)) {
            l->a = p;
        } else if (auto* a = std::get_if<Arc>(&x.geom)) {
            a->start = p;
        }
    };
    auto setEnd = [&](Entity& x, const SketchEntId& p) {
        if (auto* l = std::get_if<Line>(&x.geom)) {
            l->b = p;
        } else if (auto* a = std::get_if<Arc>(&x.geom)) {
            a->end = p;
        }
    };
    if (!lo) {
        const SketchEntId p = addPoint(*hi, 0);
        setStart(*findEntity(model, id), p);
    } else if (!hi) {
        const SketchEntId p = addPoint(*lo, 0);
        setEnd(*findEntity(model, id), p);
    } else {
        const SketchEntId p1 = addPoint(*lo, 0);
        const SketchEntId p2 = addPoint(*hi, 1);
        RAY_TRY(newEntity, newIdArg(*args, "newEntity", model, e->feature, id, "trim.entity"));
        Entity* x = findEntity(model, id);
        Entity piece{*newEntity, x->geom, x->construction};
        setStart(piece, p2);
        setEnd(*x, p1);
        model.entities.push_back(std::move(piece));
    }
    cleanup();
    return commit(*e, model, std::string(kTrim));
}

#undef RAY_TRY

using ApplyFn = Result<DocStatePtr> (*)(const DocState&, std::string_view);

class SketchCommand final : public ICommandHandler {
public:
    SketchCommand(std::string_view name, std::string schema, ApplyFn fn)
        : name_(name), schema_(std::move(schema)), fn_(fn)
    {
    }
    std::string_view name() const override { return name_; }
    std::string_view argsSchemaJson() const override { return schema_; }
    bool changesToolpathsOrGcode() const override { return false; }
    Result<DocStatePtr> apply(DocStatePtr before, std::string_view argsJson) const override
    {
        if (!before) {
            return invalid(std::string(name_) + ": there is no document snapshot to apply it to");
        }
        try {
            return fn_(*before, argsJson);
        } catch (const std::bad_alloc&) {
            return failed(ErrorCode::Internal, std::string(name_) + ": out of memory");
        } catch (...) {
            return failed(ErrorCode::Internal, std::string(name_) + ": unexpected failure");
        }
    }

private:
    std::string_view name_;
    std::string schema_;
    ApplyFn fn_;
};

std::string schemaOf(const std::string& properties, const std::string& required)
{
    return objectSchema(required, R"("id":)" + uuidSchema() + (properties.empty() ? "" : ",") + properties);
}

} // namespace

std::vector<std::unique_ptr<ICommandHandler>> makeSketchCommands()
{
    const std::string uuid = uuidSchema();
    const std::string ids = R"({"type":"array","minItems":1,"items":)" + uuid + "}";
    const std::string point = R"({"type":"object","additionalProperties":false,"required":["x","y"],)"
                              R"("properties":{"x":{"type":"number"},"y":{"type":"number"}}})";
    const std::string objects = R"({"type":"array","minItems":1,"items":{"type":"object"}})";
    std::vector<std::unique_ptr<ICommandHandler>> out;
    out.push_back(std::make_unique<SketchCommand>(
        kSet, schemaOf(R"("sketch":{"type":"object"},"plane":)" + planeSchema(), R"("id","sketch")"), &sketchSet));
    out.push_back(std::make_unique<SketchCommand>(
        kEntityAdd, schemaOf(R"("entities":)" + objects, R"("id","entities")"), &entityAdd));
    out.push_back(std::make_unique<SketchCommand>(kEntityRemove, schemaOf(R"("entities":)" + ids, R"("id","entities")"),
                                                  &entityRemove));
    out.push_back(std::make_unique<SketchCommand>(
        kConstraintAdd, schemaOf(R"("constraints":)" + objects, R"("id","constraints")"), &constraintAdd));
    out.push_back(std::make_unique<SketchCommand>(
        kConstraintRemove, schemaOf(R"("constraints":)" + ids, R"("id","constraints")"), &constraintRemove));
    out.push_back(std::make_unique<SketchCommand>(
        kConstraintSet,
        schemaOf(
            R"("constraint":)" + uuid +
                R"(,"expression":{"type":"string","description":"An expression such as \"12 mm\" or \"width / 2\"; )"
                R"(\"\" removes it"},"value":{"type":"number"},"driven":{"type":"boolean"})",
            R"("id","constraint")"),
        &constraintSet));
    out.push_back(std::make_unique<SketchCommand>(
        kSplit,
        schemaOf(R"("entity":)" + uuid + R"(,"at":)" + point + R"(,"newEntity":)" + uuid + R"(,"newPoint":)" + uuid,
                 R"("id","entity","at")"),
        &split));
    out.push_back(std::make_unique<SketchCommand>(
        kTrim,
        schemaOf(R"("entity":)" + uuid + R"(,"at":)" + point + R"(,"newEntity":)" + uuid +
                     R"(,"newPoints":{"type":"array","minItems":1,"maxItems":2,"items":)" + uuid + "}",
                 R"("id","entity","at")"),
        &trim));
    return out;
}

} // namespace ray::doc::features
