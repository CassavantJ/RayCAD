// The built-in model commands (PLAN.md 3.1, 3.2, 3.5). Each is a pure function of (snapshot, canonical arguments).
//
// Arguments (JSON objects; any other key is rejected; ids are lower-case UUIDs, never the nil one):
//   feature.add       {"id", "type", "name", "props": {...}, "suppressed"?: false, "index"?: <end>}
//                     inserts a new feature at `index` (0 to the feature count; without it, right after the rollback
//                     bar, or at the end when the history is not rolled back). A feature that goes in right after
//                     the bar moves the bar past it (DocState::rollback). The id comes from the caller, so
//                     replay gives the same document. `type` is 1 to 64 of A-Z a-z 0-9 _ . -; props are not checked
//                     against the type's schema here (the rebuilder reports bad props as a failed feature). A sketch's
//                     props are stored with its coordinates solved, as feature.sketch.add stores them.
//   feature.edit      {"id", "name"?, "props"?}        replaces the name and/or the whole props object (a sketch's
//                     stored solved)
//   feature.remove    {"id"}                           (the rollback bar on it moves to the feature before)
//   feature.reorder   {"id", "index"}                  moves the feature to `index` of the resulting list
//   feature.suppress  {"id", "suppressed"}
//   feature.rollback  {"to"?: "<feature id>" | null}   moves the rollback bar after that feature; without "to" (or
//                     null, or the last feature) the whole history is built again. Moving it nowhere is refused.
//   param.set         {"id", "name"?, "expression"?, "kind"?}   kind: "length" | "angle" | "count" | "unitless"
//                     adds the parameter (all three fields required) or changes the given fields of an existing one.
//                     The name must be a valid, unreserved and unused identifier. If the parameters evaluated before
//                     the change, they must still evaluate after it (no cycle, unknown name or unit mismatch). Every
//                     sketch with a dimension whose value the change alters is stored solved again in the same step
//                     (features::storeDependentSketches).
//   body.set          {"id", "name"?, "role"?, "color"?, "visible"?}  role: "part" | "fixture" | "stock_model";
//                     color: [r, g, b, a] from 0 to 1, stored as float. Adds the body (name and role required;
//                     color defaults to [0.72, 0.74, 0.78, 1], visible to true) or changes the given fields.
// Edits keep every field they do not name, including extraJson (unknown keys read from a file).
// None of these commands changes toolpaths or G-code directly: CAM marks operations stale, it never regenerates
// them silently, so changesToolpathsOrGcode() is false.
#include "args.h"
#include "commands.h"

#include "../document_impl.h"
#include "../expr.h"
#include "../features/feature_props.h"
#include "../features/sketch_feature.h"
#include "../json_number.h"

#include <ray/cam.h>

#include <algorithm>
#include <array>
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

// Assigns the value of a Result to a new variable, or returns its error.
#define RAY_DOC_TRY(var, expr)                                                                                         \
    auto var = (expr);                                                                                                 \
    if (!var) {                                                                                                        \
        return tl::unexpected(var.error());                                                                            \
    }

// `job` is the job the command runs in (Document::prepare), null for IDocument::execute: what may solve a sketch
// passes it on for cancellation.
using ApplyFn = Result<DocStatePtr> (*)(const DocState& before, std::string_view argsJson, JobContext* job);

class ModelCommand final : public ICommandHandler {
public:
    ModelCommand(std::string_view name, std::string_view schema, ApplyFn fn) : name_(name), schema_(schema), fn_(fn) {}
    std::string_view name() const override { return name_; }
    std::string_view argsSchemaJson() const override { return schema_; }
    bool changesToolpathsOrGcode() const override { return false; }
    Result<DocStatePtr> apply(DocStatePtr before, std::string_view argsJson) const override
    {
        return run(before, argsJson, nullptr);
    }
    Result<DocStatePtr> applyWithJob(DocStatePtr before, std::string_view argsJson, JobContext& ctx) const override
    {
        return run(before, argsJson, &ctx);
    }

private:
    Result<DocStatePtr> run(const DocStatePtr& before, std::string_view argsJson, JobContext* job) const
    {
        if (!before) {
            return invalidArgument(std::string(name_) + ": there is no document snapshot to apply it to");
        }
        try {
            return fn_(*before, argsJson, job);
        } catch (const std::bad_alloc&) {
            return tl::unexpected(Error{ErrorCode::Internal, std::string(name_) + ": out of memory", {}});
        } catch (...) {
            return tl::unexpected(Error{ErrorCode::Internal, std::string(name_) + ": unexpected failure", {}});
        }
    }

