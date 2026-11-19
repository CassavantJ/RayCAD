// The sketch editing commands (sketch_commands.h).
#include "sketch_commands.h"

#include "feature_props.h"
#include "sketch_feature.h"
#include "sketch_json.h"

#include "../commands/args.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <map>
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

// T-093: the props with "labels" (dimension label offsets) kept only for dimensions `model` still has, and with
// `edits` applied (nullopt removes a label). The member goes when no label is left.
using LabelEdits = std::map<Uuid, std::optional<Vec2>>;

Result<std::string> withLabels(const std::string& where, const std::string& propsJson, const Sketch& model,
                               const LabelEdits& edits)
{
    RAY_TRY(root, PropsReader::parse(where, propsJson));
    const Value* old = root->find("labels");
    if (old == nullptr && edits.empty()) {
        return propsJson;
    }
    std::set<Uuid> dimensions;
    for (const Constraint& c : model.constraints) {
        if (isDimension(c.type)) {
            dimensions.insert(c.id);
        }
    }
    std::map<Uuid, Vec2> labels;
    if (old != nullptr && old->isObject()) {
        for (const Member& m : old->asObject()) {
            const Value* x = m.value.find("x");
            const Value* y = m.value.find("y");
            if (x != nullptr && y != nullptr && x->isNumber() && y->isNumber()) {
                labels[Uuid::fromString(m.key)] = Vec2{x->asNumber(), y->asNumber()};
            }
        }
    }
    for (const auto& [id, at] : edits) {
        if (at) {
            labels[id] = *at;
        } else {
            labels.erase(id);
        }
    }
    Value::Object out;
    for (const auto& [id, at] : labels) {
        if (dimensions.count(id)) {
            out.push_back({id.toString(), Value::object({{"x", Value::number(at.x)}, {"y", Value::number(at.y)}})});
        }
    }
    Value::Object members;
    for (const Member& m : root->asObject()) {
        if (m.key != "labels") {
            members.push_back(m);
        }
    }
    if (!out.empty()) {
        members.push_back({"labels", Value::object(std::move(out))});
    }
    return json::write(Value::object(std::move(members)));
}

