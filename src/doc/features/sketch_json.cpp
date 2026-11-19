// The JSON form of a sketch model (sketch_json.h).
#include "sketch_json.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <span>
#include <utility>

namespace ray::doc::features {

using json::Member;
using json::Value;
using namespace ray::sketch;

namespace {

struct TypeName {
    ConstraintType type;
    const char* name;
};
constexpr TypeName kConstraintNames[] = {
    {ConstraintType::Coincident, "coincident"},
    {ConstraintType::Horizontal, "horizontal"},
    {ConstraintType::Vertical, "vertical"},
    {ConstraintType::Parallel, "parallel"},
    {ConstraintType::Perpendicular, "perpendicular"},
    {ConstraintType::Tangent, "tangent"},
    {ConstraintType::Equal, "equal"},
    {ConstraintType::Symmetric, "symmetric"},
    {ConstraintType::Concentric, "concentric"},
    {ConstraintType::Midpoint, "midpoint"},
    {ConstraintType::PointOnCurve, "pointOnCurve"},
    {ConstraintType::Fix, "fix"},
    {ConstraintType::Distance, "distance"},
    {ConstraintType::DistanceX, "distanceX"},
    {ConstraintType::DistanceY, "distanceY"},
    {ConstraintType::Radius, "radius"},
    {ConstraintType::Diameter, "diameter"},
    {ConstraintType::Angle, "angle"},
};

std::uint64_t low64(const Hash256& h)
{
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | h.bytes[static_cast<std::size_t>(i)];
    }
    return v;
}

Value idValue(const Uuid& id)
{
    return Value::string(id.toString());
}

Result<SketchEntId> entityRef(const PropsReader& r, const Value& item, std::string_view key, const std::string& field)
{
    const Value* v = item.find(key);
    if (v == nullptr) {
        return r.error(field + "." + std::string(key), "is required");
    }
    if (!v->isString()) {
        return r.error(field + "." + std::string(key), "must be an entity id (a lower-case UUID)");
    }
    const Uuid id = Uuid::fromString(v->asString());
    if (id.toString() != v->asString() || id == Uuid{}) {
        return r.error(field + "." + std::string(key), "must be an entity id (a lower-case UUID)");
    }
    return SketchEntId{id};
}

Result<double> finiteNumber(const PropsReader& r, const Value& item, std::string_view key, const std::string& field,
                            bool positive)
{
    const Value* v = item.find(key);
    if (v == nullptr) {
        return r.error(field + "." + std::string(key), "is required");
    }
    if (!v->isNumber() || !std::isfinite(v->asNumber()) || (positive && !(v->asNumber() > 0))) {
        return r.error(field + "." + std::string(key), positive ? "must be a number greater than 0 (mm)"
                                                                : "must be a finite number (mm)");
    }
    return v->asNumber();
}

Result<bool> flag(const PropsReader& r, const Value& item, std::string_view key, const std::string& field,
                  bool fallback)
{
    const Value* v = item.find(key);
    if (v == nullptr) {
        return fallback;
    }
    if (!v->isBool()) {
        return r.error(field + "." + std::string(key), "must be true or false");
    }
    return v->asBool();
}

Result<Entity> readEntity(const PropsReader& r, const Value& item, const std::string& at)
{
    if (!item.isObject()) {
        return r.error(at, "must be a JSON object");
    }
    Entity e;
    auto id = entityRef(r, item, "id", at);
    if (!id) {
        return tl::unexpected(id.error());
    }
    e.id = *id;
    const Value* type = item.find("type");
    if (type == nullptr || !type->isString()) {
        return r.error(at + ".type", "must be one of point, line, arc, circle, spline, slot");
    }
    const std::string& t = type->asString();
    auto construction = flag(r, item, "construction", at, false);
    if (!construction) {
        return tl::unexpected(construction.error());
    }
    e.construction = *construction;
    auto only = [&](std::initializer_list<std::string_view> keys) -> Status {
        for (const Member& m : item.asObject()) {
            if (m.key != "id" && m.key != "type" && m.key != "construction" &&
                std::find(keys.begin(), keys.end(), m.key) == keys.end()) {
                return r.error(at + "." + m.key, "is not a property of a sketch " + t);
            }
        }
        return {};
    };
    if (t == "point") {
        if (auto ok = only({"x", "y"}); !ok) {
            return tl::unexpected(ok.error());
        }
        auto x = finiteNumber(r, item, "x", at, false);
        auto y = finiteNumber(r, item, "y", at, false);
        if (!x || !y) {
            return tl::unexpected(!x ? x.error() : y.error());
        }
        e.geom = Point{Vec2{*x, *y}};
    } else if (t == "line") {
        if (auto ok = only({"a", "b"}); !ok) {
            return tl::unexpected(ok.error());
        }
        auto a = entityRef(r, item, "a", at);
        auto b = entityRef(r, item, "b", at);
        if (!a || !b) {
            return tl::unexpected(!a ? a.error() : b.error());
        }
        e.geom = Line{*a, *b};
    } else if (t == "arc") {
        if (auto ok = only({"center", "start", "end", "ccw"}); !ok) {
            return tl::unexpected(ok.error());
        }
        auto c = entityRef(r, item, "center", at);
        auto s = entityRef(r, item, "start", at);
        auto en = entityRef(r, item, "end", at);
        auto ccw = flag(r, item, "ccw", at, true);
        if (!c || !s || !en || !ccw) {
            return tl::unexpected(!c ? c.error() : !s ? s.error() : !en ? en.error() : ccw.error());
        }
        e.geom = Arc{*c, *s, *en, *ccw};
    } else if (t == "circle") {
        if (auto ok = only({"center", "radius"}); !ok) {
            return tl::unexpected(ok.error());
        }
        auto c = entityRef(r, item, "center", at);
        auto radius = finiteNumber(r, item, "radius", at, true);
        if (!c || !radius) {
            return tl::unexpected(!c ? c.error() : radius.error());
        }
        e.geom = Circle{*c, *radius};
    } else if (t == "spline") {
        if (auto ok = only({"points", "degree", "closed"}); !ok) {
            return tl::unexpected(ok.error());
        }
        Spline sp;
        const Value* pts = item.find("points");
        if (pts == nullptr || !pts->isArray() || pts->asArray().size() < 2) {
            return r.error(at + ".points", "must list at least two point ids");
        }
        for (const Value& p : pts->asArray()) {
            const Uuid pointId = p.isString() ? Uuid::fromString(p.asString()) : Uuid{};
            if (!p.isString() || pointId.toString() != p.asString() || pointId == Uuid{}) {
                return r.error(at + ".points", "must list point ids (lower-case UUIDs)");
            }
            sp.controlPoints.push_back(SketchEntId{pointId});
        }
        const Value* degree = item.find("degree");
        if (degree == nullptr || !degree->isNumber() || degree->asNumber() < 1 || degree->asNumber() > 25 ||
            std::floor(degree->asNumber()) != degree->asNumber()) {
            return r.error(at + ".degree", "must be a whole number from 1 to 25");
        }
        sp.degree = static_cast<int>(degree->asNumber());
        auto closed = flag(r, item, "closed", at, false);
        if (!closed) {
            return tl::unexpected(closed.error());
        }
        sp.closed = *closed;
        e.geom = std::move(sp);
    } else if (t == "slot") {
        if (auto ok = only({"a", "b", "width"}); !ok) {
            return tl::unexpected(ok.error());
        }
        auto a = entityRef(r, item, "a", at);
        auto b = entityRef(r, item, "b", at);
        auto width = finiteNumber(r, item, "width", at, true);
        if (!a || !b || !width) {
            return tl::unexpected(!a ? a.error() : !b ? b.error() : width.error());
        }
        e.geom = Slot{*a, *b, *width};
    } else {
        return r.error(at + ".type", "must be one of point, line, arc, circle, spline, slot");
    }
    return e;
}

Result<Constraint> readConstraint(const PropsReader& r, const Value& item, const std::string& at)
{
    if (!item.isObject()) {
        return r.error(at, "must be a JSON object");
    }
    for (const Member& m : item.asObject()) {
        if (m.key != "id" && m.key != "type" && m.key != "refs" && m.key != "expression" && m.key != "value" &&
            m.key != "driven") {
            return r.error(at + "." + m.key, "is not a property of a sketch constraint");
        }
    }
    Constraint c;
    auto id = entityRef(r, item, "id", at);
    if (!id) {
        return tl::unexpected(id.error());
    }
    c.id = id->value;
    const Value* type = item.find("type");
    const auto parsed = type != nullptr && type->isString() ? constraintTypeFromName(type->asString()) : std::nullopt;
    if (!parsed) {
        std::string list;
        for (const TypeName& n : kConstraintNames) {
            list += list.empty() ? "" : ", ";
            list += n.name;
        }
        return r.error(at + ".type", "must be one of " + list);
    }
    c.type = *parsed;
    const Value* refs = item.find("refs");
    if (refs == nullptr || !refs->isArray() || refs->asArray().empty() || refs->asArray().size() > 3) {
        return r.error(at + ".refs", "must list 1 to 3 entity ids");
    }
    for (const Value& v : refs->asArray()) {
        const Uuid ref = v.isString() ? Uuid::fromString(v.asString()) : Uuid{};
        if (!v.isString() || ref.toString() != v.asString() || ref == Uuid{}) {
            return r.error(at + ".refs", "must list entity ids (lower-case UUIDs)");
        }
        c.refs.push_back(SketchEntId{ref});
    }
    if (const Value* e = item.find("expression")) {
        if (!e->isString() || e->asString().empty()) {
            return r.error(at + ".expression", "must be an expression such as \"12 mm\" or \"width / 2\"");
        }
        if (!isDimension(c.type)) {
            return r.error(at + ".expression", "is only allowed on a dimension (distance, radius, diameter, angle)");
        }
        c.expression = e->asString();
    }
    if (const Value* v = item.find("value")) {
        if (!v->isNumber() || !std::isfinite(v->asNumber())) {
            return r.error(at + ".value", "must be a finite number (mm or radians)");
        }
        c.value = v->asNumber();
    }
    auto driven = flag(r, item, "driven", at, false);
    if (!driven) {
        return tl::unexpected(driven.error());
    }
    c.driven = *driven;
    return c;
}

// Point references of an entity's geometry.
std::vector<SketchEntId> pointRefs(const Entity& e)
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
    if (const auto* s = std::get_if<Spline>(&e.geom)) {
        return s->controlPoints;
    }
    if (const auto* s = std::get_if<Slot>(&e.geom)) {
        return {s->a, s->b};
    }
    return {};
}

} // namespace

