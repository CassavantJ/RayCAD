// The "adaptive2d" operation type (op.h, task T-061): the IOperationType, and the toolpath of a plan (linking, stats).
#include "op.h"
#include "../op_params.h"
#include "../plunge_entry.h"

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

namespace ray::cam::ops {

namespace {

class Adaptive2dOperation final : public IOperationType {
public:
    std::string_view typeName() const override { return "adaptive2d"; }
    std::string_view paramsSchemaJson() const override { return embeddedOpSchema("adaptive2d"); }

    // The checks up to the areas of every depth (op.h VALIDATE); validate has no job, so it cannot be cancelled.
    Status validate(const GenerateInput& in) const override
    {
        JobContext ctx;
        auto plan = planAdaptive2d(in, ctx, Adaptive2dStage::Regions);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return {};
    }

    // The same checks and the warnings found by then (op.h VALIDATE: the engine is not run here).
    Result<std::vector<std::string>> check(const GenerateInput& in) const override
    {
        JobContext ctx;
        auto plan = planAdaptive2d(in, ctx, Adaptive2dStage::Regions);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return std::move(plan->warnings);
    }

    Result<Toolpath> generate(const GenerateInput& in, JobContext& ctx) const override
    {
        Progress planning = ctx.progress.sub(0.85);
        JobContext planCtx{ctx.cancel, planning};
        auto plan = planAdaptive2d(in, planCtx, Adaptive2dStage::Full);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        Progress linking = ctx.progress.sub(0.15);
        JobContext linkCtx{ctx.cancel, linking};
        auto path = adaptive2dToolpath(*plan, linkCtx);
        if (!path) {
            return tl::unexpected(path.error());
        }
        path->operation = in.op.id;
        ctx.progress.set(1.0);
        return path;
    }
};

} // namespace

Result<Toolpath> adaptive2dToolpath(const Adaptive2dPlan& plan, JobContext& ctx)
{
    try {
        auto linked = link::linkPasses(plan.passes, plan.options, plan.stock, ctx);
        if (!linked) {
            return tl::unexpected(linked.error());
        }
        // After M3: last-resort plunges (plunge_entry.h; op.h AT EACH DEPTH) and what follows them, read from the
        // linked moves before they are taken below.
        std::optional<std::string> plunged =
            plungeWarning(*linked, "There the area is cut at full width (a slot) along its walls, at the reduced feed, "
                                   "instead of at the set load.");
        Toolpath path;
        path.startX = linked->start.x;
        path.startY = linked->start.y;
        path.startZ = linked->start.z;
        path.moves = std::move(linked->moves);
        const link::Lengths lengths = link::lengthsOf(path.moves, linked->start);
        path.stats.cutLengthMm = lengths.feed / kNmPerMm;
        path.stats.rapidLengthMm = lengths.rapid / kNmPerMm;
        Nm minZ = linked->start.z, maxZ = linked->start.z;
        for (const Move& m : path.moves) {
            minZ = std::min(minZ, m.z);
            maxZ = std::max(maxZ, m.z);
        }
        path.stats.minZ = toMm(minZ);
        path.stats.maxZ = toMm(maxZ);
        path.stats.maxEngagementDeg = plan.maxEngagementDeg;
        path.stats.p99EngagementDeg = plan.p99EngagementDeg;
        path.stats.airCutLengthMm = plan.airCutLengthMm;
        path.warnings = plan.warnings;
        path.warnings.insert(path.warnings.end(), linked->warnings.begin(), linked->warnings.end());
        if (plunged) {
            path.warnings.push_back(std::move(*plunged));
        }
        return path;
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while linking the 2D adaptive roughing", {}});
    }
}

std::unique_ptr<IOperationType> makeAdaptive2dOperationType()
{
    return std::make_unique<Adaptive2dOperation>();
}

} // namespace ray::cam::ops
