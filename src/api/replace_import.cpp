// file.replaceImport and the replace dialog's helpers (task T-084); see ray_api/replace_import.h.
//
// The action, like file.import (AppImpl::importWork):
//   1. UI thread: the document is claimed (it cannot close meanwhile) and its state read.
//   2. Worker: doc's prepareReplaceImport reads the new file, pairs and matches its bodies, and stores the new BREP and
//      element map objects; they go into the recovery journal before the command, under the save lock.
//   3. UI thread: the objects go into the document's pool and feature.import.replace is executed (the job's point of
//      no return). If the document changed in between (another edit came in), the replace is refused: it was matched
//      against geometry that may no longer be the import's.
#include "replace_import_actions.h"

#include "app_impl.h"

#include "../doc/commands/replace_import.h"
#include "../doc/features/import_props.h"

#include <ray_api/replace_import.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace ray::api {

namespace replace_import {

std::optional<kernel::SubId> locate(const doc::BuildResult& build, const BodyId& body, kernel::SubKind kind,
                                    const std::vector<Hash256>& names)
{
    const auto it = build.bodies.find(body);
    if (it == build.bodies.end() || it->second.shape.isNull()) {
        return std::nullopt;
    }
    for (const Hash256& name : names) {
        if (const auto sub = it->second.map.find(name); sub && sub->kind == kind) {
            return sub;
        }
    }
    return std::nullopt;
}

std::vector<const doc::Feature*> replaceableImports(const doc::DocState& state)
{
    std::vector<const doc::Feature*> out;
    for (const doc::Feature& f : state.features) {
        if (f.type != doc::features::kImportType) {
            continue;
        }
        auto props = doc::features::parseImportProps(f.propsJson, false, "Import");
        if (props && props->bodies) {
            out.push_back(&f);
        }
    }
    return out;
}

} // namespace replace_import

