// The "engrave" operation type (engrave.h, task T-056): the IOperationType, and the toolpath of a plan.
#include "engrave.h"
#include "op_params.h"

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

namespace ray::cam::ops {

namespace {

class EngraveOperation final : public IOperationType {
public:
    std::string_view typeName() const override { return "engrave"; }
    std::string_view paramsSchemaJson() const override { return embeddedOpSchema("engrave"); }

    Status validate(const GenerateInput& in) const override
    {
        JobContext ctx; // validate has no job
        auto plan = planEngrave(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return {};
    }

    Result<std::vector<std::string>> check(const GenerateInput& in) const override
    {
        JobContext ctx;
        auto plan = planEngrave(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return std::move(plan->warnings);
    }

    Result<Toolpath> generate(const GenerateInput& in, JobContext& ctx) const override
    {
        auto plan = planEngrave(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        try {
            Toolpath path = engraveToolpath(*plan);
            path.operation = in.op.id;
            ctx.progress.set(1.0);
            return path;
        } catch (const std::bad_alloc&) {
            return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while making the engraving toolpath", {}});
        }
    }
};

} // namespace

Toolpath engraveToolpath(const EngravePlan& plan)
{
    Toolpath path;
    const link::Linked& l = plan.linked;
    path.startX = l.start.x;
    path.startY = l.start.y;
    path.startZ = l.start.z;
    path.moves = l.moves;
    const link::Lengths lengths = link::lengthsOf(path.moves, l.start);
    path.stats.cutLengthMm = lengths.feed / kNmPerMm;
    path.stats.rapidLengthMm = lengths.rapid / kNmPerMm;
    Nm lo = l.start.z, hi = l.start.z;
    for (const Move& m : path.moves) {
        lo = std::min(lo, m.z);
        hi = std::max(hi, m.z);
    }
    path.stats.minZ = toMm(lo);
    path.stats.maxZ = toMm(hi);
    path.warnings = plan.warnings;
    return path;
}

std::unique_ptr<IOperationType> makeEngraveOperationType()
{
    return std::make_unique<EngraveOperation>();
}

} // namespace ray::cam::ops
