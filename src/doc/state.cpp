// Document snapshots: storage form, parsing back and the state hash (PLAN.md 3.1, 3.2, 3.5, 4.1, 4.4).
//
// A DocState is stored as one manifest object plus one object per feature, all canonical JSON (json.h):
//
//   manifest  {"bodies":[Body...],"features":["<feature object hash>"...],"parameters":[Parameter...],
//              "rollback"?:"<feature uuid>","schema":1,"settings":Settings, ...unknown keys}
//   ("rollback", the rollback bar, only while it is set; an older RayCAD keeps it as an unknown key and builds the
//   whole history.)
//   Settings  {"decimals":4,"material":"<uuid>"|null,"units":"inch"|"metric", ...}
//   Parameter {"expression":"4 in","id":"<uuid>","kind":"length"|"angle"|"count"|"unitless","name":"width", ...}
//   Body      {"color":[r,g,b,a],"id":"<uuid>","name":"Bracket","role":"part"|"fixture"|"stock_model",
//              "visible":true, ...}
//   feature   {"id":"<uuid>","name":"Extrude 1","props":{...},"schema":1,"suppressed":false,"type":"extrude", ...}
//
// Hashes are BLAKE3-256 of the canonical bytes, written as 64 lower-case hex digits; ids are lower-case
// 8-4-4-4-12 UUIDs; the nil material id is written as null. Colours are floats written with the shortest decimal
// that reads back to the same float (0.72, not 0.7200000286102295) and read back directly to float precision.
// Feature props are embedded as JSON; an empty propsJson is written as {}. Keys this version does not know are
// kept in the extraJson member of the struct they belong to and written back unchanged; a known key in
// extraJson is ignored on writing. A state with CAM state (DocState::cam, not empty) also has the CAM object and one
// object per embedded library item, named in the manifest's "cam" and "embedded" keys and written after the feature
// objects (commands/cam_json.h has their form, task T-032); a state without CAM state is written exactly as before,
// and a manifest without those keys (a document from before CAM) reads back with a null DocState::cam.
// The schemas schemas/doc.manifest.schema.json, schemas/doc.feature.schema.json and schemas/doc.cam.schema.json
// describe the objects.
#include "commands/cam_json.h"
#include "document_impl.h"
#include "json.h"
#include "json_number.h"

#include <ray/cam.h>
#include <ray/document.h>

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <new>
#include <set>

