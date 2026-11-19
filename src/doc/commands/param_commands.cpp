// param.rename and param.remove (task T-098: the parameter table). Pure functions of (snapshot, canonical arguments),
// like the other model commands (model_commands.cpp).
//
//   param.rename  {"id", "name"}   renames a parameter and, in the same step, every use of the old name: the
//                 expressions of the other parameters, the numeric props of the features (the props their type's
//                 schema marks with "x-unit") and the dimension expressions of sketches. The new name must be a valid,
//                 unreserved and unused identifier.
//   param.remove  {"id"}           deletes a parameter that nothing uses; when a parameter or a feature uses it, it
//                 fails with a message that names them.
// A use is the name as a whole identifier in an expression ("width * 2" uses width, "widths" and "2width" do not).
#include "args.h"
#include "commands.h"

#include "../expr.h"
#include "../json.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ray::doc::commands {
namespace {

using json::Value;

#define RAY_DOC_TRY(var, expr)                                                                                         \
    auto var = (expr);                                                                                                 \
    if (!var) {                                                                                                        \
        return tl::unexpected(var.error());                                                                            \
    }

constexpr std::string_view kParamRename = "param.rename";
constexpr std::string_view kParamRemove = "param.remove";

bool identStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool identChar(char c)
{
    return identStart(c) || (c >= '0' && c <= '9');
}
bool digit(char c)
{
    return c >= '0' && c <= '9';
}

// The expression with every identifier `from` replaced by `to` (to empty: unchanged); `found` counts them.
std::string renameIn(std::string_view text, std::string_view from, std::string_view to, int& found)
{
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (digit(c) || (c == '.' && i + 1 < text.size() && digit(text[i + 1]))) {
            // A number, with its exponent ("2e5" is a number, not 2 and the name e5).
            const std::size_t start = i;
            while (i < text.size() && (digit(text[i]) || text[i] == '.')) {
                ++i;
            }
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                std::size_t j = i + 1;
                if (j < text.size() && (text[j] == '+' || text[j] == '-')) {
                    ++j;
                }
                if (j < text.size() && digit(text[j])) {
                    i = j;
                    while (i < text.size() && digit(text[i])) {
                        ++i;
                    }
                }
            }
            out.append(text.substr(start, i - start));
        } else if (identStart(c)) {
            const std::size_t start = i;
            while (i < text.size() && identChar(text[i])) {
                ++i;
            }
            const std::string_view name = text.substr(start, i - start);
            if (name == from) {
                ++found;
                out.append(to.empty() ? name : to);
            } else {
                out.append(name);
            }
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

// Walks a props value along its schema and renames in every expression string (schema "x-unit"); `fields` collects
// the paths of the expressions that use the name.
struct Walker {
    std::string_view from, to;
    std::vector<std::string> fields;

    Value walk(const Value& value, const Value* schema, const std::string& path)
    {
        if (value.isString()) {
            if (schema != nullptr && schema->find("x-unit") != nullptr) {
                int found = 0;
                std::string renamed = renameIn(value.asString(), from, to, found);
                if (found > 0) {
                    fields.push_back(path);
                    return Value::string(std::move(renamed));
                }
            }
            return value;
        }
        if (value.isArray()) {
            const Value* items = schema != nullptr ? schema->find("items") : nullptr;
            Value::Array out;
            for (std::size_t i = 0; i < value.asArray().size(); ++i) {
                out.push_back(walk(value.asArray()[i], items, path + "[" + std::to_string(i) + "]"));
            }
            return Value::array(std::move(out));
        }
        if (value.isObject()) {
            const Value* properties = schema != nullptr ? schema->find("properties") : nullptr;
            Value::Object out;
            for (const json::Member& m : value.asObject()) {
                const Value* sub = properties != nullptr ? properties->find(m.key) : nullptr;
                // Sketch dimensions keep their expression in "expression" (no "x-unit" in the sketch schema).
                if (m.key == "expression" && m.value.isString()) {
                    int found = 0;
                    std::string renamed = renameIn(m.value.asString(), from, to, found);
                    if (found > 0) {
                        fields.push_back(path.empty() ? m.key : path + "." + m.key);
                        out.push_back({m.key, Value::string(std::move(renamed))});
                        continue;
                    }
                }
                out.push_back({m.key, walk(m.value, sub, path.empty() ? m.key : path + "." + m.key)});
            }
            return Value::object(std::move(out));
        }
        return value;
    }
};

struct Use {
    std::string who; // "parameter depth", "Extrude 1 (distance)"
};

// Every use of `name` in `state` (other parameters and features); with `to`, the renamed state too.
Result<std::vector<Use>> usesOf(const DocState& state, const ParamId& self, const std::string& name, DocState* renamed,
                                const std::string& to)
{
    std::vector<Use> uses;
    const auto registry = builtinFeatureRegistry();
    for (std::size_t i = 0; i < state.parameters.size(); ++i) {
        const Parameter& p = state.parameters[i];
        if (p.id == self) {
            continue;
        }
        int found = 0;
        std::string text = renameIn(p.expression, name, to, found);
        if (found > 0) {
            uses.push_back(Use{"parameter " + p.name});
            if (renamed != nullptr) {
                renamed->parameters[i].expression = std::move(text);
            }
        }
    }
    for (std::size_t i = 0; i < state.features.size(); ++i) {
        const Feature& f = state.features[i];
        auto props = json::parse(f.propsJson.empty() ? std::string_view("{}") : std::string_view(f.propsJson));
        if (!props) {
            continue;
        }
        std::optional<Value> schema;
        if (const IFeatureType* type = registry ? registry->find(f.type) : nullptr) {
            if (auto parsed = json::parse(type->propsSchemaJson())) {
                schema = std::move(*parsed);
            }
        }
        Walker walker{name, to, {}};
        Value out = walker.walk(*props, schema ? &*schema : nullptr, "");
        if (walker.fields.empty()) {
            continue;
        }
        std::string fields;
        for (const std::string& field : walker.fields) {
            fields += (fields.empty() ? "" : ", ") + field;
        }
        uses.push_back(Use{(f.name.empty() ? f.type : f.name) + " (" + fields + ")"});
        if (renamed != nullptr) {
            renamed->features[i].propsJson = json::write(out);
        }
    }
    return uses;
}

std::ptrdiff_t paramIndex(const DocState& state, const ParamId& id)
{
    const auto it =
        std::find_if(state.parameters.begin(), state.parameters.end(), [&](const Parameter& p) { return p.id == id; });
    return it == state.parameters.end() ? -1 : it - state.parameters.begin();
}

Result<DocStatePtr> paramRename(const DocState& before, std::string_view argsJson)
{
    RAY_DOC_TRY(args, Args::parse(kParamRename, argsJson, {"id", "name"}));
    RAY_DOC_TRY(id, args->id("id"));
    RAY_DOC_TRY(name, args->text("name"));
    const std::ptrdiff_t index = paramIndex(before, ParamId{*id});
    if (index < 0) {
        return args->error("id", "names no parameter of this document (" + id->toString() + ")");
    }
    if (detail::isReservedName(*name)) {
        return args->error("name", "'" + *name + "' is reserved: it is a unit or function name");
    }
    if (!detail::isValidParameterName(*name)) {
        return args->error("name", "must be a letter or '_' followed by letters, digits or '_'");
    }
    for (const Parameter& other : before.parameters) {
        if (other.name == *name && other.id != ParamId{*id}) {
            return args->error("name", "'" + *name + "' is already used by another parameter");
        }
    }
    const std::string old = before.parameters[static_cast<std::size_t>(index)].name;
    auto next = std::make_shared<DocState>(before);
    next->parameters[static_cast<std::size_t>(index)].name = *name;
    if (old == *name) {
        return DocStatePtr(std::move(next));
    }
    RAY_DOC_TRY(uses, usesOf(before, ParamId{*id}, old, next.get(), *name));
    if (evaluateParameters(before.parameters)) {
        if (auto values = evaluateParameters(next->parameters); !values) {
            return args->error("name", "would leave the parameters invalid: " + values.error().message);
        }
    }
    return DocStatePtr(std::move(next));
}

Result<DocStatePtr> paramRemove(const DocState& before, std::string_view argsJson)
{
    RAY_DOC_TRY(args, Args::parse(kParamRemove, argsJson, {"id"}));
    RAY_DOC_TRY(id, args->id("id"));
    const std::ptrdiff_t index = paramIndex(before, ParamId{*id});
    if (index < 0) {
        return args->error("id", "names no parameter of this document (" + id->toString() + ")");
    }
    const std::string name = before.parameters[static_cast<std::size_t>(index)].name;
    RAY_DOC_TRY(uses, usesOf(before, ParamId{*id}, name, nullptr, ""));
    if (!uses->empty()) {
        std::string list;
        for (const Use& u : *uses) {
            list += (list.empty() ? "" : "; ") + u.who;
        }
        return args->error("id", "'" + name + "' cannot be deleted while it is used by: " + list);
    }
    auto next = std::make_shared<DocState>(before);
    next->parameters.erase(next->parameters.begin() + index);
    return DocStatePtr(std::move(next));
}

#undef RAY_DOC_TRY

using ApplyFn = Result<DocStatePtr> (*)(const DocState& before, std::string_view argsJson);

class ParamCommand final : public ICommandHandler {
public:
    ParamCommand(std::string_view name, std::string_view schema, ApplyFn fn) : name_(name), schema_(schema), fn_(fn) {}
    std::string_view name() const override { return name_; }
    std::string_view argsSchemaJson() const override { return schema_; }
    bool changesToolpathsOrGcode() const override { return false; }
    Result<DocStatePtr> apply(DocStatePtr before, std::string_view argsJson) const override
    {
        if (!before) {
            return invalidArgument(std::string(name_) + ": there is no document snapshot to apply it to");
        }
        try {
            return fn_(*before, argsJson);
        } catch (const std::bad_alloc&) {
            return tl::unexpected(Error{ErrorCode::Internal, std::string(name_) + ": out of memory", {}});
        } catch (...) {
            return tl::unexpected(Error{ErrorCode::Internal, std::string(name_) + ": unexpected failure", {}});
        }
    }

private:
    std::string_view name_;
    std::string_view schema_;
    ApplyFn fn_;
};

#define RAY_DOC_UUID R"({"type":"string","pattern":"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"})"
constexpr std::string_view kParamRenameSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("required":["id","name"],"properties":{"id":)" RAY_DOC_UUID
    R"(,"name":{"type":"string","pattern":"^[A-Za-z_][A-Za-z0-9_]*$"}}})";
constexpr std::string_view kParamRemoveSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("required":["id"],"properties":{"id":)" RAY_DOC_UUID R"(}})";
#undef RAY_DOC_UUID

} // namespace

std::vector<std::unique_ptr<ICommandHandler>> makeParamCommandHandlers()
{
    std::vector<std::unique_ptr<ICommandHandler>> out;
    out.push_back(std::make_unique<ParamCommand>(kParamRename, kParamRenameSchema, &paramRename));
    out.push_back(std::make_unique<ParamCommand>(kParamRemove, kParamRemoveSchema, &paramRemove));
    return out;
}

} // namespace ray::doc::commands