namespace detail {
namespace {

using json::Value;
namespace dc = doc::commands;

constexpr const char* kUuidPattern = "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$";

Value prop(const char* type, const char* description)
{
    return Value::object({{"type", Value::string(type)}, {"description", Value::string(description)}});
}

const char* surfaceWord(kernel::SurfaceType t)
{
    switch (t) {
    case kernel::SurfaceType::Plane:
        return "plane";
    case kernel::SurfaceType::Cylinder:
        return "cylinder";
    case kernel::SurfaceType::Cone:
        return "cone";
    case kernel::SurfaceType::Sphere:
        return "sphere";
    case kernel::SurfaceType::Torus:
        return "torus";
    default:
        return "freeform";
    }
}

const char* fateWord(dc::ElementFate f)
{
    switch (f) {
    case dc::ElementFate::Unchanged:
        return "unchanged";
    case dc::ElementFate::Changed:
        return "changed";
    case dc::ElementFate::New:
        return "new";
    case dc::ElementFate::Removed:
        return "removed";
    }
    return "";
}

// A face of the summary: what it is and where it lies.
Value faceRow(const kernel::Shape& shape, const dc::ReplacedElement& e, dc::ElementFate fate)
{
    Value row = Value::object();
    row.set("fate", Value::string(fateWord(fate)));
    row.set("index", Value::integer(e.sub.index));
    Value names = Value::array();
    for (const Hash256& n : e.names) {
        names.push(Value::string(n.hex()));
    }
    row.set("names", std::move(names));
    const auto info = kernel::surfaceInfo(shape, e.sub);
    if (!info) {
        row.set("surface", Value::string("freeform"));
        return row;
    }
    row.set("surface", Value::string(surfaceWord(info->type)));
    bool hole = false;
    if (info->type == kernel::SurfaceType::Cylinder) {
        if (const auto s = kernel::faceSample(shape, e.sub)) {
            const Vec3 w{s->point.x - info->origin.x, s->point.y - info->origin.y, s->point.z - info->origin.z};
            const double a = w.x * info->axis.x + w.y * info->axis.y + w.z * info->axis.z;
            const Vec3 r{w.x - a * info->axis.x, w.y - a * info->axis.y, w.z - a * info->axis.z};
            hole = r.x * s->normal.x + r.y * s->normal.y + r.z * s->normal.z < 0;
        }
    }
    row.set("hole", Value::boolean(hole));
    row.set("radius", Value::number(info->radius));
    row.set("area", Value::number(info->area));
    const Box3& b = info->bounds;
    row.set("box", Value::array({Value::number(b.min.x), Value::number(b.min.y), Value::number(b.min.z),
                                 Value::number(b.max.x), Value::number(b.max.y), Value::number(b.max.z)}));
    return row;
}

Value summaryOf(const dc::PreparedReplace& prepared, const doc::DocState& state, const std::string& documentId,
                const FeatureId& feature)
{
    Value out = Value::object();
    out.set("document", Value::string(documentId));
    out.set("feature", Value::string(feature.value.toString()));
    out.set("label", Value::string(prepared.command.label));
    out.set("oldFile", Value::string(prepared.oldFile));
    out.set("newFile", Value::string(prepared.newFile));
    auto units = [](UnitSystem u) {
        return Value::string(u == UnitSystem::Inch ? "inch" : "metric");
    };
    if (prepared.oldUnits) {
        out.set("oldUnits", units(*prepared.oldUnits));
    }
    if (prepared.newUnits) {
        out.set("newUnits", units(*prepared.newUnits));
    }
    Value warnings = Value::array();
    for (const std::string& w : prepared.warnings) {
        warnings.push(Value::string(w));
    }
    out.set("warnings", std::move(warnings));

    Value counts = Value::object();
    const std::pair<kernel::SubKind, const char*> kinds[] = {
        {kernel::SubKind::Face, "faces"}, {kernel::SubKind::Edge, "edges"}, {kernel::SubKind::Vertex, "vertices"}};
    for (const auto& [kind, word] : kinds) {
        Value c = Value::object();
        for (const dc::ElementFate f :
             {dc::ElementFate::Unchanged, dc::ElementFate::Changed, dc::ElementFate::New, dc::ElementFate::Removed}) {
            std::size_t n = 0;
            for (const dc::ReplacedBody& b : prepared.bodies) {
                n += b.of(kind, f).size();
            }
            c.set(fateWord(f), Value::integer(static_cast<std::int64_t>(n)));
        }
        counts.set(word, std::move(c));
    }
    out.set("counts", std::move(counts));

    // The operations that use a replaced body: they go stale (or lose a reference) and need regenerating.
    std::set<BodyId> replaced;
    for (const dc::ReplacedBody& b : prepared.bodies) {
        replaced.insert(b.body);
    }
    std::int64_t toolpaths = 0;
    if (state.cam) {
        for (const cam::Setup& setup : state.cam->setups) {
            if (std::none_of(setup.partBodies.begin(), setup.partBodies.end(),
                             [&](const BodyId& b) { return replaced.count(b) != 0; })) {
                continue;
            }
            for (const OperationId& op : setup.operations) {
                const auto it = state.cam->operations.find(op);
                if (it != state.cam->operations.end() && !it->second.suppressed) {
                    ++toolpaths;
                }
            }
        }
    }
    out.set("toolpaths", Value::integer(toolpaths));

    Value bodies = Value::array();
    for (const dc::ReplacedBody& b : prepared.bodies) {
        Value body = Value::object();
        body.set("body", Value::string(b.body.value.toString()));
        body.set("name", Value::string(b.name));
        Value faces = Value::array();
        for (const dc::ElementFate f : {dc::ElementFate::Changed, dc::ElementFate::New, dc::ElementFate::Removed}) {
            for (const dc::ReplacedElement& e : b.of(kernel::SubKind::Face, f)) {
                faces.push(faceRow(f == dc::ElementFate::Removed ? b.before : b.after, e, f));
            }
        }
        body.set("faces", std::move(faces));
        bodies.push(std::move(body));
    }
    out.set("bodies", std::move(bodies));
    return out;
}

} // namespace

Status registerReplaceImportActions(AppImpl& app)
{
    Value path = prop("string", "The new revision: a STEP or IGES file (UTF-8)");
    path.set("minLength", Value::integer(1));
    Value feature = prop("string", "The import feature whose file is replaced");
    feature.set("pattern", Value::string(kUuidPattern));
    Value props =
        Value::object({{"feature", std::move(feature)},
                       {"path", std::move(path)},
                       {"document", prop("string", "The OpenDocument id; the active document when omitted")}});
    Value schema = Value::object({{"$schema", Value::string("https://json-schema.org/draft/2020-12/schema")},
                                  {"type", Value::string("object")},
                                  {"additionalProperties", Value::boolean(false)},
                                  {"properties", std::move(props)},
                                  {"required", Value::array({Value::string("feature"), Value::string("path")})}});
    ActionInfo info;
    info.id = replace_import::kAction;
    info.title = "Replace with New Revision...";
    info.category = "File";
    info.description = "Replaces an imported part's file with a new revision of it: faces that did not change keep "
                       "their references; changed, new and removed faces are listed for review";
    info.argsSchemaJson = json::canonical(schema);
    info.changesDocument = true;
    info.longRunning = true;
    app.setOrder(info.id, ActionOrder::Ordered);
    return app.actions().add(
        std::move(info), [&app](std::string_view argsJson, Caller, JobContext& ctx) -> Result<ActionResult> {
            auto args = json::parse(argsJson);
            if (!args || !args->isObject()) {
                return fail(ErrorCode::InvalidArgument, "The arguments must be a JSON object");
            }
            const std::string documentId = args->stringOr("document", "");
            const std::string path = args->stringOr("path", "");
            const FeatureId featureId{Uuid::fromString(args->stringOr("feature", ""))};

            // 1. Claim the document and read its state.
            struct Claimed {
                std::shared_ptr<BusyGuard> guard;
                doc::DocStatePtr state;
                std::shared_ptr<doc::IObjectSource> objects;
            };
            auto claimed = app.bridge().call<Claimed>(
                [&app, documentId]() -> Result<Claimed> {
                    auto guard = app.claim(documentId);
                    if (!guard) {
                        return tl::unexpected<Error>(guard.error());
                    }
                    auto shared = std::make_shared<BusyGuard>(std::move(*guard));
                    DocEntry* entry = shared->get();
                    return Claimed{shared, entry->od.document->state(), entry->objects};
                },
                ctx.cancel);
            if (!claimed) {
                return tl::unexpected<Error>(claimed.error());
            }
            DocEntry* entry = claimed->guard->get();

            // 2. Read and match the new file.
            dc::ReplaceRequest request;
            request.feature = featureId;
            request.path = path;
            auto prepared = dc::prepareReplaceImport(*claimed->state, *claimed->objects, request, ctx);
            if (!prepared) {
                return tl::unexpected<Error>(prepared.error());
            }
            auto shared = std::make_shared<dc::PreparedReplace>(std::move(*prepared));
            std::optional<Error> journalError;
            {
                std::lock_guard<std::mutex> saving(entry->saveMutex);
                journalError = app.ensureJournal(*entry);
                if (!journalError) {
                    std::lock_guard<std::mutex> lock(entry->journalMutex);
                    if (entry->od.journal && !entry->journalOff) {
                        for (const auto& [hash, bytes] : shared->objects) {
                            if (auto st = entry->od.journal->appendObject(hash, bytes); !st) {
                                journalError = st.error();
                                break;
                            }
                        }
                    }
                }
            }
            Value summary = summaryOf(*shared, *claimed->state, entry->od.id, featureId);

            // 3. Execute it on the state it was matched against.
            const doc::DocStatePtr matched = claimed->state;
            auto done = app.bridge().call<std::monostate>(
                [&app, entry, shared, matched, journalError, &ctx]() -> Result<std::monostate> {
                    if (entry->od.document->state() != matched) {
                        return fail(ErrorCode::InvalidArgument,
                                    "The document changed while the new revision was being read; try again.");
                    }
                    if (!app.pointOfNoReturn(ctx.cancel)) {
                        return fail(ErrorCode::Cancelled, "Cancelled");
                    }
                    if (journalError) {
                        app.journalFailed(*entry, *journalError);
                    }
                    for (const auto& [hash, bytes] : shared->objects) {
                        auto put = entry->od.objects->put(bytes);
                        if (!put) {
                            return withContext(put.error(), "keeping the new revision's geometry");
                        }
                    }
                    auto executed = app.execute(*entry, shared->command);
                    if (!executed) {
                        return tl::unexpected<Error>(executed.error());
                    }
                    return std::monostate{};
                },
                ctx.cancel);
            if (!done) {
                return tl::unexpected<Error>(done.error());
            }
            ActionResult result{json::canonical(summary), shared->warnings};
            return result;
        });
}

} // namespace detail
} // namespace ray::api
