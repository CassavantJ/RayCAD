// The references of a document for the reference review panel (ray_api/references.h, task T-083).
#include <ray_api/references.h>

#include "../doc/commands/cam_json.h"
#include "json.h"

#include <ray_api/feature_edit.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <set>
#include <utility>

namespace ray::api::references {
namespace {

using json::Array;
using json::Value;

constexpr double kQuantumMm = 1e-4; // naming.h Territory: coordinates in 1e-4 mm

tl::unexpected<Error> fail(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

// Feature types whose props an edit names member by member (feature.<type>.edit, feature_props.h TypedCommand).
// Every other type is edited with feature.edit and its whole props.
bool typedEdit(const std::string& type)
{
    static const std::set<std::string, std::less<>> typed = {
        "datum_plane", "extrude",        "revolve",     "fillet",      "chamfer",
        "hole",        "transform_body", "delete_face", "offset_face", "resize_hole"};
    return typed.count(type) != 0;
}

// A JSON object that reads as a TopoRef: naming's own reader decides.
std::optional<naming::TopoRef> asRef(const Value& v)
{
    if (!v.isObject() || v.find("names") == nullptr || v.find("kind") == nullptr || v.find("body") == nullptr) {
        return std::nullopt;
    }
    auto ref = naming::topoRefFromJson(json::write(v));
    return ref ? std::optional<naming::TopoRef>(std::move(*ref)) : std::nullopt;
}

struct Found {
    std::vector<std::string> path; // segments
    bool inList = false;
    naming::TopoRef ref;
};

void findRefs(const Value& v, std::vector<std::string>& path, bool inList, std::vector<Found>& out)
{
    if (auto ref = asRef(v)) {
        out.push_back(Found{path, inList, std::move(*ref)});
        return;
    }
    if (v.isObject()) {
        for (const auto& [key, member] : v.asObject()) {
            path.push_back(key);
            findRefs(member, path, false, out);
            path.pop_back();
        }
    } else if (v.isArray()) {
        const Array& items = v.asArray();
        for (std::size_t i = 0; i < items.size(); ++i) {
            path.push_back(std::to_string(i));
            findRefs(items[i], path, true, out);
            path.pop_back();
        }
    }
}

std::string joinPath(const std::vector<std::string>& segments)
{
    std::string out;
    for (const std::string& s : segments) {
        out += (out.empty() ? "" : "/") + s;
    }
    return out;
}

std::vector<std::string> splitPath(const std::string& path)
{
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t slash = path.find('/', at);
        out.push_back(path.substr(at, slash == std::string::npos ? std::string::npos : slash - at));
        if (slash == std::string::npos) {
            break;
        }
        at = slash + 1;
    }
    return out;
}

void evidenceOf(const naming::TopoRef& ref, Reference& out)
{
    if (!ref.territory.known()) {
        return;
    }
    const auto mm = [](const std::array<std::int64_t, 3>& q) {
        return Vec3{static_cast<double>(q[0]) * kQuantumMm, static_cast<double>(q[1]) * kQuantumMm,
                    static_cast<double>(q[2]) * kQuantumMm};
    };
    out.pickedBox = Box3{mm(ref.territory.qMin), mm(ref.territory.qMax)};
    for (const auto& p : ref.territory.points) {
        out.pickedPoints.push_back(mm(p));
    }
}

void resolveInto(const naming::IResolver& resolver, const naming::TopoRef& ref, Reference& out)
{
    naming::Resolved r = resolver.resolve(ref);
    out.state = r.state;
    out.reason = std::move(r.reason);
    out.repairConfirmed = ref.repairConfirmed;
    out.elements = std::move(r.elements);
}

std::vector<naming::ResolverBody> resolverBodies(const doc::BuildResult& build)
{
    std::vector<naming::ResolverBody> bodies;
    for (const auto& [id, built] : build.bodies) {
        if (!built.shape.isNull()) {
            bodies.push_back(naming::ResolverBody{id, built.map, built.shape});
        }
    }
    return bodies;
}

const doc::FeatureReport* reportOf(const doc::BuildResult* build, const FeatureId& id)
{
    if (build == nullptr) {
        return nullptr;
    }
    for (const doc::FeatureReport& r : build->reports) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

// The JSON text of `refs` as Values, checked to be references of `kind`.
Result<std::vector<Value>> parseRefs(std::span<const std::string> refs, kernel::SubKind kind)
{
    std::vector<Value> out;
    for (const std::string& text : refs) {
        auto v = json::parse(text);
        if (!v) {
            return fail(ErrorCode::InvalidArgument, "A new reference is not JSON: " + v.error().message);
        }
        auto ref = asRef(*v);
        if (!ref) {
            return fail(ErrorCode::InvalidArgument, "A new reference is not a reference");
        }
        if (ref->kind != kind) {
            return fail(ErrorCode::InvalidArgument, "A new reference is of another kind of element");
        }
        out.push_back(std::move(*v));
    }
    return out;
}

// `items` with the entry at `index` (which must equal `old`) replaced by `fresh`, none twice.
Result<Array> splice(const Array& items, std::size_t index, const Value& old, const std::vector<Value>& fresh)
{
    if (index >= items.size() || !(items[index] == old)) {
        return fail(ErrorCode::NotFound, "The reference changed since it was listed; look again");
    }
    Array out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != index) {
            out.push_back(items[i]);
            continue;
        }
        for (const Value& v : fresh) {
            if (std::none_of(out.begin(), out.end(), [&](const Value& o) { return o == v; }) &&
                std::none_of(items.begin() + static_cast<std::ptrdiff_t>(i) + 1, items.end(),
                             [&](const Value& o) { return o == v; })) {
                out.push_back(v);
            }
        }
    }
    return out;
}

Result<doc::Command> replaceInFeature(const doc::DocState& state, const Reference& ref, std::vector<Value> fresh)
{
    const FeatureId id{Uuid::fromString(ref.ownerId)};
    const auto it =
        std::find_if(state.features.begin(), state.features.end(), [&](const doc::Feature& f) { return f.id == id; });
    if (it == state.features.end()) {
        return fail(ErrorCode::NotFound, "The feature is no longer in the document");
    }
    auto props = json::parse(it->propsJson);
    if (!props || !props->isObject()) {
        return fail(ErrorCode::Internal, "The feature's properties could not be read");
    }
    const auto old = json::parse(ref.refJson);
    if (!old) {
        return fail(ErrorCode::InvalidArgument, "The listed reference could not be read");
    }
    const std::vector<std::string> path = splitPath(ref.path);
    // Walk to the parent of the reference.
    Value* parent = &*props;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        Value* next = nullptr;
        if (parent->isObject()) {
            next = parent->find(path[i]);
        } else if (parent->isArray()) {
            const std::size_t k = static_cast<std::size_t>(std::stoul(path[i]));
            next = k < parent->asArray().size() ? &parent->asArray()[k] : nullptr;
        }
        if (next == nullptr) {
            return fail(ErrorCode::NotFound, "The reference is no longer in the feature; look again");
        }
        parent = next;
    }
    const std::string& last = path.back();
    if (parent->isArray()) {
        auto spliced = splice(parent->asArray(), static_cast<std::size_t>(std::stoul(last)), *old, fresh);
        if (!spliced) {
            return tl::unexpected(spliced.error());
        }
        if (spliced->empty()) {
            return fail(ErrorCode::InvalidArgument, "Pick at least one element");
        }
        *parent = Value::array(std::move(*spliced));
    } else {
        Value* slot = parent->find(last);
        if (slot == nullptr || !(*slot == *old)) {
            return fail(ErrorCode::NotFound, "The reference changed since it was listed; look again");
        }
        if (fresh.size() != 1) {
            return fail(ErrorCode::InvalidArgument, "Pick exactly one element for this reference");
        }
        *slot = std::move(fresh.front());
    }
    doc::Command c;
    Value args = Value::object({{"id", Value::string(ref.ownerId)}});
    if (typedEdit(it->type)) {
        const Value* top = props->find(path.front());
        if (top == nullptr) {
            return fail(ErrorCode::Internal, "The edited property is missing");
        }
        args.set("props", Value::object({{path.front(), *top}}));
        c.name = "feature." + it->type + ".edit";
    } else {
        args.set("props", std::move(*props));
        c.name = "feature.edit";
    }
    c.argsJson = json::canonical(args);
    c.label = "Edit " + it->name;
    return c;
}

