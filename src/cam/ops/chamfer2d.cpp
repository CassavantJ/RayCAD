// The "chamfer2d" operation type (chamfer2d.h, task T-055): the IOperationType, and the toolpath of a plan through
// linking.
#include "chamfer2d.h"
#include "op_params.h"
#include "slot_common.h"

#include <exception>
#include <new>
#include <utility>

namespace ray::cam::ops {

namespace {

class Chamfer2dOperation final : public IOperationType {
public:
    std::string_view typeName() const override { return "chamfer2d"; }
    std::string_view paramsSchemaJson() const override { return embeddedOpSchema("chamfer2d"); }

    Status validate(const GenerateInput& in) const override
    {
        JobContext ctx; // validate has no job; the kernel work here is not cancellable
        auto plan = planChamfer(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return {};
    }

    Result<std::vector<std::string>> check(const GenerateInput& in) const override
    {
        JobContext ctx;
        auto plan = planChamfer(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return std::move(plan->warnings);
    }

    Result<Toolpath> generate(const GenerateInput& in, JobContext& ctx) const override
    {
        auto plan = planChamfer(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        ctx.progress.set(0.5);
        auto path = chamferToolpath(*plan, in.machine, ctx);
        if (!path) {
            return tl::unexpected(path.error());
        }
        path->operation = in.op.id;
        ctx.progress.set(1.0);
        return path;
    }
};

} // namespace

Result<Toolpath> chamferToolpath(const ChamferPlan& plan, const lib::MachineDef& machine, JobContext& ctx)
{
    try {
        auto linked = link::linkPasses(plan.passes, plan.options, plan.stock, ctx);
        if (!linked) {
            return tl::unexpected(linked.error());
        }
        Toolpath path;
        mill::fillStats(path, *linked);
        if (auto st = mill::checkTravel(path.moves, path.startZ, machine); !st) {
            return tl::unexpected(st.error());
        }
        path.warnings = plan.warnings;
        path.warnings.insert(path.warnings.end(), linked->warnings.begin(), linked->warnings.end());
        return path;
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while making the chamfer toolpath", {}});
    }
}

std::unique_ptr<IOperationType> makeChamfer2dOperationType()
{
    return std::make_unique<Chamfer2dOperation>();
}

} // namespace ray::cam::ops