    std::string_view name_;
    std::string_view schema_;
    ApplyFn fn_;
};

std::shared_ptr<DocState> copyOf(const DocState& state)
{
    return std::make_shared<DocState>(state);
}

template <class T, class IdT>
std::ptrdiff_t indexOf(const std::vector<T>& items, const IdT& id)
{
    const auto it = std::find_if(items.begin(), items.end(), [&](const T& item) { return item.id == id; });
    return it == items.end() ? -1 : it - items.begin();
}

Result<std::size_t> existingFeature(const Args& args, const DocState& state, std::string_view key = "id")
{
    RAY_DOC_TRY(id, args.id(key));
    const std::ptrdiff_t index = indexOf(state.features, FeatureId{*id});
    if (index < 0) {
        return args.error(key, "names no feature of this document (" + id->toString() + ")");
    }
    return static_cast<std::size_t>(index);
}

bool validTypeName(std::string_view type)
{
    if (type.empty() || type.size() > 64) {
        return false;
    }
    return std::all_of(type.begin(), type.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == '-';
    });
}

// ---------- feature.* ----------

constexpr std::string_view kFeatureAdd = "feature.add";
constexpr std::string_view kFeatureEdit = "feature.edit";
constexpr std::string_view kFeatureRemove = "feature.remove";
constexpr std::string_view kFeatureReorder = "feature.reorder";
constexpr std::string_view kFeatureSuppress = "feature.suppress";
constexpr std::string_view kFeatureRollback = "feature.rollback";
constexpr std::string_view kParamSet = "param.set";
constexpr std::string_view kBodySet = "body.set";

Result<DocStatePtr> featureAdd(const DocState& before, std::string_view argsJson, JobContext* job)
{
    RAY_DOC_TRY(args, Args::parse(kFeatureAdd, argsJson, {"id", "index", "name", "props", "suppressed", "type"}));
    RAY_DOC_TRY(id, args->id("id"));
    RAY_DOC_TRY(type, args->text("type"));
    RAY_DOC_TRY(name, args->text("name"));
    RAY_DOC_TRY(props, args->optObject("props"));
    RAY_DOC_TRY(suppressed, args->optBool("suppressed"));
    RAY_DOC_TRY(index, args->optIndex("index", before.features.size()));
    if (indexOf(before.features, FeatureId{*id}) >= 0) {
        return args->error("id", "is already used by a feature (" + id->toString() + ")");
    }
    if (!validTypeName(*type)) {
        return args->error("type", "must be 1 to 64 of the characters A-Z a-z 0-9 _ . -");
    }
    if (!*props) {
        return args->error("props", "is required");
    }
    Feature feature;
    feature.id = FeatureId{*id};
    feature.type = std::move(*type);
    feature.name = std::move(*name);
    feature.propsJson = std::move(**props);
    feature.suppressed = suppressed->value_or(false);
    auto next = copyOf(before);
    const std::size_t at = index->value_or(detail::defaultInsertIndex(*next));
    detail::insertFeature(*next, at, std::move(feature));
    // A sketch is stored solved, as feature.sketch.add stores it (features::storedPropsOf).
    RAY_DOC_TRY(stored, features::storedPropsOf(*next, next->features[at], job));
    next->features[at].propsJson = std::move(*stored);
    return next;
}

Result<DocStatePtr> featureEdit(const DocState& before, std::string_view argsJson, JobContext* job)
{
    RAY_DOC_TRY(args, Args::parse(kFeatureEdit, argsJson, {"id", "name", "props"}));
    RAY_DOC_TRY(index, existingFeature(*args, before));
    RAY_DOC_TRY(name, args->optText("name"));
    RAY_DOC_TRY(props, args->optObject("props"));
    if (!*name && !*props) {
        return args->error("nothing to change: give 'name', 'props' or both");
    }
    auto next = copyOf(before);
    Feature& feature = next->features[*index];
    if (*name) {
        feature.name = std::move(**name);
    }
    if (*props) {
        feature.propsJson = std::move(**props);
        RAY_DOC_TRY(stored, features::storedPropsOf(*next, feature, job)); // a sketch: stored solved
        feature.propsJson = std::move(*stored);
    }
    return next;
}