Result<doc::Command> replaceInOperation(const doc::DocState& state, const Reference& ref, std::vector<Value> fresh)
{
    const OperationId id{Uuid::fromString(ref.ownerId)};
    if (!state.cam) {
        return fail(ErrorCode::NotFound, "The operation is no longer in the document");
    }
    const auto op = state.cam->operations.find(id);
    if (op == state.cam->operations.end()) {
        return fail(ErrorCode::NotFound, "The operation is no longer in the document");
    }
    const auto old = json::parse(ref.refJson);
    if (!old) {
        return fail(ErrorCode::InvalidArgument, "The listed reference could not be read");
    }
    Array items;
    for (const naming::TopoRef& g : op->second.geometry) {
        auto v = json::parse(naming::toJson(g));
        if (!v) {
            return fail(ErrorCode::Internal, "The operation's geometry could not be read");
        }
        items.push_back(std::move(*v));
    }
    const std::vector<std::string> path = splitPath(ref.path);
    if (path.size() != 2 || path[0] != "geometry") {
        return fail(ErrorCode::InvalidArgument, "Not a reference of the operation's geometry");
    }
    auto spliced = splice(items, static_cast<std::size_t>(std::stoul(path[1])), *old, fresh);
    if (!spliced) {
        return tl::unexpected(spliced.error());
    }
    if (spliced->empty()) {
        return fail(ErrorCode::InvalidArgument, "Pick at least one element");
    }
    doc::Command c;
    c.name = "cam.operation.edit";
    c.argsJson = json::canonical(
        Value::object({{"id", Value::string(ref.ownerId)}, {"geometry", Value::array(std::move(*spliced))}}));
    c.label = "Edit " + op->second.name;
    return c;
}

