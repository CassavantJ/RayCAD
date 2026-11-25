// Reading and writing the props of the "import" feature (import_props.h).
#include "import_props.h"

#include "../json.h"
#include "../json_number.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace ray::doc::features {
namespace {

using json::Member;
using json::Value;

constexpr std::string_view kKnownKeys[] = {"bodies", "fileHash",   "format",      "options",
                                           "role",   "sourceFile", "sourceUnits", "warnings"};
constexpr std::string_view kOptionKeys[] = {"heal", "sew", "sewTolerance"};
constexpr std::string_view kBodyKeys[] = {"blob", "color", "id", "name", "names"};

class Reader {
public:
    explicit Reader(std::string_view where) : where_(where) {}

    tl::unexpected<Error> error(const std::string& field, const std::string& what) const
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, where_ + ": '" + field + "' " + what, {}});
    }
    tl::unexpected<Error> error(const std::string& what) const
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, where_ + ": " + what, {}});
    }

    template <std::size_t N>
    Status onlyKnown(const Value& object, const std::string& prefix, const std::string_view (&known)[N]) const
    {
        for (const Member& member : object.asObject()) {
            if (std::find(std::begin(known), std::end(known), member.key) == std::end(known)) {
                return error(prefix + member.key, "is not a property of an import feature");
            }
        }
        return {};
    }

    Result<std::string> string(const Value& object, std::string_view key, const std::string& field, bool nonEmpty) const
    {
        const Value* value = object.find(key);
        if (value == nullptr) {
            return error(field, "is required");
        }
        if (!value->isString()) {
            return error(field, "must be a string");
        }
        if (nonEmpty && value->asString().empty()) {
            return error(field, "must not be empty");
        }
        return value->asString();
    }

    Result<Hash256> hash(const Value& object, std::string_view key, const std::string& field) const
    {
        auto text = string(object, key, field, true);
        if (!text) {
            return tl::unexpected(text.error());
        }
        auto parsed = parseHashHex(*text);
        if (!parsed) {
            return error(field, "must be a hash: 64 lower-case hex digits");
        }
        return *parsed;
    }

    // One of `names`, as the matching entry of `values`.
    template <class T, std::size_t N>
    Result<std::optional<T>> optEnum(const Value& object, std::string_view key, const std::string& field,
                                     const std::pair<std::string_view, T> (&choices)[N]) const
    {
        const Value* value = object.find(key);
        if (value == nullptr) {
            return std::optional<T>();
        }
        std::string list;
        for (const auto& [name, v] : choices) {
            if (value->isString() && value->asString() == name) {
                return std::optional<T>(v);
            }
            list += list.empty() ? "" : ", ";
            list += name;
        }
        return error(field, "must be one of " + list);
    }

private:
    std::string where_;
};

constexpr std::pair<std::string_view, ImportFormat> kFormats[] = {{"step", ImportFormat::Step},
                                                                  {"iges", ImportFormat::Iges}};
constexpr std::pair<std::string_view, kernel::Heal> kHeals[] = {
    {"none", kernel::Heal::None}, {"reader", kernel::Heal::Reader}, {"full", kernel::Heal::Full}};
constexpr std::pair<std::string_view, Body::Role> kRoles[] = {{"part", Body::Role::Part},
                                                              {"fixture", Body::Role::Fixture}};
constexpr std::pair<std::string_view, UnitSystem> kUnits[] = {{"inch", UnitSystem::Inch},
                                                              {"metric", UnitSystem::Metric}};

template <class T, std::size_t N>
std::string_view nameOf(T v, const std::pair<std::string_view, T> (&choices)[N])
{
    for (const auto& [name, value] : choices) {
        if (value == v) {
            return name;
        }
    }
    return choices[0].first;
}

Status readOptions(const Reader& r, const Value& props, bool strict, kernel::ImportOptions& out)
{
    const Value* options = props.find("options");
    if (options == nullptr) {
        return {};
    }
    if (!options->isObject()) {
        return r.error("options", "must be a JSON object");
    }
    if (strict) {
        if (auto known = r.onlyKnown(*options, "options.", kOptionKeys); !known) {
            return known;
        }
    }
    auto heal = r.optEnum(*options, "heal", "options.heal", kHeals);
    if (!heal) {
        return tl::unexpected(heal.error());
    }
    if (*heal) {
        out.heal = **heal;
    }
    if (const Value* sew = options->find("sew")) {
        if (!sew->isBool()) {
            return r.error("options.sew", "must be true or false");
        }
        out.sew = sew->asBool();
    }
    if (const Value* tolerance = options->find("sewTolerance")) {
        if (!tolerance->isNumber() || !(tolerance->asNumber() > 0.0) || !std::isfinite(tolerance->asNumber())) {
            return r.error("options.sewTolerance", "must be a number greater than 0 (mm)");
        }
        out.sewTolerance = tolerance->asNumber();
    }
    return {};
}