Result<DocStatePtr> featureRemove(const DocState& before, std::string_view argsJson, JobContext*)
{
    RAY_DOC_TRY(args, Args::parse(kFeatureRemove, argsJson, {"id"}));
    RAY_DOC_TRY(index, existingFeature(*args, before));
    auto next = copyOf(before);
    detail::eraseFeature(*next, *index);
    return next;
}

Result<DocStatePtr> featureReorder(const DocState& before, std::string_view argsJson, JobContext*)
{
    RAY_DOC_TRY(args, Args::parse(kFeatureReorder, argsJson, {"id", "index"}));
    RAY_DOC_TRY(from, existingFeature(*args, before));
    RAY_DOC_TRY(to, args->optIndex("index", before.features.size() - 1));
    if (!*to) {
        return args->error("index", "is required");
    }
    auto next = copyOf(before);
    auto& features = next->features;
    Feature moved = std::move(features[*from]);
    features.erase(features.begin() + static_cast<std::ptrdiff_t>(*from));
    features.insert(features.begin() + static_cast<std::ptrdiff_t>(**to), std::move(moved));
    detail::normalizeRollback(*next); // the bar stays with its feature
    return next;
}

Result<DocStatePtr> featureRollback(const DocState& before, std::string_view argsJson, JobContext*)
{
    RAY_DOC_TRY(args, Args::parse(kFeatureRollback, argsJson, {"to"}));
    RAY_DOC_TRY(to, args->optValue("to"));
    auto next = copyOf(before);
    if (*to == nullptr || (*to)->isNull()) {
        next->rollback.reset(); // roll forward to the end
    } else {
        RAY_DOC_TRY(index, existingFeature(*args, before, "to"));
        next->rollback = before.features[*index].id;
        detail::normalizeRollback(*next);
    }
    if (next->rollback == before.rollback) {
        return args->error(next->rollback ? "the rollback bar is there already" : "the history is not rolled back");
    }
    return next;
}

Result<DocStatePtr> featureSuppress(const DocState& before, std::string_view argsJson, JobContext*)
{
    RAY_DOC_TRY(args, Args::parse(kFeatureSuppress, argsJson, {"id", "suppressed"}));
    RAY_DOC_TRY(index, existingFeature(*args, before));
    RAY_DOC_TRY(suppressed, args->boolean("suppressed"));
    auto next = copyOf(before);
    next->features[*index].suppressed = *suppressed;
    return next;
}

// ---------- param.set ----------

Result<std::optional<Parameter::Kind>> optKind(const Args& args)
{
    RAY_DOC_TRY(text, args.optString("kind"));
    if (!*text) {
        return std::optional<Parameter::Kind>();
    }
    constexpr std::pair<std::string_view, Parameter::Kind> kinds[] = {{"length", Parameter::Kind::Length},
                                                                      {"angle", Parameter::Kind::Angle},
                                                                      {"count", Parameter::Kind::Count},
                                                                      {"unitless", Parameter::Kind::Unitless}};
    for (const auto& [name, kind] : kinds) {
        if (**text == name) {
            return std::optional<Parameter::Kind>(kind);
        }
    }
    return args.error("kind", "must be one of length, angle, count, unitless");
}

Result<DocStatePtr> paramSet(const DocState& before, std::string_view argsJson, JobContext* job)
{
    RAY_DOC_TRY(args, Args::parse(kParamSet, argsJson, {"expression", "id", "kind", "name"}));
    RAY_DOC_TRY(id, args->id("id"));
    RAY_DOC_TRY(name, args->optText("name"));
    RAY_DOC_TRY(expression, args->optText("expression"));
    RAY_DOC_TRY(kind, optKind(*args));
    const std::ptrdiff_t index = indexOf(before.parameters, ParamId{*id});
    if (index >= 0) {
        if (!*name && !*expression && !*kind) {
            return args->error("nothing to change: give 'name', 'expression' or 'kind'");
        }
    } else {
        for (const auto& [key, present] : {std::pair<const char*, bool>{"name", name->has_value()},
                                           std::pair<const char*, bool>{"expression", expression->has_value()},
                                           std::pair<const char*, bool>{"kind", kind->has_value()}}) {
            if (!present) {
                return args->error(key, "is required to add a parameter");
            }
        }
    }
    if (*name) {
        if (detail::isReservedName(**name)) {
            return args->error("name", "'" + **name + "' is reserved: it is a unit or function name");
        }
        if (!detail::isValidParameterName(**name)) {
            return args->error("name", "must be a letter or '_' followed by letters, digits or '_'");
        }
        for (const Parameter& other : before.parameters) {
            if (other.name == **name && other.id != ParamId{*id}) {
                return args->error("name", "'" + **name + "' is already used by another parameter");
            }
        }
    }
    auto next = copyOf(before);
    if (index < 0) {
        Parameter added;
        added.id = ParamId{*id};
        added.kind = Parameter::Kind::Unitless;
        next->parameters.push_back(std::move(added));
    }
    Parameter& p = next->parameters[index < 0 ? next->parameters.size() - 1 : static_cast<std::size_t>(index)];
    if (*name) {
        p.name = std::move(**name);
    }
    if (*expression) {
        p.expression = std::move(**expression);
    }
    if (*kind) {
        p.kind = **kind;
    }
    // Parameters that were already broken (e.g. read from a file) may be repaired one step at a time.
    if (evaluateParameters(before.parameters)) {
        auto values = evaluateParameters(next->parameters);
        if (!values) {
            const char* field = expression->has_value() ? "expression" : kind->has_value() ? "kind" : "name";
            return args->error(field, "would leave the parameters invalid: " + values.error().message);
        }
    }
    // The sketches whose dimensions the change reaches, stored solved in this step (the rebuild must not be the only
    // one to see the new shape: the document, a save, undo and the next sketch command would hold the old one).
    if (auto st = features::storeDependentSketches(before, *next, job); !st) {
        return tl::unexpected(st.error());
    }
    return next;
}