const char* constraintTypeName(ConstraintType type)
{
    for (const TypeName& n : kConstraintNames) {
        if (n.type == type) {
            return n.name;
        }
    }
    return "coincident";
}

std::optional<ConstraintType> constraintTypeFromName(std::string_view name)
{
    for (const TypeName& n : kConstraintNames) {
        if (name == n.name) {
            return n.type;
        }
    }
    return std::nullopt;
}

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

std::uint64_t entityCurveTag(const SketchEntId& entity)
{
    Hasher h;
    h.bytes(std::as_bytes(std::span<const std::uint8_t>(entity.value.bytes)));
    return low64(h.finish());
}

std::uint64_t slotPartCurveTag(const SketchEntId& slot, int part)
{
    Hasher h;
    h.bytes(std::as_bytes(std::span<const std::uint8_t>(slot.value.bytes)));
    const auto b = static_cast<std::byte>(part);
    h.bytes(std::span<const std::byte>(&b, 1));
    return low64(h.finish());
}

std::string entityText(const SketchEntId& id)
{
    return id.value.toString();
}

Result<Sketch> sketchFromJson(const PropsReader& r, const Value& value, const std::string& field)
{
    if (!value.isObject()) {
        return r.error(field, "must be a sketch (a JSON object with entities and constraints)");
    }
    const PropsReader s = r.child(value);
    if (auto ok = s.onlyKeys({"constraints", "entities", "schema"}, field + "."); !ok) {
        return tl::unexpected(ok.error());
    }
    if (const Value* schema = value.find("schema")) {
        if (!schema->isNumber() || schema->asNumber() != 1.0) {
            return r.error(field + ".schema", "must be 1 (this version reads sketch schema 1)");
        }
    }
    Sketch out;
    std::set<Uuid> ids;
    if (const Value* entities = value.find("entities")) {
        if (!entities->isArray()) {
            return r.error(field + ".entities", "must be an array");
        }
        for (std::size_t i = 0; i < entities->asArray().size(); ++i) {
            auto e = readEntity(r, entities->asArray()[i], field + ".entities[" + std::to_string(i) + "]");
            if (!e) {
                return tl::unexpected(e.error());
            }
            if (!ids.insert(e->id.value).second) {
                return r.error(field + ".entities[" + std::to_string(i) + "].id",
                               "is used twice (" + e->id.value.toString() + ")");
            }
            out.entities.push_back(std::move(*e));
        }
    }
    std::set<Uuid> points;
    for (const Entity& e : out.entities) {
        if (std::holds_alternative<Point>(e.geom)) {
            points.insert(e.id.value);
        }
    }
    for (const Entity& e : out.entities) {
        for (const SketchEntId& p : pointRefs(e)) {
            if (points.count(p.value) == 0) {
                return r.error(field + ".entities",
                               "the entity " + e.id.value.toString() + " uses " + p.value.toString() +
                                   ", which is not a point of this sketch");
            }
        }
    }
    if (const Value* constraints = value.find("constraints")) {
        if (!constraints->isArray()) {
            return r.error(field + ".constraints", "must be an array");
        }
        for (std::size_t i = 0; i < constraints->asArray().size(); ++i) {
            const std::string at = field + ".constraints[" + std::to_string(i) + "]";
            auto c = readConstraint(r, constraints->asArray()[i], at);
            if (!c) {
                return tl::unexpected(c.error());
            }
            if (!ids.insert(c->id).second) {
                return r.error(at + ".id", "is used twice (" + c->id.toString() + ")");
            }
            for (const SketchEntId& ref : c->refs) {
                const bool entity = std::any_of(out.entities.begin(), out.entities.end(),
                                                [&](const Entity& e) { return e.id == ref; });
                if (!entity) {
                    return r.error(at + ".refs", "names " + ref.value.toString() + ", which is not an entity of "
                                                                                   "this sketch");
                }
            }
            out.constraints.push_back(std::move(*c));
        }
    }
    std::sort(out.entities.begin(), out.entities.end(), [](const Entity& a, const Entity& b) { return a.id < b.id; });
    std::sort(out.constraints.begin(), out.constraints.end(),
              [](const Constraint& a, const Constraint& b) { return a.id < b.id; });
    return out;
}

