// The document.import command (<ray/document.h>, "The import feature"): adds an import feature and, when its props
// list the imported bodies, a Body for each (name, colour and role from the props), as one undo step.
//
// Arguments: {"id": "<feature uuid>", "name": "...", "props": {import props}, "index"?: n}. The props are checked
// strictly (unknown keys are rejected); the body ids must not be used by bodies of the document yet. Like every
// command it is a pure function of (snapshot, canonical arguments): it reads no file.
#include "import_feature.h"
#include "import_props.h"

#include "../commands/args.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <utility>

namespace ray::doc::features {
namespace {

using commands::Args;

Result<DocStatePtr> applyImport(const DocState& before, std::string_view argsJson)
{
    auto args = Args::parse(kImportCommand, argsJson, {"id", "index", "name", "props"});
    if (!args) {
        return tl::unexpected(args.error());
    }
    auto id = args->id("id");
    if (!id) {
        return tl::unexpected(id.error());
    }
    auto name = args->text("name");
    if (!name) {
        return tl::unexpected(name.error());
    }
    auto propsJson = args->optObject("props");
    if (!propsJson) {
        return tl::unexpected(propsJson.error());
    }
    if (!*propsJson) {
        return args->error("props", "is required");
    }
    auto index = args->optIndex("index", before.features.size());
    if (!index) {
        return tl::unexpected(index.error());
    }
    const FeatureId featureId{*id};
    if (std::any_of(before.features.begin(), before.features.end(),
                    [&](const Feature& f) { return f.id == featureId; })) {
        return args->error("id", "is already used by a feature (" + id->toString() + ")");
    }
    auto props = parseImportProps(**propsJson, true, std::string(kImportCommand) + ": props");
    if (!props) {
        return tl::unexpected(props.error());
    }
    if (props->bodies) {
        for (const ImportBody& body : *props->bodies) {
            if (std::any_of(before.bodies.begin(), before.bodies.end(),
                            [&](const Body& b) { return b.id == body.id; })) {
                return args->error("props",
                                   "names body " + body.id.value.toString() + ", which the document has already");
            }
        }
    }

    auto next = std::make_shared<DocState>(before);
    Feature feature;
    feature.id = featureId;
    feature.type = std::string(kImportType);
    feature.name = std::move(*name);
    feature.propsJson = std::move(**propsJson);
    const std::size_t at = index->value_or(next->features.size());
    next->features.insert(next->features.begin() + static_cast<std::ptrdiff_t>(at), std::move(feature));
    if (props->bodies) {
        for (const ImportBody& imported : *props->bodies) {
            Body body;
            body.id = imported.id;
            body.name = imported.name;
            body.role = props->role;
            body.color = imported.color;
            body.visible = true;
            next->bodies.push_back(std::move(body));
        }
    }
    return DocStatePtr(std::move(next));
}

constexpr std::string_view kSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("required":["id","name","props"],"properties":{)"
    R"("id":{"type":"string","pattern":"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"},)"
    R"("name":{"type":"string","minLength":1},"props":{"type":"object"},)"
    R"("index":{"type":"integer","minimum":0}}})";

class ImportCommand final : public ICommandHandler {
public:
    std::string_view name() const override { return kImportCommand; }
    std::string_view argsSchemaJson() const override { return kSchema; }
    // Importing changes geometry; CAM marks operations stale, it never regenerates them silently.
    bool changesToolpathsOrGcode() const override { return false; }
    Result<DocStatePtr> apply(DocStatePtr before, std::string_view argsJson) const override
    {
        if (!before) {
            return commands::invalidArgument(std::string(kImportCommand) +
                                             ": there is no document snapshot to apply it to");
        }
        try {
            return applyImport(*before, argsJson);
        } catch (const std::bad_alloc&) {
            return tl::unexpected(Error{ErrorCode::Internal, std::string(kImportCommand) + ": out of memory", {}});
        } catch (...) {
            return tl::unexpected(Error{ErrorCode::Internal, std::string(kImportCommand) + ": unexpected failure", {}});
        }
    }
};

} // namespace

std::unique_ptr<ICommandHandler> makeImportCommandHandler()
{
    return std::make_unique<ImportCommand>();
}

} // namespace ray::doc::features