// ---------- body.set ----------

Result<std::optional<Body::Role>> optRole(const Args& args)
{
    RAY_DOC_TRY(text, args.optString("role"));
    if (!*text) {
        return std::optional<Body::Role>();
    }
    constexpr std::pair<std::string_view, Body::Role> roles[] = {
        {"part", Body::Role::Part}, {"fixture", Body::Role::Fixture}, {"stock_model", Body::Role::StockModel}};
    for (const auto& [name, role] : roles) {
        if (**text == name) {
            return std::optional<Body::Role>(role);
        }
    }
    return args.error("role", "must be one of part, fixture, stock_model");
}

Result<std::optional<std::array<float, 4>>> optColor(const Args& args)
{
    RAY_DOC_TRY(value, args.optValue("color"));
    if (*value == nullptr) {
        return std::optional<std::array<float, 4>>();
    }
    const std::string shape = "must be an array of 4 numbers from 0 to 1 (r, g, b, a)";
    if (!(*value)->isArray() || (*value)->asArray().size() != 4) {
        return args.error("color", shape);
    }
    std::array<float, 4> color{};
    for (std::size_t i = 0; i < 4; ++i) {
        const Value& c = (*value)->asArray()[i];
        if (!c.isNumber() || !(c.asNumber() >= 0.0 && c.asNumber() <= 1.0)) {
            return args.error("color", shape);
        }
        // Read through the shortest decimal of the double, as deserializeState does, so the float does not depend
        // on how the caller spelled the number (the journal keeps only the canonical spelling).
        const auto f = detail::parseFloat(detail::formatDouble(c.asNumber()));
        if (!f) {
            return args.error("color", shape);
        }
        color[i] = *f;
    }
    return std::optional<std::array<float, 4>>(color);
}

Result<DocStatePtr> bodySet(const DocState& before, std::string_view argsJson, JobContext*)
{
    RAY_DOC_TRY(args, Args::parse(kBodySet, argsJson, {"color", "id", "name", "role", "visible"}));
    RAY_DOC_TRY(id, args->id("id"));
    RAY_DOC_TRY(name, args->optText("name"));
    RAY_DOC_TRY(role, optRole(*args));
    RAY_DOC_TRY(color, optColor(*args));
    RAY_DOC_TRY(visible, args->optBool("visible"));
    const std::ptrdiff_t index = indexOf(before.bodies, BodyId{*id});
    if (index >= 0) {
        if (!*name && !*role && !*color && !*visible) {
            return args->error("nothing to change: give 'name', 'role', 'color' or 'visible'");
        }
    } else if (!*name) {
        return args->error("name", "is required to add a body");
    } else if (!*role) {
        return args->error("role", "is required to add a body");
    }
    // T-077: a fixture is never machined, so a body a setup machines cannot become one.
    if (index >= 0 && *role && **role == Body::Role::Fixture && before.cam) {
        for (const cam::Setup& setup : before.cam->setups) {
            if (std::find(setup.partBodies.begin(), setup.partBodies.end(), BodyId{*id}) != setup.partBodies.end()) {
                return args->error("role", "cannot be fixture: setup '" + setup.name +
                                               "' machines this body; take it out of that setup's part bodies first");
            }
        }
    }
    auto next = copyOf(before);
    if (index < 0) {
        Body added;
        added.id = BodyId{*id};
        added.role = Body::Role::Part;
        added.color = {0.72f, 0.74f, 0.78f, 1.0f};
        added.visible = true;
        next->bodies.push_back(std::move(added));
    }
    Body& body = next->bodies[index < 0 ? next->bodies.size() - 1 : static_cast<std::size_t>(index)];
    if (*name) {
        body.name = std::move(**name);
    }
    if (*role) {
        body.role = **role;
    }
    if (*color) {
        body.color = **color;
    }
    if (*visible) {
        body.visible = **visible;
    }
    return next;
}