Result<doc::Command> replaceInSetup(const doc::DocState& state, const Reference& ref, std::vector<Value> fresh)
{
    const SetupId id{Uuid::fromString(ref.ownerId)};
    if (!state.cam) {
        return fail(ErrorCode::NotFound, "The setup is no longer in the document");
    }
    const auto setup = std::find_if(state.cam->setups.begin(), state.cam->setups.end(),
                                    [&](const cam::Setup& s) { return s.id == id; });
    if (setup == state.cam->setups.end()) {
        return fail(ErrorCode::NotFound, "The setup is no longer in the document");
    }
    if (fresh.size() != 1) {
        return fail(ErrorCode::InvalidArgument, "Pick exactly one element for this reference");
    }
    auto picked = naming::topoRefFromJson(json::write(fresh.front()));
    if (!picked) {
        return fail(ErrorCode::InvalidArgument, "The new reference could not be read");
    }
    cam::Wcs wcs = setup->wcs;
    std::optional<naming::TopoRef>* member = ref.path == "wcs/zFace"          ? &wcs.zFace
                                             : ref.path == "wcs/xEdge"        ? &wcs.xEdge
                                             : ref.path == "wcs/originVertex" ? &wcs.originVertex
                                                                              : nullptr;
    if (member == nullptr || !member->has_value()) {
        return fail(ErrorCode::NotFound, "The reference is no longer in the setup; look again");
    }
    if (naming::toJson(**member) != naming::toJson(*naming::topoRefFromJson(ref.refJson))) {
        return fail(ErrorCode::NotFound, "The reference changed since it was listed; look again");
    }
    *member = std::move(*picked);
    doc::Command c;
    c.name = "cam.setup.edit";
    c.argsJson = doc::canonicalJson(R"({"id":")" + ref.ownerId + R"(","wcs":)" +
                                    doc::json::write(doc::camjson::toJson(wcs)) + "}");
    c.label = "Edit " + setup->name;
    return c;
}