Result<std::array<float, 4>> readColor(const Reader& r, const Value& body, const std::string& field)
{
    const Value* value = body.find("color");
    const std::string shape = "must be an array of 4 numbers from 0 to 1 (r, g, b, a)";
    if (value == nullptr) {
        return r.error(field, "is required");
    }
    if (!value->isArray() || value->asArray().size() != 4) {
        return r.error(field, shape);
    }
    std::array<float, 4> color{};
    for (std::size_t i = 0; i < 4; ++i) {
        const Value& c = value->asArray()[i];
        if (!c.isNumber() || !(c.asNumber() >= 0.0 && c.asNumber() <= 1.0)) {
            return r.error(field, shape);
        }
        // Through the shortest decimal of the double, as body.set does, so the float does not depend on spelling.
        const auto f = detail::parseFloat(detail::formatDouble(c.asNumber()));
        if (!f) {
            return r.error(field, shape);
        }
        color[i] = *f;
    }
    return color;
}

Status readBodies(const Reader& r, const Value& props, bool strict, ImportProps& out)
{
    const Value* bodies = props.find("bodies");
    if (bodies == nullptr) {
        return {};
    }
    if (!bodies->isArray()) {
        return r.error("bodies", "must be an array");
    }
    std::vector<ImportBody> list;
    for (std::size_t i = 0; i < bodies->asArray().size(); ++i) {
        const Value& body = bodies->asArray()[i];
        const std::string at = "bodies[" + std::to_string(i) + "]";
        if (!body.isObject()) {
            return r.error(at, "must be a JSON object");
        }
        if (strict) {
            if (auto known = r.onlyKnown(body, at + ".", kBodyKeys); !known) {
                return known;
            }
        }
        ImportBody b;
        auto id = r.string(body, "id", at + ".id", true);
        if (!id) {
            return tl::unexpected(id.error());
        }
        const Uuid uuid = Uuid::fromString(*id);
        if (uuid.toString() != *id || uuid == Uuid{}) {
            return r.error(at + ".id", "must be a lower-case UUID other than the nil one");
        }
        b.id = BodyId{uuid};
        auto name = r.string(body, "name", at + ".name", true);
        if (!name) {
            return tl::unexpected(name.error());
        }
        b.name = std::move(*name);
        auto color = readColor(r, body, at + ".color");
        if (!color) {
            return tl::unexpected(color.error());
        }
        b.color = *color;
        auto blob = r.hash(body, "blob", at + ".blob");
        if (!blob) {
            return tl::unexpected(blob.error());
        }
        b.blob = *blob;
        if (body.find("names") != nullptr) { // T-084: the names carried over from the revision it replaced
            auto names = r.hash(body, "names", at + ".names");
            if (!names) {
                return tl::unexpected(names.error());
            }
            b.names = *names;
        }
        for (const ImportBody& other : list) {
            if (other.id == b.id) {
                return r.error(at + ".id", "is used by another body of this import");
            }
        }
        list.push_back(std::move(b));
    }
    if (list.empty()) {
        return r.error("bodies", "must list at least one body");
    }
    out.bodies = std::move(list);
    return {};
}

Status readWarnings(const Reader& r, const Value& props, ImportProps& out)
{
    const Value* warnings = props.find("warnings");
    if (warnings == nullptr) {
        return {};
    }
    if (!warnings->isArray()) {
        return r.error("warnings", "must be an array of strings");
    }
    for (const Value& w : warnings->asArray()) {
        if (!w.isString()) {
            return r.error("warnings", "must be an array of strings");
        }
        out.warnings.push_back(w.asString());
    }
    return {};
}

Value colorValue(float c)
{
    const auto d = detail::parseDouble(detail::formatFloat(c));
    return d ? Value::number(*d) : Value::number(0.0);
}

Value text(std::string_view s)
{
    return Value::string(std::string(s));
}

} // namespace

std::optional<Hash256> parseHashHex(std::string_view text)
{
    if (text.size() != 64) {
        return std::nullopt;
    }
    Hash256 hash;
    for (std::size_t i = 0; i < 32; ++i) {
        int byte = 0;
        for (std::size_t k = 0; k < 2; ++k) {
            const char c = text[2 * i + k];
            int v = 0;
            if (c >= '0' && c <= '9') {
                v = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                v = c - 'a' + 10;
            } else {
                return std::nullopt;
            }
            byte = byte * 16 + v;
        }
        hash.bytes[i] = static_cast<std::uint8_t>(byte);
    }
    return hash;
}

