// feature.import.replace and prepareReplaceImport (task T-084); the rules are in replace_import.h.
#include "replace_import.h"

#include "args.h"

#include "../document_impl.h"
#include "../features/import_feature.h"
#include "../features/import_props.h"

#include <ray/naming.h>

#include <algorithm>
#include <exception>
#include <new>
#include <set>
#include <string>
#include <utility>

namespace ray::doc::commands {
namespace {

using features::ImportBody;
using features::ImportProps;

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

// ---------------------------------------------------------------- the command
Result<DocStatePtr> applyReplace(const DocState& before, std::string_view argsJson)
{
    auto args = Args::parse(kReplaceImportCommand, argsJson, {"id", "props"});
    if (!args) {
        return tl::unexpected(args.error());
    }
    auto id = args->id("id");
    if (!id) {
        return tl::unexpected(id.error());
    }
    auto propsJson = args->optObject("props");
    if (!propsJson) {
        return tl::unexpected(propsJson.error());
    }
    if (!*propsJson) {
        return args->error("props", "is required");
    }
    const FeatureId featureId{*id};
    const auto it = std::find_if(before.features.begin(), before.features.end(),
                                 [&](const Feature& f) { return f.id == featureId; });
    if (it == before.features.end()) {
        return args->error("id", "names no feature of the document (" + id->toString() + ")");
    }
    if (it->type != features::kImportType) {
        return args->error("id", "names a " + it->type + " feature; only an import can be replaced");
    }
    auto old = features::parseImportProps(it->propsJson, false, std::string(kReplaceImportCommand) + ": the feature");
    if (!old) {
        return tl::unexpected(old.error());
    }
    if (!old->bodies) {
        return args->error("id", "names an import whose bodies are not stored with the document; it cannot be "
                                 "replaced (import the new file instead)");
    }
    auto props = features::parseImportProps(**propsJson, true, std::string(kReplaceImportCommand) + ": props");
    if (!props) {
        return tl::unexpected(props.error());
    }
    if (!props->bodies) {
        return args->error("props", "must list the bodies");
    }
    if (props->bodies->size() != old->bodies->size()) {
        return args->error("props", "must list the import's " + std::to_string(old->bodies->size()) + " bodies, not " +
                                        std::to_string(props->bodies->size()));
    }
    for (std::size_t i = 0; i < props->bodies->size(); ++i) {
        if ((*props->bodies)[i].id != (*old->bodies)[i].id) {
            return args->error("props", "must keep the import's bodies in their order (body " + std::to_string(i + 1) +
                                            " is " + (*old->bodies)[i].id.value.toString() + ")");
        }
    }
    if (props->role != old->role) {
        return args->error("props", "must keep the import's role");
    }
    auto next = std::make_shared<DocState>(before);
    const auto at = static_cast<std::size_t>(it - before.features.begin());
    next->features[at].propsJson = std::move(**propsJson);
    return DocStatePtr(std::move(next));
}

constexpr std::string_view kSchema =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
    R"("required":["id","props"],"properties":{)"
    R"("id":{"type":"string","pattern":"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"},)"
    R"("props":{"type":"object"}}})";

class ReplaceImportCommand final : public ICommandHandler {
public:
    std::string_view name() const override { return kReplaceImportCommand; }
    std::string_view argsSchemaJson() const override { return kSchema; }
    // New geometry: CAM marks operations stale, it never regenerates them silently (as document.import).
    bool changesToolpathsOrGcode() const override { return false; }
    Result<DocStatePtr> apply(DocStatePtr before, std::string_view argsJson) const override
    {
        if (!before) {
            return invalidArgument(std::string(kReplaceImportCommand) +
                                   ": there is no document snapshot to apply it to");
        }
        try {
            return applyReplace(*before, argsJson);
        } catch (const std::bad_alloc&) {
            return failure(ErrorCode::Internal, std::string(kReplaceImportCommand) + ": out of memory");
        } catch (...) {
            return failure(ErrorCode::Internal, std::string(kReplaceImportCommand) + ": unexpected failure");
        }
    }
};

// ---------------------------------------------------------------- preparing
double boxVolume(const Box3& b)
{
    const double x = b.max.x - b.min.x, y = b.max.y - b.min.y, z = b.max.z - b.min.z;
    return x > 0 && y > 0 && z > 0 ? x * y * z : 0.0;
}

// Intersection over union of two boxes (0 when they do not overlap).
double overlap(const Box3& a, const Box3& b)
{
    const Box3 i{{std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y), std::max(a.min.z, b.min.z)},
                 {std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y), std::min(a.max.z, b.max.z)}};
    const double vi = boxVolume(i);
    const double vu = boxVolume(a) + boxVolume(b) - vi;
    return vu > 0 ? vi / vu : 0.0;
}