// Writes `model` into the feature and checks it as the sketch type does for its own commands.
Result<DocStatePtr> commit(Editing& e, const Sketch& model, const std::string& command, const LabelEdits& labels = {})
{
    Feature& feature = e.next->features[e.index];
    RAY_TRY(props, withSketch(feature, model));
    RAY_TRY(labelled, withLabels(command, *props, model, labels));
    feature.propsJson = std::move(*labelled);
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
constexpr std::string_view kConstruction = "sketch.construction";
constexpr std::string_view kExtend = "sketch.extend";
constexpr std::string_view kOffset = "sketch.offset";
constexpr std::string_view kFillet = "sketch.fillet";
constexpr std::string_view kDimensionPlace = "sketch.dimension.place";

Result<DocStatePtr> sketchSet(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kSet, argsJson, {"id", "plane", "sketch"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(sketchValue, args->optValue("sketch"));
    if (*sketchValue == nullptr) {
        return args->error("sketch", "is required");
    }
    const PropsReader r(std::string(kSet), **sketchValue);
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
    RAY_TRY(args, Args::parse(kEntityAdd, argsJson, {"constraints", "entities", "id"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(entities, arrayArg(*args, "entities"));
    // T-092: the constraints a drawing tool inferred go in with the entities, as the same undo step.
    RAY_TRY(constraints, args->optValue("constraints"));
    if (*constraints != nullptr && !(*constraints)->isArray()) {
        return args->error("constraints", "must be an array");
    }
    RAY_TRY(model, merged(e->model, *entities, *constraints, std::string(kEntityAdd)));
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

// A label offset {"x", "y"} (mm, finite).
std::optional<Vec2> labelOffset(const Value& v)
{
    if (!v.isObject() || v.asObject().size() != 2) {
        return std::nullopt;
    }
    const Value* x = v.find("x");
    const Value* y = v.find("y");
    if (x == nullptr || y == nullptr || !x->isNumber() || !y->isNumber() || !std::isfinite(x->asNumber()) ||
        !std::isfinite(y->asNumber())) {
        return std::nullopt;
    }
    return Vec2{x->asNumber(), y->asNumber()};
}

// The label offsets of dimensions in `model` named by a {"<constraint id>": {"x", "y"}} argument.
Result<LabelEdits> labelArg(const Args& args, std::string_view key, const Sketch& model)
{
    RAY_TRY(value, args.optValue(key));
    LabelEdits out;
    if (*value == nullptr) {
        return out;
    }
    if (!(*value)->isObject()) {
        return args.error(key, "must be an object {\"<dimension id>\": {\"x\": <mm>, \"y\": <mm>}}");
    }
    for (const Member& m : (*value)->asObject()) {
        const Uuid id = Uuid::fromString(m.key);
        const auto it = std::find_if(model.constraints.begin(), model.constraints.end(),
                                     [&](const Constraint& c) { return c.id == id; });
        if (id.toString() != m.key || it == model.constraints.end() || !isDimension(it->type)) {
            return args.error(key, "names " + m.key + ", which is not a dimension of the sketch");
        }
        const auto at = labelOffset(m.value);
        if (!at) {
            return args.error(key, "gives " + m.key + " an offset that is not {\"x\": <mm>, \"y\": <mm>}");
        }
        out[id] = *at;
    }
    return out;
}

// T-093: "solved" is the caller's solved form of the same model (sketch_json.h form): its coordinates (points, circle
// radii, slot widths) replace the stored ones, so the stored shape is the one the person sees and the rebuild solves
// on from there (a solve from stale coordinates can land on another solution, e.g. a point on the far side of a line).
Status takeSolved(const Args& args, Sketch& model)
{
    RAY_TRY(value, args.optValue("solved"));
    if (*value == nullptr) {
        return {};
    }
    const PropsReader r(args.command(), **value);
    RAY_TRY(solved, sketchFromJson(r, **value, "solved"));
    if (solved->entities.size() != model.entities.size()) {
        return args.error("solved", "must have the same entities as the sketch");
    }
    for (Entity& e : model.entities) {
        const auto it = std::find_if(solved->entities.begin(), solved->entities.end(),
                                     [&](const Entity& s) { return s.id == e.id; });
        if (it == solved->entities.end() || it->geom.index() != e.geom.index()) {
            return args.error("solved", "must have the same entities as the sketch (" + entityText(e.id) + ")");
        }
        if (auto* p = std::get_if<Point>(&e.geom)) {
            p->p = std::get<Point>(it->geom).p;
        } else if (auto* c = std::get_if<Circle>(&e.geom)) {
            c->radius = std::get<Circle>(it->geom).radius;
        } else if (auto* s = std::get_if<Slot>(&e.geom)) {
            s->width = std::get<Slot>(it->geom).width;
        }
    }
    return {};
}

Result<DocStatePtr> constraintAdd(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kConstraintAdd, argsJson, {"constraints", "id", "labels", "solved"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(constraints, arrayArg(*args, "constraints"));
    RAY_TRY(model, merged(e->model, nullptr, *constraints, std::string(kConstraintAdd)));
    RAY_TRY(labels, labelArg(*args, "labels", *model));
    RAY_TRY(solved, takeSolved(*args, *model));
    return commit(*e, *model, std::string(kConstraintAdd), *labels);
}

// T-093: where a dimension's value label sits, as an offset from the dimension's own anchor (the UI decides the
// anchor: the middle of a distance, the centre of a circle, the vertex of an angle). Props only; no geometry changes.
Result<DocStatePtr> dimensionPlace(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kDimensionPlace, argsJson, {"constraint", "id", "offset"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(id, args->id("constraint"));
    const auto it = std::find_if(e->model.constraints.begin(), e->model.constraints.end(),
                                 [&](const Constraint& c) { return c.id == *id; });
    if (it == e->model.constraints.end() || !isDimension(it->type)) {
        return args->error("constraint", "names no dimension of the sketch (" + id->toString() + ")");
    }
    RAY_TRY(offset, args->optValue("offset"));
    if (*offset == nullptr) {
        return args->error("offset", "is required: {\"x\": <mm>, \"y\": <mm>}, or null to remove it");
    }
    LabelEdits edits;
    if ((*offset)->isNull()) {
        edits[*id] = std::nullopt;
    } else {
        const auto at = labelOffset(**offset);
        if (!at) {
            return args->error("offset", "must be {\"x\": <mm>, \"y\": <mm>} or null");
        }
        edits[*id] = *at;
    }
    return commit(*e, e->model, std::string(kDimensionPlace), edits);
}

Result<DocStatePtr> constraintRemove(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kConstraintRemove, argsJson, {"constraints", "id", "solved"}));
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
    RAY_TRY(solved, takeSolved(*args, model));
    return commit(*e, model, std::string(kConstraintRemove));
}

Result<DocStatePtr> constraintSet(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args,
            Args::parse(kConstraintSet, argsJson, {"constraint", "driven", "expression", "id", "solved", "value"}));
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
    RAY_TRY(solved, takeSolved(*args, model));
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

// ---------- T-092: construction, extend, offset, corner fillet ----------

bool pointUsedElsewhere(const Sketch& s, const SketchEntId& point, const SketchEntId& except)
{
    for (const Entity& x : s.entities) {
        if (x.id == except) {
            continue;
        }
        for (const SketchEntId& p : pointsUsed(x)) {
            if (p == point) {
                return true;
            }
        }
    }
    return std::any_of(s.constraints.begin(), s.constraints.end(), [&](const Constraint& c) {
        return std::find(c.refs.begin(), c.refs.end(), point) != c.refs.end();
    });
}

Result<DocStatePtr> construction(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kConstruction, argsJson, {"construction", "entities", "id"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(ids, idList(*args, "entities"));
    RAY_TRY(on, args->boolean("construction"));
    Sketch model = e->model;
    for (const Uuid& id : *ids) {
        Entity* x = findEntity(model, SketchEntId{id});
        if (x == nullptr) {
            return args->error("entities", "names " + id.toString() + ", which is not an entity of the sketch");
        }
        x->construction = *on;
    }
    return commit(*e, model, std::string(kConstruction));
}

Result<DocStatePtr> extend(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kExtend, argsJson, {"at", "entity", "id", "newPoint"}));
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
    if (!cv || cv->k == Curve::K::Circle) {
        return args->error("entity", "must be a line or an arc");
    }
    // The end nearest `at` grows along the curve's carrier to the first crossing with another curve.
    const bool atEnd = len2(sub2(*at, cv->b)) < len2(sub2(*at, cv->a));
    const double length = cv->length();
    const double eps = std::max(1e-7, length * 1e-9);
    std::optional<double> best; // distance travelled beyond the end
    Vec2 target{};
    std::optional<SketchEntId> joint;
    for (const Entity& other : model.entities) {
        if (other.id == id || other.construction) {
            continue;
        }
        const auto oc = curveOf(model, other);
        if (!oc) {
            continue;
        }
        for (const Vec2& p : carrierCrossings(*cv, *oc)) {
            if (!onCurve(*oc, p, eps)) {
                continue;
            }
            double beyond = 0;
            if (cv->k == Curve::K::Line) {
                const double t = paramOf(*cv, p);
                beyond = atEnd ? t - length : -t;
            } else {
                // Along the arc's direction from its start; past the end lies in (sweep, 2 pi r).
                const double t = paramOf(*cv, p);
                const double full = kTwoPi * cv->r;
                if (t <= length + eps || t >= full - eps) {
                    continue;
                }
                beyond = atEnd ? t - length : full - t;
            }
            if (beyond <= eps || (best && beyond >= *best)) {
                continue;
            }
            best = beyond;
            target = p;
            joint.reset();
            for (const SketchEntId& q : pointsUsed(other)) {
                const auto qp = pointOf(model, q);
                if (qp && len2(sub2(*qp, p)) <= eps && !std::holds_alternative<Circle>(other.geom)) {
                    joint = q;
                }
            }
        }
    }
    if (!best) {
        return args->error("entity", "has no curve to extend to beyond that end");
    }
    const SketchEntId end = [&] {
        if (const auto* l = std::get_if<Line>(&entity->geom)) {
            return atEnd ? l->b : l->a;
        }
        const auto& a = std::get<Arc>(entity->geom);
        return atEnd ? a.end : a.start;
    }();
    SketchEntId moved = end;
    if (joint) {
        moved = *joint;
    } else if (pointUsedElsewhere(model, end, id)) {
        // The end is shared (a joint, a constraint): the extended entity gets a point of its own.
        RAY_TRY(newPoint, newIdArg(*args, "newPoint", model, e->feature, id, "extend.point"));
        model.entities.push_back(Entity{*newPoint, Point{target}, false});
        moved = *newPoint;
    } else {
        std::get<Point>(findEntity(model, end)->geom).p = target;
    }
    Entity* x = findEntity(model, id);
    if (auto* l = std::get_if<Line>(&x->geom)) {
        (atEnd ? l->b : l->a) = moved;
    } else if (auto* a = std::get_if<Arc>(&x->geom)) {
        (atEnd ? a->end : a->start) = moved;
    }
    if (moved != end && !pointUsedElsewhere(model, end, SketchEntId{})) {
        removeEntities(model, {end});
    }
    return commit(*e, model, std::string(kExtend));
}

// One curve of an offset chain, walked from `from` to `to` (its own points; for a circle both are its centre).
struct ChainPart {
    SketchEntId entity;
    Curve cv;
    bool reversed = false; // walked from the curve's end to its start
    SketchEntId from, to;
};

Vec2 partStart(const ChainPart& p)
{
    return p.reversed ? p.cv.b : p.cv.a;
}
Vec2 partEnd(const ChainPart& p)
{
    return p.reversed ? p.cv.a : p.cv.b;
}

// Unit left normal of the walking direction at the part's start (a line: constant).
Vec2 leftNormalAt(const ChainPart& p, Vec2 at)
{
    if (p.cv.k == Curve::K::Line) {
        Vec2 d = p.reversed ? sub2(p.cv.a, p.cv.b) : sub2(p.cv.b, p.cv.a);
        const double l = len2(d);
        return l > 0 ? Vec2{-d.y / l, d.x / l} : Vec2{0, 0};
    }
    // Circular: walking counter-clockwise, left is towards the centre.
    const bool ccw = p.cv.k == Curve::K::Circle ? true : (p.cv.ccw != p.reversed);
    Vec2 r = sub2(p.cv.c, at);
    const double l = len2(r);
    r = l > 0 ? Vec2{r.x / l, r.y / l} : Vec2{0, 0};
    return ccw ? r : Vec2{-r.x, -r.y};
}

Vec2 nearestOn(const Curve& cv, Vec2 p)
{
    if (cv.k == Curve::K::Line) {
        return pointAt(cv, std::clamp(paramOf(cv, p), 0.0, cv.length()));
    }
    const double d = len2(sub2(p, cv.c));
    const Vec2 onCircle = d > 0 ? Vec2{cv.c.x + (p.x - cv.c.x) * cv.r / d, cv.c.y + (p.y - cv.c.y) * cv.r / d}
                                : Vec2{cv.c.x + cv.r, cv.c.y};
    if (cv.k == Curve::K::Circle || paramOf(cv, onCircle) <= cv.length()) {
        return onCircle;
    }
    return len2(sub2(p, cv.a)) < len2(sub2(p, cv.b)) ? cv.a : cv.b;
}

Result<DocStatePtr> offset(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kOffset, argsJson, {"distance", "entities", "id", "side"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(ids, idList(*args, "entities"));
    RAY_TRY(side, pointArg(*args, "side"));
    RAY_TRY(distanceValue, args->optValue("distance"));
    if (*distanceValue == nullptr || !(*distanceValue)->isNumber() || !((*distanceValue)->asNumber() > 0) ||
        !std::isfinite((*distanceValue)->asNumber())) {
        return args->error("distance", "must be a positive number of mm");
    }
    const double d = (*distanceValue)->asNumber();
    Sketch model = e->model;
    // The curves, in the order given, gathered into chains that share end points.
    std::vector<ChainPart> pool;
    for (const Uuid& id : *ids) {
        const Entity* x = findEntity(model, SketchEntId{id});
        if (x == nullptr) {
            return args->error("entities", "names " + id.toString() + ", which is not an entity of the sketch");
        }
        const auto cv = curveOf(model, *x);
        if (!cv) {
            return args->error("entities", "names " + id.toString() + ", which is not a line, an arc or a circle");
        }
        ChainPart part{x->id, *cv, false, {}, {}};
        if (const auto* l = std::get_if<Line>(&x->geom)) {
            part.from = l->a;
            part.to = l->b;
        } else if (const auto* a = std::get_if<Arc>(&x->geom)) {
            part.from = a->start;
            part.to = a->end;
        } else {
            part.from = part.to = std::get<Circle>(x->geom).center;
        }
        pool.push_back(part);
    }
    std::vector<std::vector<ChainPart>> chains;
    std::vector<bool> used(pool.size(), false);
    for (std::size_t seed = 0; seed < pool.size(); ++seed) {
        if (used[seed]) {
            continue;
        }
        used[seed] = true;
        std::vector<ChainPart> chain{pool[seed]};
        if (pool[seed].cv.k != Curve::K::Circle) {
            // Grow forwards from the end, then backwards from the start.
            for (const bool forward : {true, false}) {
                bool grew = true;
                while (grew) {
                    grew = false;
                    const SketchEntId tip = forward ? chain.back().to : chain.front().from;
                    for (std::size_t k = 0; k < pool.size(); ++k) {
                        if (used[k] || pool[k].cv.k == Curve::K::Circle) {
                            continue;
                        }
                        ChainPart next = pool[k];
                        if (next.from == tip || next.to == tip) {
                            const bool startsAtTip = next.from == tip;
                            if (forward != startsAtTip) {
                                next.reversed = true;
                                std::swap(next.from, next.to);
                            }
                            used[k] = true;
                            if (forward) {
                                chain.push_back(next);
                            } else {
                                chain.insert(chain.begin(), next);
                            }
                            grew = true;
                            break;
                        }
                    }
                }
            }
        }
        chains.push_back(std::move(chain));
    }
    std::size_t serial = 0;
    auto newId = [&](std::string_view what) {
        return derivedId(model, e->feature, SketchEntId{(*ids)[0]},
                         std::string("offset.") + std::string(what) + "." + std::to_string(serial++));
    };
    for (const std::vector<ChainPart>& chain : chains) {
        // Which side: that of `side` at the part nearest to it.
        std::size_t nearest = 0;
        double bestDistance = -1;
        for (std::size_t k = 0; k < chain.size(); ++k) {
            const double dist = len2(sub2(nearestOn(chain[k].cv, *side), *side));
            if (bestDistance < 0 || dist < bestDistance) {
                bestDistance = dist;
                nearest = k;
            }
        }
        const Vec2 foot = nearestOn(chain[nearest].cv, *side);
        const double sign = dot2(sub2(*side, foot), leftNormalAt(chain[nearest], foot)) >= 0 ? 1.0 : -1.0;
        // The offset curves, as copies whose points are moved.
        struct Piece {
            Curve cv;
            Vec2 start, end; // in walking order
        };
        std::vector<Piece> pieces;
        for (const ChainPart& p : chain) {
            Piece piece{p.cv, partStart(p), partEnd(p)};
            if (p.cv.k == Curve::K::Line) {
                const Vec2 n = leftNormalAt(p, piece.start);
                const Vec2 shift{n.x * d * sign, n.y * d * sign};
                piece.cv.a = {p.cv.a.x + shift.x, p.cv.a.y + shift.y};
                piece.cv.b = {p.cv.b.x + shift.x, p.cv.b.y + shift.y};
            } else {
                const bool ccw = p.cv.k == Curve::K::Circle ? true : (p.cv.ccw != p.reversed);
                // Left of a counter-clockwise walk is the centre: the radius shrinks.
                const double r = p.cv.r + (ccw ? -1.0 : 1.0) * d * sign;
                if (!(r > 1e-9)) {
                    return args->error("distance", "is larger than the radius of an arc or circle on that side");
                }
                const auto scale = [&](Vec2 q) {
                    const Vec2 v = sub2(q, p.cv.c);
                    const double l = len2(v);
                    return l > 0 ? Vec2{p.cv.c.x + v.x * r / l, p.cv.c.y + v.y * r / l} : q;
                };
                piece.cv.r = r;
                piece.cv.a = scale(p.cv.a);
                piece.cv.b = scale(p.cv.b);
            }
            piece.start = p.reversed ? piece.cv.b : piece.cv.a;
            piece.end = p.reversed ? piece.cv.a : piece.cv.b;
            pieces.push_back(piece);
        }
        // Joints: neighbours that met meet again, at the crossing of their offset carriers nearest the gap.
        const bool closed = chain.size() > 1 && chain.front().from == chain.back().to;
        const std::size_t joints = chain.size() > 1 ? (closed ? chain.size() : chain.size() - 1) : 0;
        std::vector<std::optional<Vec2>> jointAt(chain.size()); // the joint after part k
        for (std::size_t k = 0; k < joints; ++k) {
            Piece& p = pieces[k];
            Piece& q = pieces[(k + 1) % pieces.size()];
            const Vec2 gap{(p.end.x + q.start.x) / 2, (p.end.y + q.start.y) / 2};
            Vec2 meet = gap;
            if (len2(sub2(p.end, q.start)) > 1e-9) {
                std::optional<Vec2> bestCrossing;
                for (const Vec2& c : carrierCrossings(p.cv, q.cv)) {
                    if (!bestCrossing || len2(sub2(c, gap)) < len2(sub2(*bestCrossing, gap))) {
                        bestCrossing = c;
                    }
                }
                if (bestCrossing) {
                    meet = *bestCrossing;
                }
            }
            jointAt[k] = meet;
            p.end = meet;
            q.start = meet;
        }
        // New entities: shared joint points, the curves (arcs and circles keep the original centre: concentric).
        std::vector<SketchEntId> jointIds(chain.size());
        for (std::size_t k = 0; k < joints; ++k) {
            jointIds[k] = SketchEntId{newId("point")};
            model.entities.push_back(Entity{jointIds[k], Point{*jointAt[k]}, false});
        }
        for (std::size_t k = 0; k < chain.size(); ++k) {
            const ChainPart& part = chain[k];
            const Piece& piece = pieces[k];
            // Copied out: pushing entities below moves them.
            const Entity original = *findEntity(model, part.entity);
            const bool construction = original.construction;
            if (part.cv.k == Curve::K::Circle) {
                const SketchEntId c = std::get<Circle>(original.geom).center;
                model.entities.push_back(Entity{SketchEntId{newId("circle")}, Circle{c, piece.cv.r}, construction});
                continue;
            }
            const bool hasPrev = k > 0 || closed;
            const bool hasNext = k < joints;
            SketchEntId startId, endId;
            if (hasPrev) {
                startId = jointIds[(k + chain.size() - 1) % chain.size()];
            } else {
                startId = SketchEntId{newId("point")};
                model.entities.push_back(Entity{startId, Point{piece.start}, false});
            }
            if (hasNext) {
                endId = jointIds[k];
            } else {
                endId = SketchEntId{newId("point")};
                model.entities.push_back(Entity{endId, Point{piece.end}, false});
            }
            // Back to the curve's own direction.
            const SketchEntId a = part.reversed ? endId : startId;
            const SketchEntId b = part.reversed ? startId : endId;
            const SketchEntId curveId{newId("curve")};
            if (part.cv.k == Curve::K::Line) {
                model.entities.push_back(Entity{curveId, Line{a, b}, construction});
                model.constraints.push_back(
                    Constraint{newId("parallel"), ConstraintType::Parallel, {part.entity, curveId}, {}, 0, false});
            } else {
                const auto& arc = std::get<Arc>(original.geom);
                model.entities.push_back(Entity{curveId, Arc{arc.center, a, b, arc.ccw}, construction});
            }
        }
    }
    std::sort(model.entities.begin(), model.entities.end(),
              [](const Entity& x, const Entity& y) { return x.id < y.id; });
    return commit(*e, model, std::string(kOffset));
}

Result<DocStatePtr> fillet(const DocState& before, std::string_view argsJson)
{
    RAY_TRY(args, Args::parse(kFillet, argsJson, {"id", "newEntity", "point", "radius"}));
    RAY_TRY(e, open(*args, before));
    RAY_TRY(pointId, args->id("point"));
    RAY_TRY(radiusValue, args->optValue("radius"));
    if (*radiusValue == nullptr || !(*radiusValue)->isNumber() || !((*radiusValue)->asNumber() > 0) ||
        !std::isfinite((*radiusValue)->asNumber())) {
        return args->error("radius", "must be a positive number of mm");
    }
    const double r = (*radiusValue)->asNumber();
    Sketch model = e->model;
    const SketchEntId corner{*pointId};
    const auto p = pointOf(model, corner);
    if (!p) {
        return args->error("point", "names no point of the sketch");
    }
    // The two lines that meet there.
    std::vector<SketchEntId> lines;
    for (const Entity& x : model.entities) {
        for (const SketchEntId& q : pointsUsed(x)) {
            if (q == corner) {
                if (!std::holds_alternative<Line>(x.geom)) {
                    return args->error("point", "is the end of a curve that is not a line; a corner fillet joins two "
                                                "lines");
                }
                lines.push_back(x.id);
                break;
            }
        }
    }
    if (lines.size() != 2) {
        return args->error("point",
                           "must be the corner of exactly two lines (it joins " + std::to_string(lines.size()) + ")");
    }
    Vec2 far[2];
    for (int i = 0; i < 2; ++i) {
        const auto& l = std::get<Line>(findEntity(model, lines[static_cast<std::size_t>(i)])->geom);
        far[i] = *pointOf(model, l.a == corner ? l.b : l.a);
    }
    const Vec2 v1 = sub2(far[0], *p), v2 = sub2(far[1], *p);
    const double l1 = len2(v1), l2 = len2(v2);
    if (!(l1 > 0) || !(l2 > 0)) {
        return args->error("point", "joins a line of zero length");
    }
    const Vec2 u1{v1.x / l1, v1.y / l1}, u2{v2.x / l2, v2.y / l2};
    const double cosAngle = std::clamp(dot2(u1, u2), -1.0, 1.0);
    const double angle = dmath::acos(cosAngle);
    if (angle < 1e-6 || angle > kTwoPi / 2 - 1e-6) {
        return args->error("point", "joins two lines that are in line; there is no corner to round");
    }
    const double half = angle / 2;
    const double t = r / dmath::tan(half);
    if (t >= l1 || t >= l2) {
        return args->error("radius", "is too large for these lines (the fillet would reach past an end)");
    }
    const Vec2 t1{p->x + u1.x * t, p->y + u1.y * t};
    const Vec2 t2{p->x + u2.x * t, p->y + u2.y * t};
    Vec2 bis{u1.x + u2.x, u1.y + u2.y};
    const double bl = len2(bis);
    bis = {bis.x / bl, bis.y / bl};
    const double h = r / dmath::sin(half);
    const Vec2 c{p->x + bis.x * h, p->y + bis.y * h};
    const bool ccw = cross2(sub2(t1, c), sub2(t2, c)) > 0;
    RAY_TRY(arcId, newIdArg(*args, "newEntity", model, e->feature, corner, "fillet.arc"));
    const SketchEntId p1{derivedId(model, e->feature, corner, "fillet.start")};
    model.entities.push_back(Entity{p1, Point{t1}, false});
    const SketchEntId p2{derivedId(model, e->feature, corner, "fillet.end")};
    model.entities.push_back(Entity{p2, Point{t2}, false});
    const SketchEntId pc{derivedId(model, e->feature, corner, "fillet.center")};
    model.entities.push_back(Entity{pc, Point{c}, false});
    model.entities.push_back(Entity{*arcId, Arc{pc, p1, p2, ccw}, false});
    for (int i = 0; i < 2; ++i) {
        auto& l = std::get<Line>(findEntity(model, lines[static_cast<std::size_t>(i)])->geom);
        (l.a == corner ? l.a : l.b) = i == 0 ? p1 : p2;
        model.constraints.push_back(
            Constraint{derivedId(model, e->feature, corner, "fillet.tangent." + std::to_string(i)),
                       ConstraintType::Tangent,
                       {lines[static_cast<std::size_t>(i)], *arcId, i == 0 ? p1 : p2},
                       {},
                       0,
                       false});
    }
    // Something may still name the corner (a dimension, a constraint): then it stays as a free point.
    if (!pointUsedElsewhere(model, corner, SketchEntId{})) {
        removeEntities(model, {corner});
    }
    std::sort(model.entities.begin(), model.entities.end(),
              [](const Entity& x, const Entity& y) { return x.id < y.id; });
    return commit(*e, model, std::string(kFillet));
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
        kEntityAdd,
        schemaOf(R"("entities":)" + objects + R"(,"constraints":{"type":"array","items":{"type":"object"}})",
                 R"("id","entities")"),
        &entityAdd));
    out.push_back(std::make_unique<SketchCommand>(kEntityRemove, schemaOf(R"("entities":)" + ids, R"("id","entities")"),
                                                  &entityRemove));
    const std::string labels = R"({"type":"object","additionalProperties":)" + point + "}";
    out.push_back(std::make_unique<SketchCommand>(
        kConstraintAdd,
        schemaOf(R"("constraints":)" + objects + R"(,"labels":)" + labels + R"(,"solved":{"type":"object"})",
                 R"("id","constraints")"),
        &constraintAdd));
    out.push_back(std::make_unique<SketchCommand>(
        kConstraintRemove,
        schemaOf(R"("constraints":)" + ids + R"(,"solved":{"type":"object"})", R"("id","constraints")"),
        &constraintRemove));
    out.push_back(std::make_unique<SketchCommand>(
        kConstraintSet,
        schemaOf(
            R"("constraint":)" + uuid +
                R"(,"expression":{"type":"string","description":"An expression such as \"12 mm\" or \"width / 2\"; )"
                R"(\"\" removes it"},"value":{"type":"number"},"driven":{"type":"boolean"},"solved":{"type":"object"})",
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
    // T-092
    out.push_back(std::make_unique<SketchCommand>(
        kConstruction,
        schemaOf(R"("entities":)" + ids + R"(,"construction":{"type":"boolean"})", R"("id","entities","construction")"),
        &construction));
    out.push_back(std::make_unique<SketchCommand>(
        kExtend,
        schemaOf(R"("entity":)" + uuid + R"(,"at":)" + point + R"(,"newPoint":)" + uuid, R"("id","entity","at")"),
        &extend));
    out.push_back(std::make_unique<SketchCommand>(
        kOffset,
        schemaOf(R"("entities":)" + ids + R"(,"distance":{"type":"number","exclusiveMinimum":0},"side":)" + point,
                 R"("id","entities","distance","side")"),
        &offset));
    out.push_back(std::make_unique<SketchCommand>(
        kFillet,
        schemaOf(R"("point":)" + uuid + R"(,"radius":{"type":"number","exclusiveMinimum":0},"newEntity":)" + uuid,
                 R"("id","point","radius")"),
        &fillet));
    // T-093
    out.push_back(std::make_unique<SketchCommand>(
        kDimensionPlace,
        schemaOf(R"("constraint":)" + uuid + R"(,"offset":{"anyOf":[)" + point + R"(,{"type":"null"}]})",
                 R"("id","constraint","offset")"),
        &dimensionPlace));
    return out;
}

} // namespace ray::doc::features