std::optional<ImportFormat> formatFromPath(std::string_view path)
{
    const std::string_view name = fileNameOf(path);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    std::string ext(name.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    if (ext == "step" || ext == "stp" || ext == "p21") {
        return ImportFormat::Step;
    }
    if (ext == "iges" || ext == "igs") {
        return ImportFormat::Iges;
    }
    return std::nullopt;
}

std::string_view formatName(ImportFormat format)
{
    return nameOf(format, kFormats);
}

std::string_view fileNameOf(std::string_view path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

Result<ImportProps> parseImportProps(std::string_view propsJson, bool strict, std::string_view where)
{
    try {
        const Reader r(where);
        auto parsed = json::parse(propsJson.empty() ? std::string_view("{}") : propsJson);
        if (!parsed) {
            return r.error("the props are not valid JSON: " + parsed.error().message);
        }
        if (!parsed->isObject()) {
            return r.error("the props must be a JSON object");
        }
        const Value& props = *parsed;
        if (strict) {
            if (auto known = r.onlyKnown(props, "", kKnownKeys); !known) {
                return tl::unexpected(known.error());
            }
        }
        ImportProps out;
        auto source = r.string(props, "sourceFile", "sourceFile", true);
        if (!source) {
            return tl::unexpected(source.error());
        }
        out.sourceFile = std::move(*source);
        auto format = r.optEnum(props, "format", "format", kFormats);
        if (!format) {
            return tl::unexpected(format.error());
        }
        if (!*format) {
            return r.error("format", "is required");
        }
        out.format = **format;
        auto fileHash = r.hash(props, "fileHash", "fileHash");
        if (!fileHash) {
            return tl::unexpected(fileHash.error());
        }
        out.fileHash = *fileHash;
        if (auto options = readOptions(r, props, strict, out.options); !options) {
            return tl::unexpected(options.error());
        }
        auto role = r.optEnum(props, "role", "role", kRoles);
        if (!role) {
            return tl::unexpected(role.error());
        }
        out.role = role->value_or(Body::Role::Part);
        auto units = r.optEnum(props, "sourceUnits", "sourceUnits", kUnits);
        if (!units) {
            return tl::unexpected(units.error());
        }
        out.sourceUnits = *units;
        if (auto bodies = readBodies(r, props, strict, out); !bodies) {
            return tl::unexpected(bodies.error());
        }
        if (auto warnings = readWarnings(r, props, out); !warnings) {
            return tl::unexpected(warnings.error());
        }
        return out;
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, std::string(where) + ": out of memory", {}});
    }
}

std::string writeImportProps(const ImportProps& p)
{
    std::vector<Member> members;
    members.push_back({"fileHash", text(p.fileHash.hex())});
    members.push_back({"format", text(formatName(p.format))});
    members.push_back({"options", Value::object({{"heal", text(nameOf(p.options.heal, kHeals))},
                                                 {"sew", Value::boolean(p.options.sew)},
                                                 {"sewTolerance", Value::number(p.options.sewTolerance)}})});
    members.push_back({"role", text(nameOf(p.role, kRoles))});
    members.push_back({"sourceFile", text(p.sourceFile)});
    if (p.sourceUnits) {
        members.push_back({"sourceUnits", text(nameOf(*p.sourceUnits, kUnits))});
    }
    if (p.bodies) {
        Value::Array bodies;
        for (const ImportBody& b : *p.bodies) {
            Value::Array color;
            for (const float c : b.color) {
                color.push_back(colorValue(c));
            }
            Value::Object body{{"blob", text(b.blob.hex())},
                               {"color", Value::array(std::move(color))},
                               {"id", text(b.id.value.toString())},
                               {"name", text(b.name)}};
            if (b.names) {
                body.push_back({"names", text(b.names->hex())});
            }
            bodies.push_back(Value::object(std::move(body)));
        }
        members.push_back({"bodies", Value::array(std::move(bodies))});
    }
    if (!p.warnings.empty()) {
        Value::Array warnings;
        for (const std::string& w : p.warnings) {
            warnings.push_back(text(w));
        }
        members.push_back({"warnings", Value::array(std::move(warnings))});
    }
    return json::write(Value::object(std::move(members)));
}

std::string writeImportCommandArgs(const FeatureId& feature, const std::string& name, const ImportProps& props,
                                   std::optional<std::size_t> index)
{
    auto propsValue = json::parse(writeImportProps(props));
    std::vector<Member> members;
    members.push_back({"id", text(feature.value.toString())});
    if (index) {
        members.push_back({"index", Value::number(static_cast<double>(*index))});
    }
    members.push_back({"name", text(name)});
    members.push_back({"props", propsValue ? std::move(*propsValue) : Value::object({})});
    return json::write(Value::object(std::move(members)));
}

} // namespace ray::doc::features