Result<doc::Command> replace(const doc::DocState& state, const Reference& ref, std::vector<Value> fresh)
{
    try {
        switch (ref.owner) {
        case Owner::Feature:
            return replaceInFeature(state, ref, std::move(fresh));
        case Owner::Operation:
            return replaceInOperation(state, ref, std::move(fresh));
        case Owner::Setup:
            return replaceInSetup(state, ref, std::move(fresh));
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::Internal, "Out of memory");
    } catch (const std::exception& e) {
        return fail(ErrorCode::InvalidArgument, std::string("The reference's place could not be read: ") + e.what());
    }
    return fail(ErrorCode::Internal, "Unknown owner");
}

} // namespace

Result<Listing> list(IApp& app, const std::string& documentId, const doc::DocStatePtr& state,
                     const doc::BuildResultPtr& build, JobContext& ctx, bool all)
{
    if (!state) {
        return fail(ErrorCode::InvalidArgument, "There is no document state");
    }
    try {
        Listing out;
        const auto keep = [&](Reference& r) {
            if (all || r.needsReview()) {
                out.references.push_back(std::move(r));
            }
        };
        // ---- features, where each stands ----
        for (std::size_t i = 0; i < state->features.size(); ++i) {
            if (auto ok = ctx.cancel.check(); !ok) {
                return tl::unexpected(ok.error());
            }
            const doc::Feature& f = state->features[i];
            if (f.suppressed || f.type == "import" || f.propsJson.find("\"names\"") == std::string::npos) {
                continue;
            }
            const doc::FeatureReport* report = reportOf(build.get(), f.id);
            if (build && (report == nullptr || report->status == doc::FeatureStatus::NotBuilt ||
                          report->status == doc::FeatureStatus::Suppressed)) {
                continue;
            }
            auto props = json::parse(f.propsJson);
            if (!props) {
                continue;
            }
            std::vector<Found> found;
            std::vector<std::string> path;
            findRefs(*props, path, false, found);
            if (found.empty()) {
                continue;
            }
            const doc::DocStatePtr snapshot = feature_edit::stateBefore(*state, f.id);
            if (!snapshot) {
                continue;
            }
            auto before = feature_edit::buildSnapshot(app, documentId, snapshot, ctx);
            if (!before) {
                if (before.error().code == ErrorCode::Cancelled) {
                    return tl::unexpected(before.error());
                }
                continue;
            }
            if (!*before) {
                continue;
            }
            bool listed = false;
            std::map<BodyId, std::unique_ptr<naming::IResolver>> resolvers; // one per body, as the rebuild has
            for (std::size_t k = 0; k < found.size(); ++k) {
                Reference r;
                r.owner = Owner::Feature;
                r.ownerId = f.id.value.toString();
                r.ownerName = f.name;
                r.ownerType = f.type;
                r.ownerOrder = i;
                r.path = joinPath(found[k].path);
                r.index = k;
                r.of = found.size();
                r.inList = found[k].inList;
                r.kind = found[k].ref.kind;
                r.body = found[k].ref.body;
                r.before = f.id;
                r.refJson = naming::toJson(found[k].ref);
                evidenceOf(found[k].ref, r);
                const auto body = (*before)->bodies.find(r.body);
                if (body == (*before)->bodies.end() || body->second.shape.isNull()) {
                    r.state = naming::ResolveState::Broken;
                    r.reason = "The body it is on is not built at this point of the history.";
                    r.repairConfirmed = found[k].ref.repairConfirmed;
                } else {
                    auto& resolver = resolvers[r.body];
                    if (!resolver) {
                        resolver =
                            naming::makeResolver({naming::ResolverBody{r.body, body->second.map, body->second.shape}});
                    }
                    if (!resolver) {
                        return fail(ErrorCode::Internal, "Out of memory");
                    }
                    resolveInto(*resolver, found[k].ref, r);
                }
                const std::size_t was = out.references.size();
                keep(r);
                listed = listed || out.references.size() > was;
            }
            if (listed) {
                out.before[f.id] = *before;
            }
        }
        // ---- CAM, in the document's build ----
        if (state->cam && build) {
            const cam::CamSnapshot snap = doc::camSnapshot(state, build);
            auto resolver = naming::makeResolver(snap.bodies.empty() ? resolverBodies(*build) : snap.bodies);
            if (!resolver) {
                return fail(ErrorCode::Internal, "Out of memory");
            }
            std::size_t order = state->features.size();
            for (const cam::Setup& s : state->cam->setups) {
                if (auto ok = ctx.cancel.check(); !ok) {
                    return tl::unexpected(ok.error());
                }
                const std::pair<const char*, const std::optional<naming::TopoRef>*> wcsRefs[] = {
                    {"wcs/zFace", &s.wcs.zFace},
                    {"wcs/xEdge", &s.wcs.xEdge},
                    {"wcs/originVertex", &s.wcs.originVertex}};
                std::size_t n = 0, k = 0;
                for (const auto& [p, member] : wcsRefs) {
                    n += member->has_value() ? 1 : 0;
                }
                const std::size_t setupOrder = order++;
                for (const auto& [p, member] : wcsRefs) {
                    if (!member->has_value()) {
                        continue;
                    }
                    Reference r;
                    r.owner = Owner::Setup;
                    r.ownerId = s.id.value.toString();
                    r.ownerName = s.name;
                    r.ownerType = "setup";
                    r.ownerOrder = setupOrder;
                    r.setupName = s.name;
                    r.path = p;
                    r.index = k++;
                    r.of = n;
                    r.kind = (*member)->kind;
                    r.body = (*member)->body;
                    r.refJson = naming::toJson(**member);
                    evidenceOf(**member, r);
                    resolveInto(*resolver, **member, r);
                    keep(r);
                }
                for (const OperationId& opId : s.operations) {
                    const auto op = state->cam->operations.find(opId);
                    if (op == state->cam->operations.end()) {
                        continue;
                    }
                    const std::size_t opOrder = order++;
                    if (op->second.suppressed) {
                        continue;
                    }
                    const std::vector<naming::TopoRef>& geometry = op->second.geometry;
                    for (std::size_t g = 0; g < geometry.size(); ++g) {
                        Reference r;
                        r.owner = Owner::Operation;
                        r.ownerId = opId.value.toString();
                        r.ownerName = op->second.name;
                        r.ownerType = op->second.type;
                        r.ownerOrder = opOrder;
                        r.setupName = s.name;
                        r.path = "geometry/" + std::to_string(g);
                        r.index = g;
                        r.of = geometry.size();
                        r.inList = true;
                        r.kind = geometry[g].kind;
                        r.body = geometry[g].body;
                        r.refJson = naming::toJson(geometry[g]);
                        evidenceOf(geometry[g], r);
                        resolveInto(*resolver, geometry[g], r);
                        keep(r);
                    }
                }
            }
        }
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::Internal, "Out of memory");
    }
}

