// The operation panel's read-only helpers (ray_api/cam_operation.h, task T-045).
#include "app_impl.h"
#include "json.h"

#include <ray_api/cam_operation.h>

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

namespace ray::api::cam_operation {

namespace {

tl::unexpected<Error> failWith(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

Vec3 apply(const Xform& xf, Vec3 p)
{
    const double* m = xf.m;
    return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

const cam::Setup* findSetup(const cam::CamState& cam, const SetupId& id)
{
    const auto it = std::find_if(cam.setups.begin(), cam.setups.end(), [&](const cam::Setup& s) { return s.id == id; });
    return it == cam.setups.end() ? nullptr : &*it;
}

constexpr std::pair<lib::ToolType, const char*> kToolTypes[] = {
    {lib::ToolType::FlatEndMill, "flatEndMill"}, {lib::ToolType::BallEndMill, "ballEndMill"},
    {lib::ToolType::BullEndMill, "bullEndMill"}, {lib::ToolType::ChamferMill, "chamferMill"},
    {lib::ToolType::FaceMill, "faceMill"},       {lib::ToolType::SpotDrill, "spotDrill"},
    {lib::ToolType::Drill, "drill"},             {lib::ToolType::Tap, "tap"},
    {lib::ToolType::Reamer, "reamer"},           {lib::ToolType::BoringBar, "boringBar"},
    {lib::ToolType::ThreadMill, "threadMill"},   {lib::ToolType::EngraveCutter, "engraveCutter"},
};

// The setup's frame and its snapshot, for the hole and backplot helpers.
struct SetupFrame {
    cam::CamSnapshot snap;
    const cam::Setup* setup = nullptr;
    cam::ResolvedSetup resolved;
};
Result<SetupFrame> frameOf(const doc::DocStatePtr& state, const doc::BuildResultPtr& build, const SetupId& id,
                           JobContext& ctx)
{
    if (!state || !build) {
        return failWith(ErrorCode::InvalidArgument, "The model is not built yet");
    }
    SetupFrame out;
    out.snap = doc::camSnapshot(state, build);
    out.setup = out.snap.cam ? findSetup(*out.snap.cam, id) : nullptr;
    if (out.setup == nullptr) {
        return failWith(ErrorCode::NotFound, "The document has no setup " + id.value.toString());
    }
    const auto resolver = naming::makeResolver(out.snap.bodies);
    if (!resolver) {
        return failWith(ErrorCode::Internal, "Out of memory");
    }
    auto resolved = cam::resolveSetup(out.snap, *out.setup, *resolver, ctx);
    if (!resolved) {
        return tl::unexpected(resolved.error());
    }
    out.resolved = std::move(*resolved);
    return out;
}

} // namespace

std::vector<TypeInfo> operationTypes(IApp& app)
{
    std::vector<TypeInfo> out;
    const cam::OperationRegistry* types = app.operationTypes();
    if (types == nullptr) {
        return out;
    }
    for (std::string_view name : types->types()) {
        const cam::IOperationType* type = types->find(name);
        if (type == nullptr) {
            continue;
        }
        TypeInfo info{std::string(name), std::string(name), std::string(type->paramsSchemaJson())};
        if (auto schema = json::parse(info.schemaJson); schema) {
            if (const json::Value* title = schema->find("title"); title != nullptr && title->isString() &&
                                                                  !title->asString().empty()) {
                info.title = title->asString();
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

Result<doc::Command> completeCommand(IApp& app, const doc::DocState& state, const doc::Command& command)
{
    try {
        return detail::checkOperationParams(app.operationTypes(), state, command);
    } catch (const std::bad_alloc&) {
        return failWith(ErrorCode::Internal, "Out of memory");
    }
}

Result<double> evaluate(const doc::DocState& state, std::string_view expression, doc::Parameter::Kind kind)
{
    try {
        auto params = doc::evaluateParameters(state.parameters);
        if (!params) {
            return tl::unexpected(params.error());
        }
        auto v = doc::evaluate(expression, *params, kind);
        if (!v) {
            return v;
        }
        if (kind == doc::Parameter::Kind::Angle) {
            return *v * 57.29577951308232; // radians to degrees, as the schemas store angles
        }
        return v;
    } catch (const std::bad_alloc&) {
        return failWith(ErrorCode::Internal, "Out of memory");
    }
}

std::string toolJson(const lib::Tool& tool)
{
    return lib::toJson(tool);
}

std::string holderJson(const lib::Holder& holder)
{
    return lib::toJson(holder);
}

std::string materialJson(const lib::Material& material)
{
    return lib::toJson(material);
}

std::string toolTypeName(lib::ToolType type)
{
    for (const auto& [t, name] : kToolTypes) {
        if (t == type) {
            return name;
        }
    }
    return {};
}

std::optional<lib::ToolType> toolTypeFromName(std::string_view name)
{
    for (const auto& [t, n] : kToolTypes) {
        if (name == n) {
            return t;
        }
    }
    return std::nullopt;
}

Result<cam::FeedsResult> computeFeeds(const FeedsRequest& r)
{
    try {
        const cam::FeedsInput in{r.tool,
                                 r.material,
                                 r.machine,
                                 r.radialEngagementMm,
                                 r.axialDepthMm,
                                 r.opType,
                                 r.opClass,
                                 r.surfaceSpeedMMin,
                                 r.chipLoadMm};
        return cam::computeFeeds(in);
    } catch (const std::bad_alloc&) {
        return failWith(ErrorCode::Internal, "Out of memory");
    }
}

Result<std::vector<Hole>> holesOf(const doc::DocStatePtr& state, const doc::BuildResultPtr& build, const SetupId& setup,
                                  JobContext& ctx)
{
    try {
        auto frame = frameOf(state, build, setup, ctx);
        if (!frame) {
            return tl::unexpected(frame.error());
        }
        // The part bodies with their element maps and the document's threads: what a generator gets, so the picker
        // finds exactly the holes the operation type will (cam.h holeOptionsFor).
        cam::ResolvedGeometry geom{};
        geom.wcsFromModel = frame->resolved.wcsFromModel;
        geom.holeThreads = frame->snap.holeThreads;
        std::vector<BodyId> bodies;
        for (const BodyId& id : frame->setup->partBodies) {
            const auto it = build->bodies.find(id);
            if (it == build->bodies.end()) {
                return failWith(ErrorCode::ReferenceBroken, "A part body of the setup is not built");
            }
            geom.partShapes.push_back(it->second.shape);
            geom.partNames.push_back(it->second.map);
            bodies.push_back(id);
        }
        std::vector<Hole> out;
        for (std::size_t i = 0; i < geom.partShapes.size(); ++i) {
            const cam::HoleOptions options = cam::holeOptionsFor(geom, i);
            auto holes = cam::recognizeHoles(geom.partShapes[i], geom.wcsFromModel, ctx, options);
            if (!holes) {
                return tl::unexpected(holes.error());
            }
            for (const cam::RecognizedHole& h : *holes) {
                Hole hole;
                hole.body = bodies[i];
                hole.boreFace = h.axisSub.index;
                for (const kernel::SubId& f : h.faces) {
                    hole.faces.push_back(f.index);
                }
                std::sort(hole.faces.begin(), hole.faces.end());
                hole.diameter = h.bore < h.segments.size() ? h.segments[h.bore].diameter : 0.0;
                hole.depth = toMm(h.zTop - h.zBottom);
                hole.through = h.through;
                hole.threadSpec = h.threadSpec;
                hole.x = toMm(h.x);
                hole.y = toMm(h.y);
                hole.top = apply(frame->resolved.modelFromWcs, Vec3{hole.x, hole.y, toMm(h.zTop)});
                out.push_back(std::move(hole));
            }
        }
        return out;
    } catch (const std::bad_alloc&) {
        return failWith(ErrorCode::Internal, "Out of memory");
    }
}

Result<std::shared_ptr<const sim::Backplot>> backplot(const doc::DocStatePtr& state, const doc::BuildResultPtr& build,
                                                      const SetupId& setup, const cam::Toolpath& toolpath,
                                                      JobContext& ctx)
{
    try {
        auto frame = frameOf(state, build, setup, ctx);
        if (!frame) {
            return tl::unexpected(frame.error());
        }
        const lib::MachineDef* machine = nullptr;
        if (const auto m = frame->snap.cam->machines.find(frame->setup->machine); m != frame->snap.cam->machines.end()) {
            machine = &m->second.item;
        }
        if (auto st = ctx.cancel.check(); !st) {
            return tl::unexpected(st.error());
        }
        return std::make_shared<const sim::Backplot>(
            sim::buildBackplot(toolpath, frame->resolved.modelFromWcs, 0.01, machine));
    } catch (const std::bad_alloc&) {
        return failWith(ErrorCode::Internal, "Out of memory");
    }
}

} // namespace ray::api::cam_operation