// For each old body, the new body that replaces it; nullopt when the pairing is not clear (replace_import.h).
std::optional<std::vector<std::size_t>> pairBodies(const std::vector<kernel::Shape>& olds,
                                                   const std::vector<kernel::Shape>& news)
{
    const std::size_t n = olds.size();
    if (n == 1) {
        return std::vector<std::size_t>{0};
    }
    std::vector<Box3> a, b;
    for (const auto& s : olds) {
        a.push_back(kernel::bounds(s));
    }
    for (const auto& s : news) {
        b.push_back(kernel::bounds(s));
    }
    std::vector<std::vector<double>> score(n, std::vector<double>(n, 0.0));
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            score[i][j] = overlap(a[i], b[j]);
        }
    }
    auto best = [&](const std::vector<double>& row, std::size_t& at) {
        at = 0;
        for (std::size_t j = 1; j < row.size(); ++j) {
            if (row[j] > row[at]) {
                at = j;
            }
        }
        double second = 0;
        for (std::size_t j = 0; j < row.size(); ++j) {
            if (j != at) {
                second = std::max(second, row[j]);
            }
        }
        return row[at] > 0 && second * 2.0 <= row[at];
    };
    std::vector<std::size_t> out(n);
    std::set<std::size_t> used;
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t j = 0;
        if (!best(score[i], j)) {
            return std::nullopt;
        }
        std::vector<double> column(n);
        for (std::size_t k = 0; k < n; ++k) {
            column[k] = score[k][j];
        }
        std::size_t back = 0;
        if (!best(column, back) || back != i || !used.insert(j).second) {
            return std::nullopt;
        }
        out[i] = j;
    }
    return out;
}

} // namespace

std::unique_ptr<ICommandHandler> makeReplaceImportCommandHandler()
{
    return std::make_unique<ReplaceImportCommand>();
}

void classifyReplacement(const naming::ElementMap& beforeMap, const kernel::Shape& before,
                         const naming::RematchResult& rematched, const kernel::Shape& after, ReplacedBody& body)
{
    using kernel::SubId;
    using kernel::SubKind;
    std::set<Hash256> oldNames, deleted;
    for (const SubKind kind : {SubKind::Vertex, SubKind::Edge, SubKind::Face}) {
        for (std::uint32_t i = 0; i < before.count(kind); ++i) {
            for (const Hash256& name : beforeMap.namesOf(SubId{kind, i})) {
                oldNames.insert(name);
            }
        }
    }
    for (const naming::LineageEvent& e : rematched.lineage) {
        if (e.kind == naming::LineageEvent::Kind::Deleted) {
            deleted.insert(e.from);
        }
    }
    for (auto& kind : body.elements) {
        for (auto& list : kind) {
            list.clear();
        }
    }
    for (const SubKind kind : {SubKind::Vertex, SubKind::Edge, SubKind::Face}) {
        auto& lists = body.elements[static_cast<std::size_t>(kind)];
        for (std::uint32_t i = 0; i < after.count(kind); ++i) {
            const SubId sub{kind, i};
            const std::span<const Hash256> names = rematched.map.namesOf(sub);
            std::size_t carried = 0;
            bool piece = false;
            for (const Hash256& name : names) {
                if (oldNames.count(name) != 0) {
                    ++carried;
                } else if (const naming::TopoName* record = rematched.map.record(name);
                           record != nullptr && record->role == "split") {
                    piece = piece || std::any_of(record->parents.begin(), record->parents.end(),
                                                 [&](const Hash256& p) { return oldNames.count(p) != 0; });
                }
            }
            const ElementFate fate = carried > 0 && carried == names.size() ? ElementFate::Unchanged
                                     : carried > 0 || piece                 ? ElementFate::Changed
                                                                            : ElementFate::New;
            lists[static_cast<std::size_t>(fate)].push_back(ReplacedElement{sub, {names.begin(), names.end()}});
        }
        for (std::uint32_t i = 0; i < before.count(kind); ++i) {
            const SubId sub{kind, i};
            const std::span<const Hash256> names = beforeMap.namesOf(sub);
            if (!names.empty() &&
                std::all_of(names.begin(), names.end(), [&](const Hash256& n) { return deleted.count(n) != 0; })) {
                lists[static_cast<std::size_t>(ElementFate::Removed)].push_back(
                    ReplacedElement{sub, {names.begin(), names.end()}});
            }
        }
    }
}

