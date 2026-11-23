// The "pocket2d" operation type (pocket2d.h, task T-054): the IOperationType, and the toolpath of a plan (linking).
#include "pocket2d.h"
#include "op_params.h"

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

namespace ray::cam::ops {

namespace {

class Pocket2dOperation final : public IOperationType {
public:
    std::string_view typeName() const override { return "pocket2d"; }
    std::string_view paramsSchemaJson() const override { return embeddedOpSchema("pocket2d"); }

    // The checks up to the regions of every depth (pocket2d.h VALIDATE); validate has no job, so it cannot be
    // cancelled.
    Status validate(const GenerateInput& in) const override
    {
        JobContext ctx;
        auto plan = planPocket2d(in, ctx, PlanStage::Regions);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return {};
    }

    // The whole plan and the linking, so the panel sees a refusal from linking (no entry fits) and every warning.
    Result<std::vector<std::string>> check(const GenerateInput& in) const override
    {
        JobContext ctx;
        auto path = make(in, ctx);
        if (!path) {
            return tl::unexpected(path.error());
        }
        return std::move(path->warnings);
    }

    Result<Toolpath> generate(const GenerateInput& in, JobContext& ctx) const override
    {
        auto path = make(in, ctx);
        if (!path) {
            return tl::unexpected(path.error());
        }
        path->operation = in.op.id;
        ctx.progress.set(1.0);
        return path;
    }

private:
    static Result<Toolpath> make(const GenerateInput& in, JobContext& ctx)
    {
        Progress planning = ctx.progress.sub(0.6);
        JobContext planCtx{ctx.cancel, planning};
        auto plan = planPocket2d(in, planCtx, PlanStage::Full);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        Progress linking = ctx.progress.sub(0.4);
        JobContext linkCtx{ctx.cancel, linking};
        return pocket2dToolpath(*plan, linkCtx);
    }
};

} // namespace

Result<Toolpath> pocket2dToolpath(const Pocket2dPlan& plan, JobContext& ctx)
{
    try {
        auto linked = link::linkPasses(plan.passes, plan.options, plan.stock, ctx);
        if (!linked) {
            return tl::unexpected(linked.error());
        }
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
        path.warnings = plan.warnings;
        path.warnings.insert(path.warnings.end(), linked->warnings.begin(), linked->warnings.end());
        return path;
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while linking the 2D pocket", {}});
    }
}

std::unique_ptr<IOperationType> makePocket2dOperationType()
{
    return std::make_unique<Pocket2dOperation>();
}

} // namespace ray::cam::ops
