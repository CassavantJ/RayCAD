// Operation parameters read against the type's schema (op_params.h).
#include "op_params.h"

#include <cmath>
#include <exception>
#include <new>
#include <utility>

namespace ray::cam::ops {

namespace detail {
// src/cam/CMakeLists.txt generates these from schemas/ops/*.schema.json.
struct EmbeddedOpSchema {
    const char* name;
    const unsigned char* data;
    std::size_t size;
};
extern const EmbeddedOpSchema kEmbeddedOpSchemas[];
extern const std::size_t kEmbeddedOpSchemaCount;
} // namespace detail

std::string_view embeddedOpSchema(std::string_view name)
{
    for (std::size_t i = 0; i < detail::kEmbeddedOpSchemaCount; ++i) {
        const detail::EmbeddedOpSchema& e = detail::kEmbeddedOpSchemas[i];
        if (name == e.name) {
            return std::string_view(reinterpret_cast<const char*>(e.data), e.size);
        }
    }
    return {};
}

namespace {

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

std::string numberText(double v)
{
    return json::write(json::Value::number(v));
}

const char* typeWord(json::Value::Type t)
{
    switch (t) {
    case json::Value::Type::Null:
        return "null";
    case json::Value::Type::Bool:
        return "true or false";
    case json::Value::Type::Number:
        return "a number";
    case json::Value::Type::String:
        return "text";
    case json::Value::Type::Array:
        return "a list";
    case json::Value::Type::Object:
        return "an object";
    }
    return "a value";
}

// Ok, or the problem with `v` as a value of `p` ("must be ...").
std::string problemOf(const OpSchema::Property& p, const json::Value& v)
{
    using Kind = OpSchema::Property::Kind;
    switch (p.kind) {
    case Kind::Boolean:
        return v.isBool() ? std::string() : std::string("must be true or false, not ") + typeWord(v.type());
    case Kind::String:
        return v.isString() ? std::string() : std::string("must be text, not ") + typeWord(v.type());
    case Kind::Enum: {
        const json::Value* options = p.schema.find("enum");
        std::string list;
        for (const json::Value& o : options->asArray()) { // enums of text only (op_params.h)
            if (o.isString() && v.isString() && o.asString() == v.asString()) {
                return {};
            }
            list += (list.empty() ? "'" : ", '") + (o.isString() ? o.asString() : json::write(o)) + "'";
        }
        return "must be one of " + list;
    }
    case Kind::Number:
    case Kind::Integer:
        break;
    }
    if (!v.isNumber()) {
        return std::string("must be a number, not ") + typeWord(v.type());
    }
    const double d = v.asNumber();
    if (p.kind == Kind::Integer && std::floor(d) != d) {
        return "must be a whole number, not " + numberText(d);
    }
    const auto limit = [&](const char* key) -> const json::Value* {
        const json::Value* l = p.schema.find(key);
        return l != nullptr && l->isNumber() ? l : nullptr;
    };
    if (const json::Value* l = limit("minimum"); l != nullptr && d < l->asNumber()) {
        return "must be at least " + numberText(l->asNumber()) + ", not " + numberText(d);
    }
    if (const json::Value* l = limit("exclusiveMinimum"); l != nullptr && d <= l->asNumber()) {
        return "must be more than " + numberText(l->asNumber()) + ", not " + numberText(d);
    }
    if (const json::Value* l = limit("maximum"); l != nullptr && d > l->asNumber()) {
        return "must be at most " + numberText(l->asNumber()) + ", not " + numberText(d);
    }
    if (const json::Value* l = limit("exclusiveMaximum"); l != nullptr && d >= l->asNumber()) {
        return "must be less than " + numberText(l->asNumber()) + ", not " + numberText(d);
    }
    return {};
}

std::string nameOf(const OpSchema::Property& p)
{
    return p.title.empty() ? "'" + p.key + "'" : p.title + " ('" + p.key + "')";
}

} // namespace

Result<std::shared_ptr<const OpSchema>> OpSchema::load(std::string_view schemaJson)
{
    try {
        auto parsed = json::parse(schemaJson);
        if (!parsed) {
            return failure(ErrorCode::FormatError, "Operation schema: " + parsed.error().message);
        }
        const json::Value* props = parsed->find("properties");
        if (props == nullptr || !props->isObject()) {
            return failure(ErrorCode::FormatError, "Operation schema: no \"properties\" object");
        }
        auto schema = std::make_shared<OpSchema>();
        for (const json::Member& m : props->asObject()) {
            Property p;
            p.key = m.key;
            p.schema = m.value;
            if (const json::Value* t = m.value.find("title"); t != nullptr && t->isString()) {
                p.title = t->asString();
            }
            const json::Value* type = m.value.find("type");
            const json::Value* options = m.value.find("enum");
            if (options != nullptr) {
                if (!options->isArray() || options->asArray().empty()) {
                    return failure(ErrorCode::FormatError, "Operation schema: '" + m.key + "' has an empty enum");
                }
                p.kind = Property::Kind::Enum;
            } else if (type != nullptr && type->isString() && type->asString() == "number") {
                p.kind = Property::Kind::Number;
            } else if (type != nullptr && type->isString() && type->asString() == "integer") {
                p.kind = Property::Kind::Integer;
            } else if (type != nullptr && type->isString() && type->asString() == "boolean") {
                p.kind = Property::Kind::Boolean;
            } else if (type != nullptr && type->isString() && type->asString() == "string") {
                p.kind = Property::Kind::String;
            } else {
                return failure(ErrorCode::FormatError, "Operation schema: '" + m.key +
                                                           "' needs a type of number, integer, boolean or string "
                                                           "or an enum");
            }
            const json::Value* fallback = m.value.find("default");
            if (fallback == nullptr) {
                return failure(ErrorCode::FormatError, "Operation schema: '" + m.key + "' has no default");
            }
            p.fallback = *fallback;
            if (const std::string problem = problemOf(p, p.fallback); !problem.empty()) {
                return failure(ErrorCode::FormatError, "Operation schema: the default of '" + m.key + "' " + problem);
            }
            schema->properties_.emplace(p.key, std::move(p));
        }
        return std::shared_ptr<const OpSchema>(std::move(schema));
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while reading an operation schema");
    }
}

Result<OpParams> OpSchema::read(std::string_view paramsJson) const
{
    try {
        json::Value given = json::Value::object({});
        if (!paramsJson.empty()) {
            auto parsed = json::parse(paramsJson);
            if (!parsed) {
                return failure(ErrorCode::InvalidArgument,
                               "The operation's parameters are not valid JSON: " + parsed.error().message);
            }
            if (!parsed->isObject()) {
                return failure(ErrorCode::InvalidArgument, "The operation's parameters must be a JSON object");
            }
            given = std::move(*parsed);
        }
        for (const json::Member& m : given.asObject()) {
            if (properties_.count(m.key) == 0) {
                return failure(ErrorCode::InvalidArgument,
                               "Unknown parameter '" + m.key +
                                   "' (from a newer RayCAD?): this version cannot honour it; remove it to go on");
            }
        }
        OpParams out;
        for (const auto& [key, p] : properties_) {
            OpParams::Entry e;
            if (const json::Value* v = given.find(key)) {
                if (const std::string problem = problemOf(p, *v); !problem.empty()) {
                    return failure(ErrorCode::InvalidArgument, nameOf(p) + " " + problem);
                }
                e.value = *v;
                e.given = true;
            } else {
                e.value = p.fallback;
            }
            out.values_.emplace(key, std::move(e));
        }
        return out;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while reading the operation's parameters");
    }
}

double OpParams::number(std::string_view key) const
{
    const auto it = values_.find(key);
    return it != values_.end() && it->second.value.isNumber() ? it->second.value.asNumber() : 0.0;
}

bool OpParams::flag(std::string_view key) const
{
    const auto it = values_.find(key);
    return it != values_.end() && it->second.value.isBool() && it->second.value.asBool();
}

const std::string& OpParams::text(std::string_view key) const
{
    static const std::string kEmpty;
    const auto it = values_.find(key);
    return it != values_.end() && it->second.value.isString() ? it->second.value.asString() : kEmpty;
}

bool OpParams::given(std::string_view key) const
{
    const auto it = values_.find(key);
    return it != values_.end() && it->second.given;
}

} // namespace ray::cam::ops