namespace ray::doc {
namespace {

using json::Member;
using json::Value;

constexpr int kSchema = 1;

// ---------- Writing ----------

Value text(std::string s)
{
    return Value::string(std::move(s));
}

Value idValue(const Uuid& id)
{
    return text(id.toString());
}

// The known members plus the unknown ones kept from reading (known keys win).
Value objectWith(std::vector<Member> known, const std::string& extraJson)
{
    if (!extraJson.empty()) {
        auto extra = json::parse(extraJson);
        if (extra && extra->isObject()) {
            for (const Member& member : extra->asObject()) {
                const bool clash =
                    std::any_of(known.begin(), known.end(), [&](const Member& k) { return k.key == member.key; });
                if (!clash) {
                    known.push_back(member);
                }
            }
        }
    }
    return Value::object(std::move(known));
}

const char* kindText(Parameter::Kind kind)
{
    switch (kind) {
    case Parameter::Kind::Length:
        return "length";
    case Parameter::Kind::Angle:
        return "angle";
    case Parameter::Kind::Count:
        return "count";
    case Parameter::Kind::Unitless:
        return "unitless";
    }
    return "unitless";
}

const char* roleText(Body::Role role)
{
    switch (role) {
    case Body::Role::Part:
        return "part";
    case Body::Role::Fixture:
        return "fixture";
    case Body::Role::StockModel:
        return "stock_model";
    }
    return "part";
}

Value colorValue(float c)
{
    // The double nearest to the float's shortest decimal; the canonical writer prints that decimal again.
    const auto d = detail::parseDouble(detail::formatFloat(c));
    return d ? Value::number(*d) : Value();
}

std::string featureObjectBytes(const Feature& f)
{
    Value props = Value::object({});
    if (!f.propsJson.empty()) {
        auto parsed = json::parse(f.propsJson);
        props = parsed ? std::move(*parsed) : Value();
    }
    std::vector<Member> members;
    members.push_back({"id", idValue(f.id.value)});
    members.push_back({"name", text(f.name)});
    members.push_back({"props", std::move(props)});
    members.push_back({"schema", Value::number(kSchema)});
    members.push_back({"suppressed", Value::boolean(f.suppressed)});
    members.push_back({"type", text(f.type)});
    return json::write(objectWith(std::move(members), f.extraJson));
}

Hash256 hashOf(std::string_view bytes)
{
    Hasher hasher;
    hasher.bytes(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    return hasher.finish();
}

StateObjects serialize(const DocState& state)
{
    StateObjects out;
    Value::Array featureHashes;
    for (const Feature& feature : state.features) {
        std::string bytes = featureObjectBytes(feature);
        const Hash256 hash = hashOf(bytes);
        featureHashes.push_back(text(hash.hex()));
        out.features.emplace_back(hash, std::move(bytes));
    }
    Value::Array params;
    for (const Parameter& p : state.parameters) {
        params.push_back(objectWith({{"expression", text(p.expression)},
                                     {"id", idValue(p.id.value)},
                                     {"kind", text(kindText(p.kind))},
                                     {"name", text(p.name)}},
                                    p.extraJson));
    }
    Value::Array bodies;
    for (const Body& b : state.bodies) {
        Value::Array color;
        for (const float c : b.color) {
            color.push_back(colorValue(c));
        }
        bodies.push_back(objectWith({{"color", Value::array(std::move(color))},
                                     {"id", idValue(b.id.value)},
                                     {"name", text(b.name)},
                                     {"role", text(roleText(b.role))},
                                     {"visible", Value::boolean(b.visible)}},
                                    b.extraJson));
    }
    const DocSettings& s = state.settings;
    Value settings = objectWith({{"decimals", Value::number(s.decimals)},
                                 {"material", s.material.value == Uuid{} ? Value() : idValue(s.material.value)},
                                 {"units", text(s.units == UnitSystem::Inch ? "inch" : "metric")}},
                                s.extraJson);
    std::vector<Member> members{{"bodies", Value::array(std::move(bodies))},
                                {"features", Value::array(std::move(featureHashes))},
                                {"parameters", Value::array(std::move(params))}};
    // CAM state ("cam", "embedded") only when there is some, so a document without it keeps its bytes and hash.
    for (Member& m : camjson::writeCam(state.cam, out.features)) {
        members.push_back(std::move(m));
    }
    // The rollback bar only when it is set, so a document that never used it keeps its bytes and hash.
    if (state.rollback) {
        members.push_back({"rollback", idValue(state.rollback->value)});
    }
    members.push_back({"schema", Value::number(kSchema)});
    members.push_back({"settings", std::move(settings)});
    Value manifest = objectWith(std::move(members), state.extraJson);
    out.manifest = json::write(manifest);
    return out;
}

// ---------- Reading ----------

tl::unexpected<Error> formatError(const std::string& where, const std::string& what)
{
    return tl::unexpected(Error{ErrorCode::FormatError, "Invalid document: " + where + ": " + what, {}});
}

const char* typeName(Value::Type type)
{
    switch (type) {
    case Value::Type::Null:
        return "null";
    case Value::Type::Bool:
        return "a boolean";
    case Value::Type::Number:
        return "a number";
    case Value::Type::String:
        return "a string";
    case Value::Type::Array:
        return "an array";
    case Value::Type::Object:
        return "an object";
    }
    return "a value";
}

Result<const Value*> field(const Value& object, std::string_view key, Value::Type type, const std::string& where)
{
    const Value* value = object.find(key);
    if (value == nullptr) {
        return formatError(where, "missing key '" + std::string(key) + "'");
    }
    if (value->type() != type) {
        return formatError(where, "'" + std::string(key) + "' must be " + typeName(type));
    }
    return value;
}

// The members of `object` whose keys are not in `known`, as canonical JSON, or empty if there are none.
std::string extrasOf(const Value& object, std::initializer_list<std::string_view> known)
{
    Value::Object extra;
    for (const Member& member : object.asObject()) {
        if (std::find(known.begin(), known.end(), member.key) == known.end()) {
            extra.push_back(member);
        }
    }
    return extra.empty() ? std::string() : json::write(Value::object(std::move(extra)));
}

Result<Uuid> readId(const Value& object, std::string_view key, const std::string& where)
{
    auto value = field(object, key, Value::Type::String, where);
    if (!value) {
        return tl::unexpected(value.error());
    }
    const std::string& s = (*value)->asString();
    const Uuid id = Uuid::fromString(s);
    if (id.toString() != s) {
        return formatError(where, "'" + std::string(key) + "' is not a lower-case UUID");
    }
    return id;
}

Result<std::string> readString(const Value& object, std::string_view key, const std::string& where)
{
    auto value = field(object, key, Value::Type::String, where);
    if (!value) {
        return tl::unexpected(value.error());
    }
    return (*value)->asString();
}

Result<bool> readBool(const Value& object, std::string_view key, const std::string& where)
{
    auto value = field(object, key, Value::Type::Bool, where);
    if (!value) {
        return tl::unexpected(value.error());
    }
    return (*value)->asBool();
}

Result<int> readInt(const Value& object, std::string_view key, const std::string& where)
{
    auto value = field(object, key, Value::Type::Number, where);
    if (!value) {
        return tl::unexpected(value.error());
    }
    const double d = (*value)->asNumber();
    if (!(d >= -2147483648.0 && d <= 2147483647.0) || static_cast<double>(static_cast<int>(d)) != d) {
        return formatError(where, "'" + std::string(key) + "' must be a whole number");
    }
    return static_cast<int>(d);
}

// Checks "schema": FormatTooNew for a newer one.
Status readSchema(const Value& object, const std::string& where)
{
    auto schema = readInt(object, "schema", where);
    if (!schema) {
        return tl::unexpected(schema.error());
    }
    if (*schema > kSchema) {
        return tl::unexpected(Error{ErrorCode::FormatTooNew,
                                    "This document needs a newer RayCAD: " + where + " has format " +
                                        std::to_string(*schema) + ", this version reads format " +
                                        std::to_string(kSchema) + ".",
                                    {}});
    }
    if (*schema < kSchema) {
        return formatError(where, "unknown schema " + std::to_string(*schema));
    }
    return {};
}

template <class Enum, std::size_t N>
Result<Enum> readEnum(const Value& object, std::string_view key, const std::string& where,
                      const std::pair<const char*, Enum> (&names)[N])
{
    auto s = readString(object, key, where);
    if (!s) {
        return tl::unexpected(s.error());
    }
    std::string allowed;
    for (const auto& [name, value] : names) {
        if (*s == name) {
            return value;
        }
        allowed += (allowed.empty() ? "" : ", ") + std::string(name);
    }
    return formatError(where, "'" + std::string(key) + "' must be one of " + allowed);
}

constexpr std::pair<const char*, Parameter::Kind> kKinds[] = {{"length", Parameter::Kind::Length},
                                                              {"angle", Parameter::Kind::Angle},
                                                              {"count", Parameter::Kind::Count},
                                                              {"unitless", Parameter::Kind::Unitless}};
constexpr std::pair<const char*, Body::Role> kRoles[] = {
    {"part", Body::Role::Part}, {"fixture", Body::Role::Fixture}, {"stock_model", Body::Role::StockModel}};
constexpr std::pair<const char*, UnitSystem> kUnitSystems[] = {{"inch", UnitSystem::Inch},
                                                               {"metric", UnitSystem::Metric}};

Result<Hash256> parseHash(const Value& value, const std::string& where)
{
    if (!value.isString() || value.asString().size() != 64) {
        return formatError(where, "expected a hash of 64 lower-case hex digits");
    }
    Hash256 hash;
    const std::string& s = value.asString();
    for (std::size_t i = 0; i < 64; ++i) {
        const char c = s[i];
        int digit = 0;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else {
            return formatError(where, "expected a hash of 64 lower-case hex digits");
        }
        hash.bytes[i / 2] = static_cast<std::uint8_t>(hash.bytes[i / 2] * 16 + digit);
    }
    return hash;
}

Result<Feature> readFeature(std::string_view bytes, const std::string& where)
{
    auto parsed = json::parse(bytes);
    if (!parsed) {
        return formatError(where, parsed.error().message);
    }
    const Value& object = *parsed;
    if (!object.isObject()) {
        return formatError(where, "must be an object");
    }
    if (auto schema = readSchema(object, where); !schema) {
        return tl::unexpected(schema.error());
    }
    Feature f;
    auto id = readId(object, "id", where);
    auto type = readString(object, "type", where);
    auto name = readString(object, "name", where);
    auto suppressed = readBool(object, "suppressed", where);
    if (!id || !type || !name || !suppressed) {
        return tl::unexpected(!id ? id.error() : !type ? type.error() : !name ? name.error() : suppressed.error());
    }
    const Value* props = object.find("props");
    if (props == nullptr) {
        return formatError(where, "missing key 'props'");
    }
    f.id = FeatureId{*id};
    f.type = std::move(*type);
    f.name = std::move(*name);
    f.propsJson = json::write(*props);
    f.suppressed = *suppressed;
    f.extraJson = extrasOf(object, {"id", "name", "props", "schema", "suppressed", "type"});
    return f;
}

Result<float> readColorComponent(const Value& value, const std::string& where)
{
    if (!value.isNumber()) {
        return formatError(where, "colour components must be numbers");
    }
    const std::string digits = value.numberText().empty() ? detail::formatDouble(value.asNumber()) : value.numberText();
    const auto f = detail::parseFloat(digits);
    if (!f) {
        return formatError(where, "colour component out of range");
    }
    return *f;
}

Result<DocState> deserialize(std::string_view manifestBytes,
                             const std::function<Result<std::string>(const Hash256&)>& featureObject)
{
    auto parsed = json::parse(manifestBytes);
    if (!parsed) {
        return formatError("manifest", parsed.error().message);
    }
    const Value& manifest = *parsed;
    if (!manifest.isObject()) {
        return formatError("manifest", "must be an object");
    }
    if (auto schema = readSchema(manifest, "manifest"); !schema) {
        return tl::unexpected(schema.error());
    }
    DocState state;
    state.extraJson =
        extrasOf(manifest, {"bodies", "cam", "embedded", "features", "parameters", "rollback", "schema", "settings"});

    // Settings.
    {
        const std::string where = "settings";
        auto object = field(manifest, "settings", Value::Type::Object, "manifest");
        if (!object) {
            return tl::unexpected(object.error());
        }
        const Value& s = **object;
        auto units = readEnum(s, "units", where, kUnitSystems);
        auto decimals = readInt(s, "decimals", where);
        if (!units || !decimals) {
            return tl::unexpected(!units ? units.error() : decimals.error());
        }
        const Value* material = s.find("material");
        if (material == nullptr) {
            return formatError(where, "missing key 'material'");
        }
        if (!material->isNull()) {
            auto id = readId(s, "material", where);
            if (!id) {
                return tl::unexpected(id.error());
            }
            state.settings.material = MaterialId{*id};
        }
        state.settings.units = *units;
        state.settings.decimals = *decimals;
        state.settings.extraJson = extrasOf(s, {"decimals", "material", "units"});
    }

    // Parameters.
    auto params = field(manifest, "parameters", Value::Type::Array, "manifest");
    if (!params) {
        return tl::unexpected(params.error());
    }
    std::set<Uuid> seen;
    for (const Value& p : (*params)->asArray()) {
        const std::string where = "parameters[" + std::to_string(state.parameters.size()) + "]";
        if (!p.isObject()) {
            return formatError(where, "must be an object");
        }
        auto id = readId(p, "id", where);
        auto name = readString(p, "name", where);
        auto expression = readString(p, "expression", where);
        auto kind = readEnum(p, "kind", where, kKinds);
        if (!id || !name || !expression || !kind) {
            return tl::unexpected(!id           ? id.error()
                                  : !name       ? name.error()
                                  : !expression ? expression.error()
                                                : kind.error());
        }
        if (!seen.insert(*id).second) {
            return formatError(where, "duplicate id " + id->toString());
        }
        Parameter param;
        param.id = ParamId{*id};
        param.name = std::move(*name);
        param.expression = std::move(*expression);
        param.kind = *kind;
        param.extraJson = extrasOf(p, {"expression", "id", "kind", "name"});
        state.parameters.push_back(std::move(param));
    }

    // Features, by hash.
    auto features = field(manifest, "features", Value::Type::Array, "manifest");
    if (!features) {
        return tl::unexpected(features.error());
    }
    seen.clear();
    for (const Value& h : (*features)->asArray()) {
        const std::string where = "features[" + std::to_string(state.features.size()) + "]";
        auto hash = parseHash(h, where);
        if (!hash) {
            return tl::unexpected(hash.error());
        }
        auto bytes = featureObject(*hash);
        if (!bytes) {
            Error error = bytes.error();
            error.context.push_back("while reading feature object " + hash->hex());
            return tl::unexpected(error);
        }
        if (hashOf(*bytes) != *hash) {
            return formatError(where, "the feature object does not match its hash " + hash->hex());
        }
        auto feature = readFeature(*bytes, "feature " + hash->hex());
        if (!feature) {
            return tl::unexpected(feature.error());
        }
        if (!seen.insert(feature->id.value).second) {
            return formatError(where, "duplicate id " + feature->id.value.toString());
        }
        state.features.push_back(std::move(*feature));
    }

    // The rollback bar: a feature of this document other than the last (anything else means the whole history).
    if (manifest.find("rollback") != nullptr) {
        auto id = readId(manifest, "rollback", "manifest");
        if (!id) {
            return tl::unexpected(id.error());
        }
        state.rollback = FeatureId{*id};
        detail::normalizeRollback(state);
    }

    // Bodies.
    auto bodies = field(manifest, "bodies", Value::Type::Array, "manifest");
    if (!bodies) {
        return tl::unexpected(bodies.error());
    }
    seen.clear();
    for (const Value& b : (*bodies)->asArray()) {
        const std::string where = "bodies[" + std::to_string(state.bodies.size()) + "]";
        if (!b.isObject()) {
            return formatError(where, "must be an object");
        }
        auto id = readId(b, "id", where);
        auto name = readString(b, "name", where);
        auto role = readEnum(b, "role", where, kRoles);
        auto visible = readBool(b, "visible", where);
        auto color = field(b, "color", Value::Type::Array, where);
        if (!id || !name || !role || !visible || !color) {
            return tl::unexpected(!id        ? id.error()
                                  : !name    ? name.error()
                                  : !role    ? role.error()
                                  : !visible ? visible.error()
                                             : color.error());
        }
        if (!seen.insert(*id).second) {
            return formatError(where, "duplicate id " + id->toString());
        }
        const Value::Array& components = (*color)->asArray();
        if (components.size() != 4) {
            return formatError(where, "'color' must have 4 components (r, g, b, a)");
        }
        Body body;
        for (std::size_t i = 0; i < 4; ++i) {
            auto c = readColorComponent(components[i], where);
            if (!c) {
                return tl::unexpected(c.error());
            }
            body.color[i] = *c;
        }
        body.id = BodyId{*id};
        body.name = std::move(*name);
        body.role = *role;
        body.visible = *visible;
        body.extraJson = extrasOf(b, {"color", "id", "name", "role", "visible"});
        state.bodies.push_back(std::move(body));
    }

    // CAM state (task T-032): null when the manifest names none.
    auto cam = camjson::readCam(manifest, featureObject);
    if (!cam) {
        return tl::unexpected(cam.error());
    }
    state.cam = std::move(*cam);
    return state;
}

} // namespace

// ---------- The rollback bar ----------
namespace detail {

std::optional<std::size_t> rollbackIndex(const DocState& state)
{
    if (!state.rollback) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < state.features.size(); ++i) {
        if (state.features[i].id == *state.rollback) {
            return i + 1 < state.features.size() ? std::optional<std::size_t>(i) : std::nullopt;
        }
    }
    return std::nullopt;
}

void normalizeRollback(DocState& state)
{
    if (!rollbackIndex(state)) {
        state.rollback.reset();
    }
}

std::size_t defaultInsertIndex(const DocState& state)
{
    const auto at = rollbackIndex(state);
    return at ? *at + 1 : state.features.size();
}

void insertFeature(DocState& state, std::size_t at, Feature feature)
{
    const auto bar = rollbackIndex(state);
    at = std::min(at, state.features.size());
    const FeatureId id = feature.id;
    state.features.insert(state.features.begin() + static_cast<std::ptrdiff_t>(at), std::move(feature));
    if (bar && at == *bar + 1) {
        state.rollback = id;
    }
    normalizeRollback(state);
}

void eraseFeature(DocState& state, std::size_t at)
{
    if (at >= state.features.size()) {
        return;
    }
    if (state.rollback && state.features[at].id == *state.rollback) {
        if (at > 0) {
            state.rollback = state.features[at - 1].id;
        } else {
            state.rollback.reset();
        }
    }
    state.features.erase(state.features.begin() + static_cast<std::ptrdiff_t>(at));
    normalizeRollback(state);
}

} // namespace detail

StateObjects serializeState(const DocState& state)
{
    try {
        return serialize(state);
    } catch (...) {
        // Only std::bad_alloc can get here. These bytes are hashed and stored, so a partial result is never
        // returned.
        std::terminate();
    }
}

Result<DocState> deserializeState(std::string_view manifest,
                                  const std::function<Result<std::string>(const Hash256&)>& featureObject)
{
    try {
        return deserialize(manifest, featureObject);
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::FormatError, "Invalid document: out of memory while reading", {}});
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Unexpected failure while reading a document", {}});
    }
}

Hash256 DocState::stateHash() const
{
    return hashOf(serializeState(*this).manifest);
}

} // namespace ray::doc