Value sketchToJson(const Sketch& input)
{
    Sketch s = input;
    std::sort(s.entities.begin(), s.entities.end(), [](const Entity& a, const Entity& b) { return a.id < b.id; });
    std::sort(s.constraints.begin(), s.constraints.end(),
              [](const Constraint& a, const Constraint& b) { return a.id < b.id; });
    Value::Array entities;
    for (const Entity& e : s.entities) {
        Value::Object m{{"id", idValue(e.id.value)}};
        if (e.construction) {
            m.push_back({"construction", Value::boolean(true)});
        }
        if (const auto* pt = std::get_if<Point>(&e.geom)) {
            m.push_back({"type", Value::string("point")});
            m.push_back({"x", Value::number(pt->p.x)});
            m.push_back({"y", Value::number(pt->p.y)});
        } else if (const auto* l = std::get_if<Line>(&e.geom)) {
            m.push_back({"type", Value::string("line")});
            m.push_back({"a", idValue(l->a.value)});
            m.push_back({"b", idValue(l->b.value)});
        } else if (const auto* a = std::get_if<Arc>(&e.geom)) {
            m.push_back({"type", Value::string("arc")});
            m.push_back({"center", idValue(a->center.value)});
            m.push_back({"start", idValue(a->start.value)});
            m.push_back({"end", idValue(a->end.value)});
            m.push_back({"ccw", Value::boolean(a->ccw)});
        } else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            m.push_back({"type", Value::string("circle")});
            m.push_back({"center", idValue(c->center.value)});
            m.push_back({"radius", Value::number(c->radius)});
        } else if (const auto* sp = std::get_if<Spline>(&e.geom)) {
            Value::Array pts;
            for (const SketchEntId& p : sp->controlPoints) {
                pts.push_back(idValue(p.value));
            }
            m.push_back({"type", Value::string("spline")});
            m.push_back({"points", Value::array(std::move(pts))});
            m.push_back({"degree", Value::number(sp->degree)});
            m.push_back({"closed", Value::boolean(sp->closed)});
        } else if (const auto* sl = std::get_if<Slot>(&e.geom)) {
            m.push_back({"type", Value::string("slot")});
            m.push_back({"a", idValue(sl->a.value)});
            m.push_back({"b", idValue(sl->b.value)});
            m.push_back({"width", Value::number(sl->width)});
        }
        entities.push_back(Value::object(std::move(m)));
    }
    Value::Array constraints;
    for (const Constraint& c : s.constraints) {
        Value::Array refs;
        for (const SketchEntId& r : c.refs) {
            refs.push_back(idValue(r.value));
        }
        Value::Object m{{"id", idValue(c.id)},
                        {"type", Value::string(constraintTypeName(c.type))},
                        {"refs", Value::array(std::move(refs))}};
        if (c.expression) {
            m.push_back({"expression", Value::string(*c.expression)});
        }
        if (isDimension(c.type)) {
            m.push_back({"value", Value::number(c.value)});
        }
        if (c.driven) {
            m.push_back({"driven", Value::boolean(true)});
        }
        constraints.push_back(Value::object(std::move(m)));
    }
    return Value::object({{"schema", Value::number(1)},
                          {"entities", Value::array(std::move(entities))},
                          {"constraints", Value::array(std::move(constraints))}});
}

} // namespace ray::doc::features