Result<doc::Command> replaceCommand(const doc::DocState& state, const Reference& ref, std::span<const std::string> refs)
{
    auto fresh = parseRefs(refs, ref.kind);
    if (!fresh) {
        return tl::unexpected(fresh.error());
    }
    if (fresh->empty()) {
        return fail(ErrorCode::InvalidArgument, "Pick at least one element");
    }
    if (!ref.inList && fresh->size() != 1) {
        return fail(ErrorCode::InvalidArgument, "Pick exactly one element for this reference");
    }
    return replace(state, ref, std::move(*fresh));
}

Result<doc::Command> confirmCommand(const doc::DocState& state, const Reference& ref)
{
    if (!ref.confirmable()) {
        return fail(ErrorCode::InvalidArgument, "Only a likely match that is not confirmed yet can be confirmed");
    }
    auto stored = naming::topoRefFromJson(ref.refJson);
    if (!stored) {
        return fail(ErrorCode::InvalidArgument, "The listed reference could not be read");
    }
    stored->repairConfirmed = true;
    auto fresh = json::parse(naming::toJson(*stored));
    if (!fresh) {
        return fail(ErrorCode::Internal, "The confirmed reference could not be written");
    }
    std::vector<Value> one;
    one.push_back(std::move(*fresh));
    return replace(state, ref, std::move(one));
}

} // namespace ray::api::references