#undef RAY_DOC_TRY

// ---------- Argument schemas (JSON Schema 2020-12) ----------

#define RAY_DOC_UUID R"({"type":"string","pattern":"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"})"

constexpr std::string_view kFeatureAddSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("required":["id","type","name","props"],"properties":{"id":)" RAY_DOC_UUID
    R"(,"type":{"type":"string","pattern":"^[A-Za-z0-9_.-]{1,64}$"},"name":{"type":"string","minLength":1},)"
    R"("props":{"type":"object"},"suppressed":{"type":"boolean"},"index":{"type":"integer","minimum":0}}})";
constexpr std::string_view kFeatureEditSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("required":["id"],"minProperties":2,"properties":{"id":)" RAY_DOC_UUID
    R"(,"name":{"type":"string","minLength":1},"props":{"type":"object"}}})";
constexpr std::string_view kFeatureRemoveSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("required":["id"],"properties":{"id":)" RAY_DOC_UUID R"(}})";
constexpr std::string_view kFeatureReorderSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("required":["id","index"],"properties":{"id":)" RAY_DOC_UUID R"(,"index":{"type":"integer","minimum":0}}})";
constexpr std::string_view kFeatureSuppressSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("required":["id","suppressed"],"properties":{"id":)" RAY_DOC_UUID R"(,"suppressed":{"type":"boolean"}}})";
constexpr std::string_view kFeatureRollbackSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("properties":{"to":{"anyOf":[)" RAY_DOC_UUID R"(,{"type":"null"}],"description":"The last feature to build; )"
    R"(absent or null rolls forward to the end of the history"}}})";
constexpr std::string_view kParamSetSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("required":["id"],"minProperties":2,"properties":{"id":)" RAY_DOC_UUID
    R"(,"name":{"type":"string","pattern":"^[A-Za-z_][A-Za-z0-9_]*$"},"expression":{"type":"string","minLength":1},)"
    R"("kind":{"enum":["length","angle","count","unitless"]}}})";
constexpr std::string_view kBodySetSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("required":["id"],"minProperties":2,"properties":{"id":)" RAY_DOC_UUID
    R"(,"name":{"type":"string","minLength":1},"role":{"enum":["part","fixture","stock_model"]},)"
    R"("color":{"type":"array","minItems":4,"maxItems":4,"items":{"type":"number","minimum":0,"maximum":1}},)"
    R"("visible":{"type":"boolean"}}})";

#undef RAY_DOC_UUID

} // namespace

std::vector<std::unique_ptr<ICommandHandler>> makeModelCommandHandlers()
{
    std::vector<std::unique_ptr<ICommandHandler>> out;
    out.push_back(std::make_unique<ModelCommand>(kFeatureAdd, kFeatureAddSchema, &featureAdd));
    out.push_back(std::make_unique<ModelCommand>(kFeatureEdit, kFeatureEditSchema, &featureEdit));
    out.push_back(std::make_unique<ModelCommand>(kFeatureRemove, kFeatureRemoveSchema, &featureRemove));
    out.push_back(std::make_unique<ModelCommand>(kFeatureReorder, kFeatureReorderSchema, &featureReorder));
    out.push_back(std::make_unique<ModelCommand>(kFeatureSuppress, kFeatureSuppressSchema, &featureSuppress));
    out.push_back(std::make_unique<ModelCommand>(kFeatureRollback, kFeatureRollbackSchema, &featureRollback));
    out.push_back(std::make_unique<ModelCommand>(kParamSet, kParamSetSchema, &paramSet));
    out.push_back(std::make_unique<ModelCommand>(kBodySet, kBodySetSchema, &bodySet));
    return out;
}

} // namespace ray::doc::commands
