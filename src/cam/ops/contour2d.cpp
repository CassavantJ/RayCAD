// The "contour2d" operation type (contour2d.h, task T-053): the IOperationType, and the toolpath of a plan.
#include "contour2d.h"
#include "op_params.h"

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

namespace ray::cam::ops::contour2d {

namespace {

class Contour2dOperation final : public IOperationType {
public:
    std::string_view typeName() const override { return "contour2d"; }
    std::string_view paramsSchemaJson() const override { return embeddedOpSchema("contour2d"); }

    // The plan links the passes too (leads that do not fit, entries that cannot be made are refusals), so validate,
    // check and generate all run it and cannot disagree.
    Status validate(const GenerateInput& in) const override
    {
        JobContext ctx; // validate has no job; the kernel slices here are not cancellable
        auto plan = planContour2d(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return {};
    }

    Result<std::vector<std::string>> check(const GenerateInput& in) const override
    {
        JobContext ctx;
        auto plan = planContour2d(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return std::move(plan->warnings);
    }

    Result<Toolpath> generate(const GenerateInput& in, JobContext& ctx) const override
    {
        auto plan = planContour2d(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        try {
            Toolpath path = contour2dToolpath(*plan);
            path.operation = in.op.id;
            ctx.progress.set(1.0);
            return path;
        } catch (const std::bad_alloc&) {
            return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while making the 2D contour toolpath", {}});
        }
    }
};

} // namespace

Toolpath contour2dToolpath(const Contour2dPlan& plan)
{
    Toolpath path;
    path.startX = plan.startX;
    path.startY = plan.startY;
    path.startZ = plan.startZ;
    path.moves = plan.moves;
    const link::Lengths l =
        link::lengthsOf(std::span<const Move>(path.moves), link::Pt3{plan.startX, plan.startY, plan.startZ});
    path.stats.cutLengthMm = l.feed / kNmPerMm;
    path.stats.rapidLengthMm = l.rapid / kNmPerMm;
    Nm lo = plan.startZ, hi = plan.startZ;
    for (const Move& m : path.moves) {
        lo = std::min(lo, m.z);
        hi = std::max(hi, m.z);
    }
    path.stats.minZ = toMm(lo);
    path.stats.maxZ = toMm(hi);
    path.warnings = plan.warnings;
    return path;
}

std::unique_ptr<IOperationType> makeContour2dOperationType()
{
    return std::make_unique<Contour2dOperation>();
}

} // namespace ray::cam::ops::contour2d