Result<PreparedReplace> prepareReplaceImport(const DocState& state, const IObjectSource& objects,
                                             const ReplaceRequest& request, JobContext& ctx)
{
    try {
        const auto it = std::find_if(state.features.begin(), state.features.end(),
                                     [&](const Feature& f) { return f.id == request.feature; });
        if (it == state.features.end()) {
            return failure(ErrorCode::NotFound, "The document has no such feature.");
        }
        if (it->type != features::kImportType) {
            return failure(ErrorCode::InvalidArgument, "'" + it->name + "' is not an imported part.");
        }
        auto old = features::parseImportProps(it->propsJson, false, "Import");
        if (!old) {
            return tl::unexpected(old.error());
        }
        if (!old->bodies) {
            return failure(ErrorCode::InvalidArgument,
                           "'" + it->name +
                               "' was imported without storing its geometry; import the new file instead.");
        }
        const std::string newFile(features::fileNameOf(request.path));
        const std::string oldFile(features::fileNameOf(old->sourceFile));
        const std::optional<ImportFormat> format =
            request.format ? request.format : features::formatFromPath(request.path);
        if (!format) {
            return failure(ErrorCode::InvalidArgument,
                           "'" + newFile + "' is not a STEP (.step, .stp) or IGES (.iges, .igs) file.");
        }
        auto cannot = [&](const Error& e) {
            return failure(e.code, "Cannot replace with '" + newFile + "': " + e.message);
        };

        // The part as the rebuild has it: stored geometry and names.
        std::vector<kernel::Shape> oldShapes;
        std::vector<naming::ElementMap> oldMaps;
        for (const ImportBody& body : *old->bodies) {
            auto bytes = objects.object(body.blob);
            if (!bytes) {
                return failure(ErrorCode::NotFound, "The stored geometry of body '" + body.name +
                                                        "' is missing, so it cannot be compared with a new revision.");
            }
            auto shape = kernel::deserialize(*bytes);
            if (!shape) {
                return failure(shape.error().code, "The stored geometry of body '" + body.name +
                                                       "' cannot be read: " + shape.error().message);
            }
            std::optional<std::string> note;
            auto map = features::importedBodyMap(
                it->id, *shape, body, [&](const Hash256& h) { return objects.object(h); }, note);
            if (!map) {
                return tl::unexpected(map.error());
            }
            oldShapes.push_back(std::move(*shape));
            oldMaps.push_back(std::move(*map));
        }

        // The new file.
        JobContext hashCtx{ctx.cancel, ctx.progress.sub(0.03)};
        auto hashBefore = hashFile(request.path, hashCtx);
        if (!hashBefore) {
            return cannot(hashBefore.error());
        }
        if (*hashBefore == old->fileHash) {
            return failure(ErrorCode::InvalidArgument, "'" + newFile +
                                                           "' is the very file the part was imported from; "
                                                           "there is nothing to replace.");
        }
        JobContext importCtx{ctx.cancel, ctx.progress.sub(0.47)};
        auto imported = features::importSourceFile(request.path, *format, old->options, importCtx);
        if (!imported) {
            return cannot(imported.error());
        }
        JobContext againCtx{ctx.cancel, ctx.progress.sub(0.03)};
        auto hashAfter = hashFile(request.path, againCtx);
        if (!hashAfter) {
            return cannot(hashAfter.error());
        }
        if (*hashAfter != *hashBefore) {
            return cannot(Error{ErrorCode::IoError, "the file changed while it was being read.", {}});
        }
        std::vector<kernel::ImportedBody>& newBodies = imported->bodies;
        if (newBodies.empty()) {
            return cannot(Error{ErrorCode::InvalidArgument, "the file holds no geometry.", {}});
        }
        const std::size_t n = old->bodies->size();
        if (newBodies.size() != n) {
            return cannot(Error{ErrorCode::InvalidArgument,
                                "it holds " + std::to_string(newBodies.size()) +
                                    (newBodies.size() == 1 ? " body" : " bodies") + " and the part has " +
                                    std::to_string(n) + ". A new revision must have the same number of bodies.",
                                {}});
        }
        std::vector<kernel::Shape> newShapes;
        for (std::size_t j = 0; j < n; ++j) {
            newShapes.push_back(newBodies[j].shape);
        }
        const auto pairing = pairBodies(oldShapes, newShapes);
        if (!pairing) {
            return cannot(Error{ErrorCode::InvalidArgument,
                                "which of its bodies replaces which body of the part cannot be told from where they "
                                "lie, so nothing was replaced.",
                                {}});
        }
        for (std::size_t i = 0; i < n; ++i) {
            const kernel::Shape& now = newShapes[(*pairing)[i]];
            const std::uint32_t solids = now.count(kernel::SubKind::Solid);
            const std::uint32_t had = oldShapes[i].count(kernel::SubKind::Solid);
            if (solids == 0) {
                return cannot(Error{ErrorCode::InvalidArgument,
                                    "its geometry is not a solid (surfaces only); only a solid can replace an imported "
                                    "part.",
                                    {}});
            }
            if (solids != had) {
                return cannot(Error{ErrorCode::InvalidArgument,
                                    "its body is " + std::to_string(solids) + " solids where '" +
                                        (*old->bodies)[i].name + "' is " + std::to_string(had) + ".",
                                    {}});
            }
        }

        PreparedReplace out;
        out.oldFile = oldFile;
        out.newFile = newFile;
        out.oldUnits = old->sourceUnits;
        out.newUnits = imported->sourceUnits;
        out.warnings = imported->warnings;
        ImportProps props = *old;
        props.sourceFile = request.path;
        props.format = *format;
        props.fileHash = *hashBefore;
        props.sourceUnits = imported->sourceUnits;
        props.warnings = imported->warnings;
        Progress matchProgress = ctx.progress.sub(0.47);
        for (std::size_t i = 0; i < n; ++i) {
            if (auto cancelled = ctx.cancel.check(); !cancelled) {
                return tl::unexpected(cancelled.error());
            }
            ImportBody& body = (*props.bodies)[i];
            auto stored = features::storedForm(newShapes[(*pairing)[i]], body.name);
            if (!stored) {
                return cannot(stored.error());
            }
            naming::RematchResult rematched = naming::rematchImported(oldMaps[i], oldShapes[i], it->id, stored->shape);
            if (!features::mapFits(rematched.map, stored->shape)) {
                return cannot(Error{ErrorCode::KernelFailure,
                                    "the faces and edges of body '" + body.name + "' could not be named.",
                                    {}});
            }
            std::vector<std::byte> mapBytes = rematched.map.serialize();
            body.blob = features::objectHash(stored->bytes);
            body.names = features::objectHash(mapBytes);
            out.objects.emplace_back(body.blob, std::move(stored->bytes));
            out.objects.emplace_back(*body.names, std::move(mapBytes));

            ReplacedBody summary;
            summary.body = body.id;
            summary.name = body.name;
            summary.before = oldShapes[i];
            summary.after = stored->shape;
            summary.beforeMap = oldMaps[i];
            summary.afterMap = rematched.map;
            classifyReplacement(oldMaps[i], oldShapes[i], rematched, stored->shape, summary);
            out.bodies.push_back(std::move(summary));
            matchProgress.set(static_cast<double>(i + 1) / static_cast<double>(n));
        }

        out.command.name = std::string(kReplaceImportCommand);
        out.command.label = newFile == oldFile ? "Replace " + oldFile + " with a new revision"
                                               : "Replace " + oldFile + " with " + newFile;
        const std::string propsJson = features::writeImportProps(props);
        out.command.argsJson =
            canonicalJson(R"({"id":")" + it->id.value.toString() + R"(","props":)" + propsJson + "}");
        if (out.command.argsJson.empty()) {
            return failure(ErrorCode::Internal, "The replacement could not be written.");
        }
        return out;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Replace: out of memory");
    } catch (...) {
        return failure(ErrorCode::Internal, "Replace: unexpected failure");
    }
}

} // namespace ray::doc::commands
